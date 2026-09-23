#include "../../include/cpu/uaccess.h"
#include "../../include/cpu/paging.h"
#include "../../include/cpu/features.h"

namespace
{
    // Validate that every 4 KiB page backing [addr, addr+len) is present,
    // user-accessible and - for writes - writable.
    bool check_range(uint64_t addr, uint64_t len, bool write)
    {
        if (len == 0)
            return true;

        if (!paging::is_user_range(addr, len))
            return false;

        uint64_t first = addr & ~(PAGE_SIZE_4K - 1);
        uint64_t last  = (addr + len - 1) & ~(PAGE_SIZE_4K - 1);

        for (uint64_t page = first; page <= last; page += PAGE_SIZE_4K)
        {
            if (!paging::user_page_accessible(page, write))
                return false;
        }
        return true;
    }

    inline void raw_copy(uint8_t* dst, const uint8_t* src, uint64_t len)
    {
        for (uint64_t i = 0; i < len; i++)
            dst[i] = src[i];
    }
}

namespace uaccess
{
    bool readable(uint64_t addr, uint64_t len) { return check_range(addr, len, false); }
    bool writable(uint64_t addr, uint64_t len) { return check_range(addr, len, true); }

    bool copy_from_user(void* dst, uint64_t user_src, uint64_t len)
    {
        if (!check_range(user_src, len, false))
            return false;

        cpu::user_access_begin();
        raw_copy((uint8_t*)dst, (const uint8_t*)user_src, len);
        cpu::user_access_end();
        return true;
    }

    bool copy_to_user(uint64_t user_dst, const void* src, uint64_t len)
    {
        if (!check_range(user_dst, len, true))
            return false;

        cpu::user_access_begin();
        raw_copy((uint8_t*)user_dst, (const uint8_t*)src, len);
        cpu::user_access_end();
        return true;
    }

    sint64_t strncpy_from_user(char* dst, uint64_t user_src, uint64_t max)
    {
        if (max == 0)
            return -1;

        dst[0] = '\0';

        // Walk page by page so that a string ending just before an unmapped
        // page still copies, instead of being rejected wholesale.
        uint64_t copied = 0;
        uint64_t limit = max - 1;

        while (copied < limit)
        {
            uint64_t addr = user_src + copied;

            if (!paging::is_user_range(addr, 1) ||
                !paging::user_page_accessible(addr & ~(PAGE_SIZE_4K - 1), false))
            {
                dst[copied] = '\0';
                return -1;
            }

            // Bytes left in this page, clamped to what the caller allows.
            uint64_t page_left = PAGE_SIZE_4K - (addr & (PAGE_SIZE_4K - 1));
            uint64_t chunk = limit - copied;
            if (chunk > page_left)
                chunk = page_left;

            cpu::user_access_begin();
            const char* src = (const char*)addr;
            uint64_t i = 0;
            for (; i < chunk; i++)
            {
                char c = src[i];
                dst[copied + i] = c;
                if (c == '\0')
                    break;
            }
            cpu::user_access_end();

            if (i < chunk)                  // hit the terminator
            {
                dst[copied + i] = '\0';
                return (sint64_t)(copied + i);
            }

            copied += chunk;
        }

        dst[limit] = '\0';
        return -2;                          // truncated
    }
}
