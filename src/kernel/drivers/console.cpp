#include "../../include/drivers/console.h"

namespace {
    UINT32 cursor_x = 0;
    UINT32 cursor_y = 0;
    UINT32 max_cols = 0;
    UINT32 max_rows = 0;

    list::List<char>* INPUT_BUFFER;
    list::List<char>* cpuinfo;
    char cpu_name[51];
}

namespace commands {
    const char* parse_and_exec(list::List<char>* input);
}

namespace console {
    static void input(keyboard_event_t e);

    void init(void) {
        INPUT_BUFFER = list::create<char>();
        cpuinfo = list::create<char>();
        max_cols = Screen.Width / Screen.SymbolSizeX;
        max_rows = Screen.Height / Screen.SymbolSizeY;

        keyboard::set_keyboard_callback(input);

        print("\n\tSurfaceOS v0.1 (C) 2025\n\r\tMem: ");
        print((int)(memory::getMemorySize() / 1048576 + 1));
        print(" Mb\n\r\tCpu: ");
        cpuid::get_cpu_name(cpu_name);
        print(cpu_name);
        print(" @ ");
        char freq[12];
        int_to_str(cpuid::get_base_freq(), freq);
        print(freq);
        print("MHz\n\r------------------------------------------------\n\n\r> ");
        
        cursor_x = 2; cursor_y = 6;
    };

    static void input(keyboard_event_t e) {
        if (e.type != KEY_PRESS) return;

        if (e.KeyCode == Keys::BACKSPACE) {
            if (cursor_x >= 1) {
                cursor_x -= 1;
            } else if (cursor_y >= 1) {
                cursor_x = max_cols - 1;
                cursor_y -= 1;
            }
            list::remove_at(INPUT_BUFFER, list::size(INPUT_BUFFER) - 1);
            screen::backspace(cursor_x, cursor_y);
        }
        else if (e.KeyCode == Keys::ENTER) {
            print(commands::parse_and_exec(INPUT_BUFFER));
            list::clear(INPUT_BUFFER);
            print("\n\r> ");
            cursor_x = 2;
            cursor_y += 1;
        // } else if (e.KeyCode == Keys::ARROW_DOWN) {
        //     if (cursor_y < max_rows - 1)
        //         cursor_y += 1;
        // } else if (e.KeyCode == Keys::ARROW_UP) {
        //     if (cursor_y > 0)
        //         cursor_y -= 1;
        // } else if (e.KeyCode == Keys::ARROW_LEFT) {
        //     if (cursor_x > 0)
        //         cursor_x -= 1;
        // } else if (e.KeyCode == Keys::ARROW_RIGHT) {
        //     if (cursor_x < max_cols)
        //         cursor_x += 1;
        } else {
            if (!e.ScrLck) {
                list::add(INPUT_BUFFER, e.KeyChar);
                char out[2] = {e.KeyChar, '\0'};
                print(out);
            } else {
                print((int)e.KeyCode);
                cursor_x += 3;
                return;
            }
            cursor_x += 1;
        
            if (cursor_x >= max_cols) {
                cursor_x = 0;
                cursor_y += 1;
            }
        }

        if (cursor_y >= max_rows - 1) {
            screen::scroll_up();
            cursor_y = max_rows - 2;
        }

        screen::set_cursor_position(cursor_x * Screen.SymbolSizeX, cursor_y * Screen.SymbolSizeY);
    }
} // namespace

namespace commands {
    static bool cmdcmp(list::List<char>* str1, const char* str2, int len) {
        for (int i = 0; i < len; i++) {
            char val;
            list::get(str1, i, val);
            if (val != str2[i]) {
                return false;
            }
        }
        return true;
    }

    const char* parse_and_exec(list::List<char>* input) {
        UINT64 len = list::size(input);
        if (len == 0) {
            return "";
        }

        if (cmdcmp(input, "help", len)) {
            cursor_x = 0;
            cursor_y += 3;
            return  "\n\rhelp  - shows this info"
                    "\n\rclear - clear screen"
                    "\n\rcpuid - get cpu info";
        }
        if (cmdcmp(input, "clear", len)) {
            screen::clear();
            cursor_y = 0;
            return "";
        }
        if (cmdcmp(input, "cpuid", len)) {
            list::clear(cpuinfo);

            char num_buf[12];
            char name_buf[64];
            name_buf[0] = '\0';
            cpuid::get_cpu_name(name_buf);

            for (const char* p = "\n\r           CPU: "; *p; ++p) list::add(cpuinfo, *p);
            for (const char* p = name_buf; *p; ++p) list::add(cpuinfo, *p);

            UINT32 base_freq = cpuid::get_base_freq();
            for (const char* p = "\n\r      BaseFreq: "; *p; ++p) list::add(cpuinfo, *p);
            int_to_str(base_freq, num_buf); for (char* p = num_buf; *p; ++p) list::add(cpuinfo, *p);
            for (const char* p = " MHz"; *p; ++p) list::add(cpuinfo, *p);

            UINT32 max_freq = cpuid::get_max_freq();
            for (const char* p = "\n\r       MaxFreq: "; *p; ++p) list::add(cpuinfo, *p);
            int_to_str(max_freq, num_buf); for (char* p = num_buf; *p; ++p) list::add(cpuinfo, *p);
            for (const char* p = " MHz"; *p; ++p) list::add(cpuinfo, *p);

            UINT32 bus_freq = cpuid::get_bus_freq();
            for (const char* p = "\n\r       BusFreq: "; *p; ++p) list::add(cpuinfo, *p);
            int_to_str(bus_freq, num_buf); for (char* p = num_buf; *p; ++p) list::add(cpuinfo, *p);
            for (const char* p = " MHz"; *p; ++p) list::add(cpuinfo, *p);

            CPUTopology topo;
            cpuid::get_cpu_topology(&topo);

            for (const char* p = "\n\r Logical cores: "; *p; ++p) list::add(cpuinfo, *p);
            int_to_str(topo.logical_cores, num_buf);
            for (char* p = num_buf; *p; ++p) list::add(cpuinfo, *p);

            for (const char* p = "\n\rPhysical cores: "; *p; ++p) list::add(cpuinfo, *p);
            int_to_str(topo.physical_cores, num_buf);
            for (char* p = num_buf; *p; ++p) list::add(cpuinfo, *p);

            for (const char* p = "\n\r       Sockets: "; *p; ++p) list::add(cpuinfo, *p);
            int_to_str(topo.packages, num_buf);
            for (char* p = num_buf; *p; ++p) list::add(cpuinfo, *p);

            for (const char* p = "\n\rHyperthreading: "; *p; ++p) list::add(cpuinfo, *p);
            const char* ht_status = topo.hyperthreading ? "Yes" : "No";
            for (const char* p = ht_status; *p; ++p) list::add(cpuinfo, *p);

            CacheInfo cache;
            cpuid::get_cache_info(&cache);

            // --- L1 ---
            for (const char* p = "\n\r            L1: "; *p; ++p) list::add(cpuinfo, *p);
            int_to_str(cache.l1d_size + cache.l1i_size, num_buf); for (char* p = num_buf; *p; ++p) list::add(cpuinfo, *p);
            for (const char* p = " KB"; *p; ++p) list::add(cpuinfo, *p);

            // --- L2 ---
            for (const char* p = "\n\r            L2: "; *p; ++p) list::add(cpuinfo, *p);
            int_to_str(cache.l2_size, num_buf); for (char* p = num_buf; *p; ++p) list::add(cpuinfo, *p);
            for (const char* p = " KB"; *p; ++p) list::add(cpuinfo, *p);

            // --- L3 ---
            for (const char* p = "\n\r            L3: "; *p; ++p) list::add(cpuinfo, *p);
            int_to_str(cache.l3_size, num_buf); for (char* p = num_buf; *p; ++p) list::add(cpuinfo, *p);
            for (const char* p = " KB"; *p; ++p) list::add(cpuinfo, *p);

            list::add(cpuinfo, '\0');
            list::Block<char>* first_block = cpuinfo->first_block;
            cursor_y += 11;
            return (const char*)first_block->data;
        }

        cursor_x = 2;
        cursor_y += 1;
        return "\n\rThis is not a command!";
    }
} // namespace