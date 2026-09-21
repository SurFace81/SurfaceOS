// FAT32 directory records: 8.3 names, LFN (VFAT) read and write,
// case handling (stage 3.3).
//
// Reading: one forward scanner walks the directory sector by sector through
// bcache, buffering the LFN slots it passes. When it hits the short entry
// the buffered run is validated (checksum + sequence) and decoded UCS-2 ->
// UTF-8; code points outside the BMP degrade to '?'. A record without LFN
// uses the short name, honouring the NT case flags.
//
// Writing: a name that is a valid uppercase 8.3 gets no LFN; a lowercase
// 8.3 gets the NT flags; anything else gets a BASENA~N alias plus the LFN
// run. The run is placed inside a single cluster (Windows does the same);
// the directory grows by a zeroed cluster when it runs out.
//
// Slot indices: the vnode key is the physical 32-byte slot number of the
// short entry (LFN slots count). Deletion marks slots 0xE5 in place and
// creation never moves existing records, so a slot number stays valid for
// the life of the record - exactly what the vnode cache needs.

#include "../../../include/fs/fat32fs.h"
#include "../../../include/dev/bcache.h"
#include "../../../include/mm/heap.h"
#include "../../../include/mm/memory.h"
#include "../../../include/stdlib/string.h"
#include "../../../include/drivers/uart.h"
#include "../../../sdk/include/abi/errno.h"

// The scanner below validates LFN runs with the short-name checksum.
namespace fatdir { uint8_t short_checksum(const uint8_t* name11); }

namespace
{
    const uint32_t LFN_CHARS_PER_ENTRY = 13;
    const uint32_t MAX_LFN_ENTRIES     = 20;    // ceil(255/13)

    inline uint32_t entries_per_cluster(fat_super* sb)
    {
        return sb->cluster_size / 32;
    }

    inline uint32_t entries_per_sector(fat_super* sb)
    {
        return sb->bytes_per_sector / 32;
    }

    char upper(char c)
    {
        return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }

    // Case-insensitive compare of two UTF-8 names (ASCII folding - what
    // every simple FAT driver does; fstest checks the ASCII cases).
    bool eq_nocase(const char* a, const char* b)
    {
        for (uint32_t i = 0; ; i++)
        {
            char ca = upper(a[i]);
            char cb = upper(b[i]);
            if (ca != cb)
                return false;
            if (ca == '\0')
                return true;
        }
    }

    // -----------------------------------------------------------------------
    // Name formatting
    // -----------------------------------------------------------------------

    // 11-byte short name -> display name, applying the NT case flags.
    void format_short(const fat_dir_entry* e, char* out)
    {
        uint32_t n = 0;
        bool base_lower = (e->nt_flags & FAT_NT_BASE_LOWER) != 0;
        bool ext_lower  = (e->nt_flags & FAT_NT_EXT_LOWER) != 0;

        for (uint32_t i = 0; i < 8 && e->name[i] != ' '; i++)
        {
            char c = (char)e->name[i];
            out[n++] = base_lower ? (char)(c >= 'A' && c <= 'Z' ? c + 32 : c) : c;
        }

        bool dot = false;
        for (uint32_t j = 8; j < 11 && e->name[j] != ' '; j++)
        {
            if (!dot)
            {
                out[n++] = '.';
                dot = true;
            }
            char c = (char)e->name[j];
            out[n++] = ext_lower ? (char)(c >= 'A' && c <= 'Z' ? c + 32 : c) : c;
        }
        out[n] = '\0';
    }

    uint32_t ucs2_to_utf8(const uint16_t* ucs2, uint32_t count,
                          char* out, uint32_t outmax)
    {
        uint32_t n = 0;
        for (uint32_t i = 0; i < count; i++)
        {
            uint16_t c = ucs2[i];
            if (c == 0x0000 || c == 0xFFFF)
                break;                      // terminator / padding
            if (n + 4 >= outmax)
                break;
            if (c >= 0xD800 && c <= 0xDFFF)
            {
                out[n++] = '?';             // surrogate: outside the BMP
                continue;
            }
            if (c < 0x80)
                out[n++] = (char)c;
            else if (c < 0x800)
            {
                out[n++] = (char)(0xC0 | (c >> 6));
                out[n++] = (char)(0x80 | (c & 0x3F));
            }
            else
            {
                out[n++] = (char)(0xE0 | (c >> 12));
                out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
                out[n++] = (char)(0x80 | (c & 0x3F));
            }
        }
        out[n] = '\0';
        return n;
    }

    uint32_t utf8_to_ucs2(const char* s, uint16_t* out, uint32_t outmax)
    {
        uint32_t n = 0;
        const uint8_t* p = (const uint8_t*)s;
        while (*p && n < outmax)
        {
            uint32_t c;
            if (*p < 0x80)
                c = *p++;
            else if ((*p & 0xE0) == 0xC0)
            {
                c = (uint32_t)(*p++ & 0x1F) << 6;
                if ((*p & 0xC0) != 0x80) { out[n++] = '?'; continue; }
                c |= (uint32_t)(*p++ & 0x3F);
            }
            else if ((*p & 0xF0) == 0xE0)
            {
                c = (uint32_t)(*p++ & 0x0F) << 12;
                if ((*p & 0xC0) != 0x80) { out[n++] = '?'; continue; }
                c |= (uint32_t)(*p++ & 0x3F) << 6;
                if ((*p & 0xC0) != 0x80) { out[n++] = '?'; continue; }
                c |= (uint32_t)(*p++ & 0x3F);
            }
            else
            {
                p++;                        // 4-byte sequence: skip, emit '?'
                while (*p && (*p & 0xC0) == 0x80) p++;
                out[n++] = '?';
                continue;
            }

            if (c >= 0xD800 && c <= 0xDFFF)
                c = '?';
            out[n++] = (uint16_t)c;
        }
        return n;
    }

    // -----------------------------------------------------------------------
    // The forward scanner
    // -----------------------------------------------------------------------
    //
    // Walks the directory from slot 0, one bcache sector at a time. LFN
    // slots are buffered as they pass; at each short entry the buffer is
    // validated against the checksum and handed to the callback. The
    // callback returns true to stop the walk.

    struct scan_ctx
    {
        uint16_t chars[MAX_LFN_ENTRIES][LFN_CHARS_PER_ENTRY];
        uint32_t count;             // slots in the run (its highest ordinal)
        uint32_t expect;            // next expected ordinal; 0 when complete
        bool     valid;             // run complete, pending checksum match
        uint8_t  checksum;
    };

    typedef bool (*scan_cb)(void* arg, const fat_dir_entry* e, uint32_t slot,
                            uint64_t lba, uint32_t byte_off,
                            const scan_ctx* lfn);

    void lfn_reset(scan_ctx* sc)
    {
        sc->count = 0;
        sc->expect = 0;
        sc->valid = false;
        sc->checksum = 0;
    }

    sint64_t scan_dir(fat_node* dir, scan_cb cb, void* arg)
    {
        fat_super* sb = dir->sb;
        scan_ctx sc;
        lfn_reset(&sc);

        uint32_t cluster = dir->first_cluster;
        if (cluster < 2)
            return -EINVAL;                 // directories always have one

        uint32_t slot = 0;

        for (;;)
        {
            for (uint32_t sec = 0; sec < sb->sectors_per_cluster; sec++)
            {
                uint64_t lba = fat::cluster_lba(sb, cluster) + sec;
                sint64_t rc = 0;
                buf* b = bcache::get(sb->dev, lba, &rc);
                if (!b)
                    return rc;

                const uint32_t eps = entries_per_sector(sb);
                bool stop = false;

                for (uint32_t i = 0; i < eps; i++, slot++)
                {
                    const fat_dir_entry* e =
                        (const fat_dir_entry*)(b->data + i * 32);

                    if (e->name[0] == FAT_DIR_END)
                    {
                        stop = true;
                        break;
                    }
                    if (e->name[0] == FAT_DIR_FREE)
                    {
                        lfn_reset(&sc);
                        continue;
                    }
                    if ((e->attr & FAT_ATTR_LFN) == FAT_ATTR_LFN)
                    {
                        const fat_lfn_entry* l = (const fat_lfn_entry*)e;
                        uint32_t ord = l->order & 0x3F;

                        if (l->order & FAT_DIR_LFN_ORD_LAST)
                        {
                            // A new run starts here (on disk it ends here).
                            lfn_reset(&sc);
                            sc.checksum = l->checksum;
                            sc.expect = ord;
                            sc.count = ord;
                        }

                        if (ord >= 1 && ord <= MAX_LFN_ENTRIES &&
                            l->checksum == sc.checksum && ord == sc.expect)
                        {
                            uint16_t* dst = sc.chars[ord - 1];
                            for (uint32_t k = 0; k < 5; k++) dst[k]     = l->name1[k];
                            for (uint32_t k = 0; k < 6; k++) dst[5 + k] = l->name2[k];
                            for (uint32_t k = 0; k < 2; k++) dst[11 + k] = l->name3[k];
                            sc.expect = ord - 1;
                            if (sc.expect == 0)
                                sc.valid = true;
                        }
                        else
                        {
                            // Sequence broke: discard the run.
                            lfn_reset(&sc);
                        }
                        continue;
                    }
                    if (e->attr & FAT_ATTR_VOLUME_ID)
                    {
                        lfn_reset(&sc);
                        continue;
                    }

                    // A short entry: validate the buffered run against the
                    // short name's checksum before handing it over.
                    const scan_ctx* use = nullptr;
                    if (sc.valid && sc.count > 0 &&
                        sc.checksum == fatdir::short_checksum(e->name))
                        use = &sc;

                    if (cb(arg, e, slot, lba, i * 32, use))
                    {
                        stop = true;
                        break;
                    }
                    lfn_reset(&sc);
                }

                bcache::put(b, false);
                if (stop)
                    return 0;
            }

            // Next cluster of the chain (position cache kept warm).
            uint32_t next = 0;
            sint64_t rc = fat::read_entry(sb, cluster, &next);
            if (rc != 0)
                return rc;
            if (next < 2 || next >= FAT_CLUSTER_EOC)
                return 0;                   // end of chain
            cluster = next;
        }
    }

    // Decode a validated LFN run into out (UTF-8).
    void lfn_decode(const scan_ctx* sc, char* out, uint32_t outmax)
    {
        uint32_t n = 0;
        char tmp[LFN_CHARS_PER_ENTRY * 3 + 1];
        for (uint32_t s = 0; s < sc->count; s++)
        {
            ucs2_to_utf8(sc->chars[s], LFN_CHARS_PER_ENTRY, tmp, sizeof(tmp));
            for (uint32_t j = 0; tmp[j] && n + 1 < outmax; j++)
                out[n++] = tmp[j];
        }
        out[n] = '\0';
    }
}

namespace fatdir
{
    // -----------------------------------------------------------------------
    // Time helpers
    // -----------------------------------------------------------------------

    void epoch_to_fat(uint64_t epoch, uint16_t* date, uint16_t* time)
    {
        if (epoch == 0)
        {
            *date = 0;
            *time = 0;
            return;
        }

        sint64_t days = (sint64_t)(epoch / 86400);
        uint32_t secs = (uint32_t)(epoch % 86400);

        // Days From Civil inverse (Howard Hinnant).
        sint64_t z = days + 719468;
        sint64_t era = (z >= 0 ? z : z - 146096) / 146097;
        uint32_t doe = (uint32_t)(z - era * 146097);
        uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        sint64_t y = (sint64_t)yoe + era * 400;
        uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        uint32_t mp = (5 * doy + 2) / 153;
        uint32_t d = doy - (153 * mp + 2) / 5 + 1;
        uint32_t m = mp + (mp < 10 ? 3u : (uint32_t)-9);
        y += (m <= 2);

        if (y < 1980)
        {
            *date = (uint16_t)((0 << 9) | (1 << 5) | 1);   // 1980-01-01
            *time = 0;
            return;
        }
        if (y > 2107)
            y = 2107;

        *date = (uint16_t)(((uint32_t)(y - 1980) << 9) | (m << 5) | d);
        *time = (uint16_t)(((secs / 3600) << 11) | ((secs / 60 % 60) << 5) |
                           ((secs % 60) / 2));
    }

    uint64_t fat_to_epoch(uint16_t date, uint16_t time)
    {
        if (date == 0)
            return 0;

        uint32_t y = 1980 + ((date >> 9) & 0x7F);
        uint32_t m = (date >> 5) & 0x0F;
        uint32_t d = date & 0x1F;
        if (m < 1 || m > 12 || d < 1 || d > 31)
            return 0;

        // Days From Civil (Howard Hinnant).
        y -= (m <= 2);
        sint64_t era = ((sint64_t)y >= 0 ? (sint64_t)y : (sint64_t)y - 399) / 400;
        uint32_t yoe = (uint32_t)((sint64_t)y - era * 400);
        uint32_t doy = (153 * (m + (m > 2 ? (uint32_t)-3 : 9u)) + 2) / 5 + d - 1;
        uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        sint64_t days = era * 146097LL + (sint64_t)doe - 719468LL;

        uint32_t h = (time >> 11) & 0x1F;
        uint32_t mi = (time >> 5) & 0x3F;
        uint32_t s2 = (time & 0x1F) * 2;

        return (uint64_t)(days * 86400LL + (sint64_t)h * 3600 +
                          (sint64_t)mi * 60 + (sint64_t)s2);
    }

    // -----------------------------------------------------------------------
    // Name helpers
    // -----------------------------------------------------------------------

    bool valid_name(const char* name)
    {
        if (!name || !name[0])
            return false;

        uint32_t len = 0;
        for (const char* p = name; *p; p++)
        {
            uint8_t c = (uint8_t)*p;
            if (c < 0x20 || c == 0x7F)
                return false;
            switch (c)
            {
                case '"': case '*': case '/': case ':':
                case '<': case '>': case '?': case '\\': case '|':
                    return false;
                default:
                    break;
            }
            len++;
        }
        return len <= NAME_MAX;
    }

    uint8_t short_checksum(const uint8_t* name11)
    {
        uint8_t sum = 0;
        for (uint32_t i = 0; i < 11; i++)
            sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + name11[i]);
        return sum;
    }

    uint32_t ucs2_to_utf8_name(const uint16_t* ucs2, uint32_t count,
                               char* out, uint32_t outmax)
    {
        return ucs2_to_utf8(ucs2, count, out, outmax);
    }

    void format_short_name(const fat_dir_entry* e, char* out)
    {
        format_short(e, out);
    }

    void make_short_name(const char* name, uint8_t* out11, bool* needs_lfn,
                         uint8_t* nt_flags)
    {
        memory::memset(out11, ' ', 11);
        *nt_flags = 0;
        *needs_lfn = false;

        uint32_t len = 0;
        while (name[len])
            len++;

        // Base/ext split at the LAST dot; a leading dot belongs to the base.
        uint32_t dot = 0xFFFFFFFF;
        for (uint32_t i = 1; i < len; i++)
            if (name[i] == '.')
                dot = i;

        const char* base = name;
        uint32_t blen = (dot == 0xFFFFFFFF) ? len : dot;
        const char* ext = (dot == 0xFFFFFFFF) ? nullptr : name + dot + 1;
        uint32_t elen = (dot == 0xFFFFFFFF) ? 0 : len - dot - 1;

        bool base_lower = false, base_upper = false;
        for (uint32_t i = 0; i < blen; i++)
        {
            if (base[i] >= 'a' && base[i] <= 'z') base_lower = true;
            if (base[i] >= 'A' && base[i] <= 'Z') base_upper = true;
        }
        bool ext_lower = false, ext_upper = false;
        for (uint32_t i = 0; ext && i < elen; i++)
        {
            if (ext[i] >= 'a' && ext[i] <= 'z') ext_lower = true;
            if (ext[i] >= 'A' && ext[i] <= 'Z') ext_upper = true;
        }

        // Expressible as 8.3 with a single case per half? No LFN needed;
        // all-lowercase is stored uppercase + NT flags.
        if (blen >= 1 && blen <= 8 && elen <= 3 && !base_upper && !ext_upper)
        {
            bool legal = true;
            for (uint32_t i = 0; i < blen; i++)
            {
                char c = base[i];
                if (c == ' ' || c == '.')
                    legal = false;
            }
            for (uint32_t i = 0; ext && i < elen; i++)
                if (ext[i] == ' ' || ext[i] == '.')
                    legal = false;

            if (legal)
            {
                for (uint32_t i = 0; i < blen; i++)
                    out11[i] = (uint8_t)upper(base[i]);
                for (uint32_t i = 0; ext && i < elen; i++)
                    out11[8 + i] = (uint8_t)upper(ext[i]);
                if (base_lower)
                    *nt_flags |= FAT_NT_BASE_LOWER;
                if (ext_lower)
                    *nt_flags |= FAT_NT_EXT_LOWER;
                return;
            }
        }

        *needs_lfn = true;

        // Alias: uppercase; spaces, dots, '~' to '_'.
        uint32_t n = 0;
        for (uint32_t i = 0; i < blen && n < 8; i++)
        {
            char c = upper(base[i]);
            if (c == ' ' || c == '.' || c == '~')
                c = '_';
            out11[n++] = (uint8_t)c;
        }
        if (out11[0] == 0xE5)
            out11[0] = '_';
        n = 0;
        for (uint32_t i = 0; ext && i < elen && n < 3; i++)
        {
            char c = upper(ext[i]);
            if (c == ' ' || c == '.' || c == '~')
                c = '_';
            out11[8 + n++] = (uint8_t)c;
        }
    }

    // -----------------------------------------------------------------------
    // Slot addressing
    // -----------------------------------------------------------------------

    sint64_t slot_location(fat_node* dir, uint32_t index,
                           uint64_t* lba, uint32_t* byte_off)
    {
        fat_super* sb = dir->sb;
        const uint32_t epc = entries_per_cluster(sb);

        uint32_t cluster = 0;
        sint64_t rc = fat::cluster_at(dir, index / epc, &cluster);
        if (rc != 0)
            return rc;

        uint32_t byte = (index % epc) * 32;
        *lba = fat::cluster_lba(sb, cluster) + byte / sb->bytes_per_sector;
        *byte_off = byte % sb->bytes_per_sector;
        return 0;
    }

    // -----------------------------------------------------------------------
    // Iteration
    // -----------------------------------------------------------------------

    struct next_record_ctx
    {
        uint64_t from_slot;       // resume here
        fat_dirent* out;
        uint32_t* out_index;
        uint64_t* out_next;
        bool* eof;
        bool done;
    };

    bool next_record_cb(void* arg, const fat_dir_entry* e, uint32_t slot,
                        uint64_t lba, uint32_t byte_off, const scan_ctx* lfn)
    {
        next_record_ctx* c = (next_record_ctx*)arg;
        (void)lba; (void)byte_off;

        if (slot < c->from_slot)
            return false;               // keep walking

        c->out->e = *e;
        if (lfn)
        {
            lfn_decode(lfn, c->out->lfn, sizeof(c->out->lfn));
            c->out->has_lfn = (c->out->lfn[0] != '\0');
        }
        else
        {
            format_short(e, c->out->lfn);
            c->out->has_lfn = false;
        }

        *c->out_index = slot;
        *c->out_next = (uint64_t)slot + 1;
        *c->eof = false;
        c->done = true;
        return true;                    // stop: one record per call
    }

    sint64_t next_record(fat_node* dir, uint64_t cookie,
                         fat_dirent* out, uint32_t* out_index,
                         uint64_t* out_next_cookie, bool* eof)
    {
        memory::memset((uint8_t*)out, 0, sizeof(fat_dirent));
        *eof = true;

        next_record_ctx c;
        c.from_slot = cookie;
        c.out = out;
        c.out_index = out_index;
        c.out_next = out_next_cookie;
        c.eof = eof;
        c.done = false;

        sint64_t rc = scan_dir(dir, next_record_cb, &c);
        if (rc != 0)
            return rc;

        if (!c.done)
        {
            *out_next_cookie = cookie;
            *eof = true;
        }
        return 0;
    }

    struct find_ctx
    {
        const char* name;
        fat_dirent* out;
        uint32_t* out_index;
        uint64_t* out_lba;
        uint32_t* out_off;
        bool* found;
    };

    bool find_cb(void* arg, const fat_dir_entry* e, uint32_t slot,
                 uint64_t lba, uint32_t byte_off, const scan_ctx* lfn)
    {
        find_ctx* c = (find_ctx*)arg;

        // Display name: the decoded LFN when present, else the short name.
        char name[NAME_MAX + 1];
        if (lfn)
            lfn_decode(lfn, name, sizeof(name));
        else
            format_short(e, name);

        bool match = eq_nocase(name, c->name);
        if (!match && lfn)
        {
            // Records with an LFN are also reachable by their alias
            // ("FILENA~1"): compare the uppercase short form.
            char shortname[13];
            format_short(e, shortname);
            match = eq_nocase(shortname, c->name);
        }
        if (!match)
            return false;

        c->out->e = *e;
        strncpy(c->out->lfn, name, sizeof(c->out->lfn) - 1);
        c->out->lfn[sizeof(c->out->lfn) - 1] = '\0';
        c->out->has_lfn = (lfn != nullptr);

        *c->out_index = slot;
        *c->out_lba = lba;
        *c->out_off = byte_off;
        *c->found = true;
        return true;
    }

    sint64_t find(fat_node* dir, const char* name,
                  fat_dirent* out, uint32_t* out_index, bool* found,
                  uint64_t* out_entry_lba, uint32_t* out_entry_off)
    {
        *found = false;
        memory::memset((uint8_t*)out, 0, sizeof(fat_dirent));

        // When the caller does not need the location, point the context at
        // throwaway storage instead of branching in the callback.
        uint64_t dummy_lba = 0;
        uint32_t dummy_off = 0;

        find_ctx c;
        c.name = name;
        c.out = out;
        c.out_index = out_index;
        c.out_lba = out_entry_lba ? out_entry_lba : &dummy_lba;
        c.out_off = out_entry_off ? out_entry_off : &dummy_off;
        c.found = found;

        return scan_dir(dir, find_cb, &c);
    }

    // -----------------------------------------------------------------------
    // Writing
    // -----------------------------------------------------------------------

    // Grow the directory by one zeroed cluster linked after the last.
    static sint64_t grow_dir(fat_node* dir)
    {
        fat_super* sb = dir->sb;

        uint32_t last = dir->first_cluster;
        uint32_t guard = 0;
        for (;;)
        {
            uint32_t next = 0;
            sint64_t rc = fat::read_entry(sb, last, &next);
            if (rc != 0)
                return rc;
            if (next < 2 || next >= FAT_CLUSTER_EOC)
                break;
            last = next;
            if (++guard > sb->total_clusters + 2)
                return -EIO;                // chain loop: corrupt FAT
        }

        uint32_t newc = 0;
        sint64_t rc = fat::alloc_cluster(sb, &newc);
        if (rc != 0)
            return rc;

        memory::memset(sb->scratch, 0, sb->cluster_size);
        rc = fat::write_cluster(sb, newc, sb->scratch);
        if (rc != 0)
        {
            fat::write_entry(sb, newc, FAT_CLUSTER_FREE);
            return rc;
        }
        rc = fat::write_entry(sb, last, newc);
        if (rc != 0)
        {
            fat::write_entry(sb, newc, FAT_CLUSTER_FREE);
            return rc;
        }
        // alloc_cluster already marked newc as EOC.
        dir->last_index = 0xFFFFFFFF;       // chain changed: reset the cache
        dir->last_cluster = 0;
        return 0;
    }

    // Find `need` contiguous free slots; grow the directory as needed.
    // Runs may cross sector and cluster boundaries (the reader buffers LFN
    // slots in physical slot order, the writer resolves each slot through
    // slot_location) - only physical adjacency matters.
    // *out_slot is the physical index of the first slot of the run.
    static sint64_t reserve_run(fat_node* dir, uint32_t need, uint32_t* out_slot)
    {
        fat_super* sb = dir->sb;

        for (;;)
        {
            uint32_t cluster = dir->first_cluster;
            uint32_t slot = 0;
            uint32_t run = 0, run_start = 0;
            bool end_seen = false;

            for (;;)
            {
                for (uint32_t sec = 0; sec < sb->sectors_per_cluster && !end_seen; sec++)
                {
                    uint64_t lba = fat::cluster_lba(sb, cluster) + sec;
                    sint64_t rc = 0;
                    buf* b = bcache::get(sb->dev, lba, &rc);
                    if (!b)
                        return rc;

                    const uint32_t eps = entries_per_sector(sb);

                    for (uint32_t i = 0; i < eps; i++, slot++)
                    {
                        uint8_t first = b->data[i * 32];
                        if (first == FAT_DIR_FREE || first == FAT_DIR_END)
                        {
                            if (first == FAT_DIR_END)
                                end_seen = true;
                            if (run == 0)
                                run_start = slot;
                            run++;
                            if (run >= need)
                            {
                                bcache::put(b, false);
                                *out_slot = run_start;
                                return 0;
                            }
                            // end_seen with an incomplete run: fall through
                            // to grow_dir, then restart the scan (the run
                            // may continue into the fresh cluster).
                        }
                        else
                        {
                            run = 0;
                        }
                    }
                    bcache::put(b, false);
                }

                if (end_seen)
                    break;

                uint32_t next = 0;
                sint64_t rc = fat::read_entry(sb, cluster, &next);
                if (rc != 0)
                    return rc;
                if (next < 2 || next >= FAT_CLUSTER_EOC)
                    break;                  // chain end without 0x00: grow
                cluster = next;
            }

            // No room: grow and retry (bounded by ENOSPC from the allocator).
            sint64_t rc = grow_dir(dir);
            if (rc != 0)
                return rc;
        }
    }

    // Does an existing short entry hold exactly this 11-byte name?
    struct exists_ctx
    {
        const uint8_t* name11;
        bool found;
    };

    bool exists_cb(void* arg, const fat_dir_entry* e, uint32_t, uint64_t,
                   uint32_t, const scan_ctx*)
    {
        exists_ctx* c = (exists_ctx*)arg;
        if (memory::memcmp(e->name, c->name11, 11) == 0)
        {
            c->found = true;
            return true;
        }
        return false;
    }

    static sint64_t exists_short(fat_node* dir, const uint8_t* name11, bool* out)
    {
        exists_ctx c;
        c.name11 = name11;
        c.found = false;
        sint64_t rc = scan_dir(dir, exists_cb, &c);
        *out = c.found;
        return rc;
    }

    sint64_t create(fat_node* dir, const char* name,
                    bool is_dir, uint32_t first_cluster,
                    uint32_t* out_index,
                    uint64_t* out_entry_lba, uint32_t* out_entry_off)
    {
        fat_super* sb = dir->sb;

        if (!valid_name(name))
            return -EINVAL;

        uint16_t u16[NAME_MAX + 1];
        uint32_t ucount = utf8_to_ucs2(name, u16, NAME_MAX);

        uint8_t alias[11];
        bool needs_lfn = false;
        uint8_t nt_flags = 0;
        make_short_name(name, alias, &needs_lfn, &nt_flags);

        uint8_t short_name[11];
        memory::memcpy(short_name, alias, 11);

        if (needs_lfn)
        {
            // BASENA~N: smallest N whose alias is free.
            for (uint32_t n = 1; ; n++)
            {
                char suffix[8];
                uint32_t sl = 0;
                suffix[sl++] = '~';
                char digits[7];
                uint32_t dl = 0, v = n;
                while (v)
                {
                    digits[dl++] = (char)('0' + v % 10);
                    v /= 10;
                }
                while (dl)
                    suffix[sl++] = digits[--dl];

                if (sl > 7)
                    return -ENAMETOOLONG;   // cannot fit any suffix

                uint32_t keep = 8 - sl;
                memory::memset(short_name, ' ', 11);
                uint32_t bn = 0;
                for (uint32_t i = 0; i < 8 && alias[i] != ' ' && bn < keep; i++)
                    short_name[bn++] = alias[i];
                while (bn > 0 && short_name[bn - 1] == ' ')
                    bn--;
                for (uint32_t i = 0; i < sl; i++)
                    short_name[bn++] = (uint8_t)suffix[i];
                for (uint32_t i = 0; i < 3; i++)
                    short_name[8 + i] = alias[8 + i];

                bool taken = false;
                sint64_t rc = exists_short(dir, short_name, &taken);
                if (rc != 0)
                    return rc;
                if (!taken)
                    break;
                if (n == 999999)
                    return -EEXIST;
            }
        }
        else
        {
            bool taken = false;
            sint64_t rc = exists_short(dir, short_name, &taken);
            if (rc != 0)
                return rc;
            if (taken)
                return -EEXIST;
        }

        // LFN slots + the short entry, one contiguous run.
        uint32_t lfn_slots = 0;
        if (needs_lfn)
            lfn_slots = (ucount + LFN_CHARS_PER_ENTRY) / LFN_CHARS_PER_ENTRY;
        if (lfn_slots > MAX_LFN_ENTRIES)
            return -ENAMETOOLONG;

        uint32_t run_start = 0;
        sint64_t rc = reserve_run(dir, lfn_slots + 1, &run_start);
        if (rc != 0)
            return rc;

        uint8_t sum = short_checksum(short_name);

        for (uint32_t s = lfn_slots; s >= 1; s--)
        {
            uint32_t slot = run_start + (s - 1);

            fat_lfn_entry l;
            memory::memset((uint8_t*)&l, 0, sizeof(l));
            l.order = (uint8_t)(s | (s == lfn_slots ? FAT_DIR_LFN_ORD_LAST : 0));
            l.attr = FAT_ATTR_LFN;
            l.type = 0;
            l.checksum = sum;
            l.first_cluster = 0;

            uint32_t cbase = (s - 1) * LFN_CHARS_PER_ENTRY;
            for (uint32_t i = 0; i < LFN_CHARS_PER_ENTRY; i++)
            {
                uint32_t ci = cbase + i;
                uint16_t ch;
                if (ci < ucount)        ch = u16[ci];
                else if (ci == ucount)  ch = 0x0000;
                else                    ch = 0xFFFF;

                if (i < 5)        l.name1[i] = ch;
                else if (i < 11)  l.name2[i - 5] = ch;
                else              l.name3[i - 11] = ch;
            }

            uint64_t lba = 0;
            uint32_t off = 0;
            rc = slot_location(dir, slot, &lba, &off);
            if (rc != 0)
                return rc;

            buf* b = bcache::get(sb->dev, lba, &rc);
            if (!b)
                return rc;
            memory::memcpy(b->data + off, (const uint8_t*)&l, 32);
            bcache::put(b, true);
        }

        fat_dir_entry e;
        memory::memset((uint8_t*)&e, 0, sizeof(e));
        memory::memcpy(e.name, short_name, 11);
        e.attr = FAT_ATTR_ARCHIVE | (is_dir ? FAT_ATTR_DIRECTORY : 0);
        e.nt_flags = needs_lfn ? 0 : nt_flags;

        uint16_t date = 0, time = 0;
        epoch_to_fat(vfs::now_epoch(), &date, &time);
        e.create_time = time;
        e.create_date = date;
        e.write_time = time;
        e.write_date = date;
        e.last_access_date = date;

        e.first_cluster_hi = (uint16_t)(first_cluster >> 16);
        e.first_cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
        e.file_size = 0;

        uint32_t short_slot = run_start + lfn_slots;

        uint64_t lba = 0;
        uint32_t off = 0;
        rc = slot_location(dir, short_slot, &lba, &off);
        if (rc != 0)
            return rc;

        buf* b = bcache::get(sb->dev, lba, &rc);
        if (!b)
            return rc;
        memory::memcpy(b->data + off, (const uint8_t*)&e, 32);
        bcache::put(b, true);

        *out_index = short_slot;
        if (out_entry_lba)
            *out_entry_lba = lba;
        if (out_entry_off)
            *out_entry_off = off;
        return 0;
    }

    sint64_t remove(fat_node* dir, uint32_t index)
    {
        fat_super* sb = dir->sb;

        uint64_t lba = 0;
        uint32_t off = 0;
        sint64_t rc = slot_location(dir, index, &lba, &off);
        if (rc != 0)
            return rc;

        buf* b = bcache::get(sb->dev, lba, &rc);
        if (!b)
            return rc;

        fat_dir_entry* e = (fat_dir_entry*)(b->data + off);
        if (e->name[0] == FAT_DIR_FREE || e->name[0] == FAT_DIR_END)
        {
            bcache::put(b, false);
            return -ENOENT;
        }
        uint8_t sum = short_checksum(e->name);
        e->name[0] = FAT_DIR_FREE;
        bcache::put(b, true);

        // Mark the preceding LFN run free too, while the checksum matches.
        while (index >= 1)
        {
            uint64_t plba = 0;
            uint32_t poff = 0;
            rc = slot_location(dir, index - 1, &plba, &poff);
            if (rc != 0)
                break;

            b = bcache::get(sb->dev, plba, &rc);
            if (!b)
                break;

            fat_dir_entry* raw = (fat_dir_entry*)(b->data + poff);
            bool is_lfn = (raw->attr & FAT_ATTR_LFN) == FAT_ATTR_LFN &&
                          raw->name[0] != FAT_DIR_FREE;
            const fat_lfn_entry* l = (const fat_lfn_entry*)raw;
            bool match = is_lfn && l->checksum == sum;
            if (match)
                raw->name[0] = FAT_DIR_FREE;
            bcache::put(b, match);
            if (!match)
                break;
            index--;
        }
        return 0;
    }

    sint64_t update_entry(vnode* v, fat_node* fn)
    {
        fat_super* sb = fn->sb;

        if (fn->entry_index == 0xFFFFFFFF)
            return 0;               // the root directory has no entry

        sint64_t rc = 0;
        buf* b = bcache::get(sb->dev, fn->entry_lba, &rc);
        if (!b)
            return rc;

        fat_dir_entry* e = (fat_dir_entry*)(b->data + fn->entry_off);
        e->file_size = (uint32_t)v->size;
        e->first_cluster_hi = (uint16_t)(fn->first_cluster >> 16);
        e->first_cluster_lo = (uint16_t)(fn->first_cluster & 0xFFFF);

        uint16_t date = 0, time = 0;
        epoch_to_fat(v->mtime ? v->mtime : vfs::now_epoch(), &date, &time);
        e->write_date = date;
        e->write_time = time;
        if (v->atime)
        {
            uint16_t adate = 0, atime = 0;
            epoch_to_fat(v->atime, &adate, &atime);
            e->last_access_date = adate;
        }

        bcache::put(b, true);
        v->flags &= (uint16_t)~VF_DIRTY;
        return 0;
    }
}
