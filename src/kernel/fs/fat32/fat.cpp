// FAT32: cluster chains, the allocator and byte-range I/O (stage 3.3).
//
// Every access goes through bcache. The allocator starts from the FSInfo
// hint (next_free) and wraps at the end of the data area; when FSInfo is
// missing or invalid the hint is rebuilt once at mount by scanning the FAT
// in block-sized runs. All FAT copies (num_fats) are written identically.
//
// Chain walking is cached per vnode (last_index/last_cluster): sequential
// reads are O(1) per cluster instead of O(n) from the file start.

#include "../../../include/fs/fat32fs.h"
#include "../../../include/dev/bcache.h"
#include "../../../include/mm/heap.h"
#include "../../../include/mm/memory.h"
#include "../../../include/drivers/uart.h"
#include "../../../sdk/include/abi/errno.h"

namespace fat
{
    uint32_t cluster_lba(fat_super* sb, uint32_t cluster)
    {
        return sb->data_start + (cluster - 2) * sb->sectors_per_cluster;
    }

    // -----------------------------------------------------------------------
    // FAT entries
    // -----------------------------------------------------------------------

    sint64_t read_entry(fat_super* sb, uint32_t cluster, uint32_t* out)
    {
        // Cluster 1 is legal: it holds the volume flags (dirty bit).
        if (cluster < 1 || cluster > sb->total_clusters + 1)
            return -EINVAL;

        uint32_t byte_off  = cluster * 4;
        uint64_t sector    = sb->fat_start + byte_off / sb->bytes_per_sector;
        uint32_t in_sector = byte_off % sb->bytes_per_sector;

        sint64_t rc = 0;
        buf* b = bcache::get(sb->dev, sector, &rc);
        if (!b)
            return rc;

        uint32_t raw = *(const uint32_t*)(b->data + in_sector);
        bcache::put(b, false);

        *out = raw & 0x0FFFFFFF;
        return 0;
    }

    // Write one entry to every FAT copy. Each copy is an independent
    // read-modify-write through the cache; the top 4 bits are preserved.
    sint64_t write_entry(fat_super* sb, uint32_t cluster, uint32_t value)
    {
        if (cluster < 1 || cluster > sb->total_clusters + 1)
            return -EINVAL;

        uint32_t byte_off  = cluster * 4;
        uint32_t sec_in_fat = byte_off / sb->bytes_per_sector;
        uint32_t in_sector = byte_off % sb->bytes_per_sector;
        value &= 0x0FFFFFFF;

        for (uint32_t f = 0; f < sb->num_fats; f++)
        {
            uint64_t sector = sb->fat_start + (uint64_t)f * sb->fat_size + sec_in_fat;

            sint64_t rc = 0;
            buf* b = bcache::get(sb->dev, sector, &rc);
            if (!b)
                return rc;

            uint32_t* slot = (uint32_t*)(b->data + in_sector);
            *slot = (*slot & 0xF0000000u) | value;
            bcache::put(b, true);
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    // Chain walking
    // -----------------------------------------------------------------------

    sint64_t chain_advance(fat_super* sb, uint32_t* cluster, uint32_t steps)
    {
        while (steps--)
        {
            uint32_t next = 0;
            sint64_t rc = read_entry(sb, *cluster, &next);
            if (rc != 0)
                return rc;
            if (next >= FAT_CLUSTER_EOC)
                return -ENXIO;          // chain ended earlier than expected
            if (next == FAT_CLUSTER_FREE)
                return -EIO;            // hole in a live chain: corrupt
            if (next == FAT_CLUSTER_BAD)
                return -EIO;
            *cluster = next;
        }
        return 0;
    }

    // Cluster #index (0-based) of the file described by fn, using and
    // updating the vnode position cache.
    sint64_t cluster_at(fat_node* fn, uint32_t index, uint32_t* out)
    {
        fat_super* sb = fn->sb;

        if (fn->first_cluster < 2)
            return -ENXIO;              // empty file: no clusters at all

        uint32_t cluster;
        uint32_t steps;

        if (fn->last_cluster >= 2 && index >= fn->last_index &&
            index - fn->last_index < fn->sb->total_clusters)
        {
            // Forward from the cached position (the common sequential case).
            cluster = fn->last_cluster;
            steps   = index - fn->last_index;
        }
        else
        {
            // Random access: walk from the start. Still cached afterwards.
            cluster = fn->first_cluster;
            steps   = index;
        }

        sint64_t rc = chain_advance(sb, &cluster, steps);
        if (rc != 0)
            return rc;

        fn->last_index   = index;
        fn->last_cluster = cluster;
        *out = cluster;
        return 0;
    }

    // -----------------------------------------------------------------------
    // Allocator
    // -----------------------------------------------------------------------

    // Rebuild the hint by scanning the FAT once. Runs of entries fit inside
    // whole cached sectors, so this is sectors_per_FAT cache lookups worst
    // case - one time, at mount, only when FSInfo is unusable.
    static void scan_free_hint(fat_super* sb)
    {
        uint32_t free = 0;
        uint32_t first_free = 0xFFFFFFFF;
        uint32_t max_cluster = sb->total_clusters + 1;

        for (uint32_t c = 2; c <= max_cluster; c++)
        {
            uint32_t v = 0;
            if (read_entry(sb, c, &v) != 0)
                break;
            if (v == FAT_CLUSTER_FREE)
            {
                free++;
                if (first_free == 0xFFFFFFFF)
                    first_free = c;
            }
        }

        sb->free_count = free;
        sb->next_free  = (first_free != 0xFFFFFFFF) ? first_free : 2;
    }

    sint64_t alloc_cluster(fat_super* sb, uint32_t* out_cluster)
    {
        uint32_t max_cluster = sb->total_clusters + 1;
        uint32_t start = sb->next_free;
        if (start < 2 || start > max_cluster)
            start = 2;

        uint32_t c = start;
        uint32_t scanned = 0;
        bool found = false;

        // Search from the hint, wrapping once at the end of the data area.
        while (scanned < sb->total_clusters)
        {
            uint32_t v = 0;
            sint64_t rc = read_entry(sb, c, &v);
            if (rc != 0)
                return rc;

            if (v == FAT_CLUSTER_FREE)
            {
                found = true;
                break;
            }

            c++;
            if (c > max_cluster)
                c = 2;
            scanned++;
        }

        if (!found)
            return -ENOSPC;

        sint64_t rc = write_entry(sb, c, FAT_CLUSTER_EOC);
        if (rc != 0)
            return rc;

        if (sb->free_count != 0xFFFFFFFF && sb->free_count > 0)
            sb->free_count--;

        // Hint: continue after the cluster we just took.
        sb->next_free = c + 1;
        if (sb->next_free > max_cluster)
            sb->next_free = 2;

        *out_cluster = c;
        return 0;
    }

    sint64_t free_chain(fat_super* sb, uint32_t first_cluster)
    {
        uint32_t c = first_cluster;
        uint32_t guard = 0;

        while (c >= 2 && c < FAT_CLUSTER_EOC)
        {
            if (++guard > sb->total_clusters + 2)
                return -EIO;            // loop in the chain: corrupt FAT

            uint32_t next = 0;
            sint64_t rc = read_entry(sb, c, &next);
            if (rc != 0)
                return rc;

            rc = write_entry(sb, c, FAT_CLUSTER_FREE);
            if (rc != 0)
                return rc;

            if (sb->free_count != 0xFFFFFFFF)
                sb->free_count++;

            c = next;
        }
        return 0;
    }

    sint64_t chain_append(fat_node* fn, uint32_t newc)
    {
        fat_super* sb = fn->sb;

        if (fn->first_cluster < 2)
        {
            // First cluster of a previously empty file.
            fn->first_cluster = newc;
            fn->last_index    = 0;
            fn->last_cluster  = newc;
            return 0;
        }

        // Find the current end of the chain. Walk from the cached position
        // forward until EOC; the cache then points at the last cluster.
        uint32_t idx = fn->last_index;
        uint32_t c = fn->last_cluster >= 2 ? fn->last_cluster
                                           : fn->first_cluster;
        if (fn->last_cluster < 2)
            idx = 0;

        uint32_t guard = 0;
        for (;;)
        {
            uint32_t next = 0;
            sint64_t rc = read_entry(sb, c, &next);
            if (rc != 0)
                return rc;

            if (next >= FAT_CLUSTER_EOC)
                break;
            if (next < 2 || ++guard > sb->total_clusters + 2)
                return -EIO;            // corrupt chain
            c = next;
            idx++;
        }

        sint64_t rc = write_entry(sb, c, newc);
        if (rc != 0)
            return rc;

        fn->last_index   = idx + 1;
        fn->last_cluster = newc;
        return 0;
    }

    // -----------------------------------------------------------------------
    // Cluster I/O
    // -----------------------------------------------------------------------

    sint64_t read_cluster(fat_super* sb, uint32_t cluster, void* out)
    {
        if (cluster < 2 || cluster > sb->total_clusters + 1)
            return -EINVAL;

        uint64_t lba = cluster_lba(sb, cluster);
        struct buf** range = (struct buf**)kmalloc(sb->sectors_per_cluster * sizeof(buf*));
        if (!range)
            return -ENOMEM;

        sint64_t rc = bcache::get_range(sb->dev, lba, sb->sectors_per_cluster,
                                        range);
        if (rc == 0)
        {
            uint8_t* dst = (uint8_t*)out;
            for (uint32_t i = 0; i < sb->sectors_per_cluster; i++)
            {
                memory::memcpy(dst + (uint64_t)i * sb->bytes_per_sector,
                               range[i]->data, sb->bytes_per_sector);
                bcache::put(range[i], false);
            }
        }
        kfree(range);
        return rc;
    }

    sint64_t write_cluster(fat_super* sb, uint32_t cluster, const void* data)
    {
        if (cluster < 2 || cluster > sb->total_clusters + 1)
            return -EINVAL;

        uint64_t lba = cluster_lba(sb, cluster);
        struct buf** range = (struct buf**)kmalloc(sb->sectors_per_cluster * sizeof(buf*));
        if (!range)
            return -ENOMEM;

        sint64_t rc = bcache::get_range(sb->dev, lba, sb->sectors_per_cluster,
                                        range);
        if (rc == 0)
        {
            const uint8_t* src = (const uint8_t*)data;
            for (uint32_t i = 0; i < sb->sectors_per_cluster; i++)
            {
                memory::memcpy(range[i]->data,
                               src + (uint64_t)i * sb->bytes_per_sector,
                               sb->bytes_per_sector);
                bcache::put(range[i], true);
            }
        }
        else
        {
            kfree(range);
            return rc;
        }
        kfree(range);
        return 0;
    }

    // -----------------------------------------------------------------------
    // Byte-range I/O
    // -----------------------------------------------------------------------

    sint64_t read_at(vnode* v, fat_node* fn, uint64_t off, void* buf,
                     uint64_t len, uint64_t* done)
    {
        *done = 0;
        if (len == 0)
            return 0;
        if (off >= v->size)
            return 0;                       // EOF: a short (zero) read
        if (off + len > v->size)
            len = v->size - off;

        fat_super* sb = fn->sb;
        uint8_t* dst = (uint8_t*)buf;

        while (len)
        {
            uint32_t idx = (uint32_t)(off / sb->cluster_size);
            uint32_t in  = (uint32_t)(off % sb->cluster_size);

            uint32_t cluster = 0;
            sint64_t rc = cluster_at(fn, idx, &cluster);
            if (rc != 0)
                return rc == -ENXIO ? 0 : rc;   // sparse past EOF: stop

            uint32_t chunk = sb->cluster_size - in;
            if (chunk > len)
                chunk = (uint32_t)len;

            rc = read_cluster(sb, cluster, sb->scratch);
            if (rc != 0)
                return rc;
            memory::memcpy(dst, sb->scratch + in, chunk);

            dst += chunk;
            off += chunk;
            len -= chunk;
            *done += chunk;
        }
        return 0;
    }

    // Ensure the file has clusters covering byte offset `end_excl`,
    // zeroing any gap between the old EOF and the written range.
    static sint64_t extend_to(vnode* v, fat_node* fn, uint64_t end_excl)
    {
        fat_super* sb = fn->sb;
        uint64_t need_clusters = (end_excl + sb->cluster_size - 1) / sb->cluster_size;
        uint64_t have = v->size ? (v->size + sb->cluster_size - 1) / sb->cluster_size : 0;

        // A write into an existing cluster past EOF (a hole) must zero the
        // skipped bytes first, so a hole never leaks stale cluster data.
        if (have > 0 && end_excl > v->size)
        {
            uint32_t last_idx = (uint32_t)((v->size - 1) / sb->cluster_size);
            uint32_t in_last  = (uint32_t)(v->size % sb->cluster_size);
            uint32_t c = 0;
            sint64_t rc = cluster_at(fn, last_idx, &c);
            if (rc != 0 && rc != -ENXIO)
                return rc;
            if (rc == 0 && in_last != 0)
            {
                rc = read_cluster(sb, c, sb->scratch);
                if (rc != 0)
                    return rc;
                memory::memset(sb->scratch + in_last, 0,
                               sb->cluster_size - in_last);
                rc = write_cluster(sb, c, sb->scratch);
                if (rc != 0)
                    return rc;
            }
        }

        while (have < need_clusters)
        {
            uint32_t newc = 0;
            sint64_t rc = alloc_cluster(sb, &newc);
            if (rc != 0)
                return rc;

            // Fresh clusters start zeroed: FAT has no sparse files.
            memory::memset(sb->scratch, 0, sb->cluster_size);
            rc = write_cluster(sb, newc, sb->scratch);
            if (rc != 0)
            {
                // Keep the FAT consistent: the cluster is marked EOC and
                // unreferenced by the file now.
                free_chain(sb, newc);
                return rc;
            }

            rc = chain_append(fn, newc);
            if (rc != 0)
            {
                free_chain(sb, newc);
                return rc;
            }
            have++;
        }
        return 0;
    }

    sint64_t write_at(vnode* v, fat_node* fn, uint64_t off, const void* buf,
                      uint64_t len, uint64_t* done)
    {
        *done = 0;
        if (len == 0)
            return 0;

        fat_super* sb = fn->sb;

        // FAT's hard limit: file sizes are uint32.
        if (off + len > 0xFFFFFFFFULL)
            return -EFBIG;

        sint64_t rc = extend_to(v, fn, off + len);
        if (rc != 0)
            return rc;

        const uint8_t* src = (const uint8_t*)buf;

        while (len)
        {
            uint32_t idx = (uint32_t)(off / sb->cluster_size);
            uint32_t in  = (uint32_t)(off % sb->cluster_size);

            uint32_t cluster = 0;
            rc = cluster_at(fn, idx, &cluster);
            if (rc != 0)
                return rc;

            uint32_t chunk = sb->cluster_size - in;
            if (chunk > len)
                chunk = (uint32_t)len;

            // Partial cluster: read-modify-write so the untouched bytes of
            // the cluster survive.
            if (in != 0 || chunk != sb->cluster_size)
            {
                rc = read_cluster(sb, cluster, sb->scratch);
                if (rc != 0)
                    return rc;
                memory::memcpy(sb->scratch + in, src, chunk);
                rc = write_cluster(sb, cluster, sb->scratch);
            }
            else
            {
                // Whole cluster straight from the caller: copy into scratch
                // once so write_cluster's bcache scatter stays simple.
                memory::memcpy(sb->scratch, src, chunk);
                rc = write_cluster(sb, cluster, sb->scratch);
            }
            if (rc != 0)
                return rc;

            src += chunk;
            off += chunk;
            len -= chunk;
            *done += chunk;
        }

        if (off > v->size)
            v->size = off;
        vfs::touch(v, true);
        return 0;
    }

    sint64_t truncate(vnode* v, fat_node* fn, uint64_t size)
    {
        fat_super* sb = fn->sb;

        if (size > 0xFFFFFFFFULL)
            return -EFBIG;

        if (size == v->size)
            return 0;

        if (size > v->size)
        {
            // Grow with zeroes: extend the chain, zero the new tail.
            sint64_t rc = extend_to(v, fn, size);
            if (rc != 0)
                return rc;

            uint32_t in_last = (uint32_t)(v->size % sb->cluster_size);
            if (v->size > 0 && in_last != 0)
            {
                uint32_t idx = (uint32_t)((v->size - 1) / sb->cluster_size);
                uint32_t c = 0;
                rc = cluster_at(fn, idx, &c);
                if (rc == 0)
                {
                    rc = read_cluster(sb, c, sb->scratch);
                    if (rc == 0)
                    {
                        memory::memset(sb->scratch + in_last, 0,
                                       sb->cluster_size - in_last);
                        rc = write_cluster(sb, c, sb->scratch);
                    }
                }
                if (rc != 0 && rc != -ENXIO)
                    return rc;
            }
            v->size = size;
            vfs::touch(v, true);
            return 0;
        }

        // Shrink: free the cluster tail beyond the new size.
        uint64_t keep_clusters = size ? (size + sb->cluster_size - 1) / sb->cluster_size : 0;

        if (keep_clusters == 0)
        {
            if (fn->first_cluster >= 2)
            {
                sint64_t rc = free_chain(sb, fn->first_cluster);
                if (rc != 0)
                    return rc;
                fn->first_cluster = 0;
                fn->last_index = 0;
                fn->last_cluster = 0;
            }
        }
        else
        {
            uint32_t cut = 0;
            sint64_t rc = cluster_at(fn, (uint32_t)keep_clusters, &cut);
            if (rc != 0 && rc != -ENXIO)
                return rc;
            if (rc == 0 && cut >= 2 && cut < FAT_CLUSTER_EOC)
            {
                // Mark the kept cluster as the end, then free the rest.
                uint32_t idx = (uint32_t)(keep_clusters - 1);
                uint32_t keepc = 0;
                rc = cluster_at(fn, idx, &keepc);
                if (rc != 0)
                    return rc;

                rc = write_entry(sb, keepc, FAT_CLUSTER_EOC);
                if (rc != 0)
                    return rc;

                rc = free_chain(sb, cut);
                if (rc != 0)
                    return rc;

                fn->last_index = idx;
                fn->last_cluster = keepc;
            }
        }

        // Zero the tail of the new last cluster beyond the new size, so a
        // later extend does not resurrect old bytes.
        if (keep_clusters > 0 && size % sb->cluster_size)
        {
            uint32_t idx = (uint32_t)(keep_clusters - 1);
            uint32_t c = 0;
            sint64_t rc = cluster_at(fn, idx, &c);
            if (rc == 0)
            {
                rc = read_cluster(sb, c, sb->scratch);
                if (rc == 0)
                {
                    uint32_t in = (uint32_t)(size % sb->cluster_size);
                    memory::memset(sb->scratch + in, 0, sb->cluster_size - in);
                    rc = write_cluster(sb, c, sb->scratch);
                }
            }
            if (rc != 0)
                return rc;
        }

        v->size = size;
        vfs::touch(v, true);
        return 0;
    }

    // -----------------------------------------------------------------------
    // FSInfo and the volume dirty bit
    // -----------------------------------------------------------------------

    sint64_t flush_fsinfo(fat_super* sb)
    {
        if (!sb->fsinfo_valid || sb->fsinfo_sector == 0)
            return 0;

        sint64_t rc = 0;
        buf* b = bcache::get(sb->dev, sb->fsinfo_sector, &rc);
        if (!b)
            return rc;

        fat_fsinfo* fi = (fat_fsinfo*)b->data;
        if (fi->lead_sig == 0x41615252 && fi->struc_sig == 0x61417272)
        {
            fi->free_count = sb->free_count;
            fi->next_free  = sb->next_free;
            bcache::put(b, true);
            return 0;
        }

        bcache::put(b, false);
        return 0;       // FSInfo vanished: not fatal, the hint stays in RAM
    }

    void set_dirty_bit(fat_super* sb, bool dirty)
    {
        // FAT[1] holds the volume flags; bit 27 is "dirty".
        uint32_t v = 0;
        if (read_entry(sb, 1, &v) != 0)
            return;

        uint32_t want = dirty ? (v | FAT_VOL_DIRTY) : (v & ~FAT_VOL_DIRTY);
        if (want == v)
            return;

        if (write_entry(sb, 1, want) != 0)
            uart::printf("fat32: cannot %s the dirty bit\n",
                         dirty ? "set" : "clear");
    }

    // Mount-time FSInfo read; rebuilds the hint when unusable.
    sint64_t load_fsinfo(fat_super* sb)
    {
        sb->fsinfo_valid = false;
        sb->free_count = 0xFFFFFFFF;
        sb->next_free = 2;

        if (sb->fsinfo_sector == 0 || sb->fsinfo_sector >= sb->reserved_sectors)
        {
            scan_free_hint(sb);
            return 0;
        }

        sint64_t rc = 0;
        buf* b = bcache::get(sb->dev, sb->fsinfo_sector, &rc);
        if (!b)
        {
            scan_free_hint(sb);
            return rc;
        }

        // Copy the fields out while the buffer is locked.
        const fat_fsinfo* fi = (const fat_fsinfo*)b->data;
        bool sigs_ok = fi->lead_sig == 0x41615252 &&
                       fi->struc_sig == 0x61417272 &&
                       fi->trail_sig == 0xAA550000;
        uint32_t free_count = fi->free_count;
        uint32_t next_free = fi->next_free;
        bcache::put(b, false);

        if (!sigs_ok || free_count == 0xFFFFFFFF ||
            next_free < 2 || next_free > sb->total_clusters + 1)
        {
            // Invalid FSInfo: the hint must be rebuilt before the first
            // allocation, otherwise alloc_cluster scans from cluster 2 every
            // time on a nearly-full volume.
            scan_free_hint(sb);
            sb->fsinfo_valid = sigs_ok;  // signatures decide if we write back
            return 0;
        }

        sb->fsinfo_valid = true;
        sb->free_count = free_count;
        sb->next_free = next_free;
        return 0;
    }
}
