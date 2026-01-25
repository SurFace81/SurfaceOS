#include "../../include/drivers/console.h"

namespace {
    uint32_t cursor_x = 0;
    uint32_t cursor_y = 0;
    uint32_t max_cols = 0;
    uint32_t max_rows = 0;

    list::List<char>* INPUT_BUFFER;
    list::List<char>* infobuf;
    char cpu_name[51];
}

namespace commands {
    void parse_and_exec(list::List<char>* input);
}

namespace console {
    static void input(keyboard_event_t e);

    void init(void) {
        INPUT_BUFFER = list::create<char>();
        infobuf = list::create<char>();
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
            commands::parse_and_exec(INPUT_BUFFER);
            print(infobuf);
            list::clear(infobuf);
            list::clear(INPUT_BUFFER);
            print("\n\r> ");
            cursor_x = 2;
            cursor_y += 1;
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

        screen::set_cursor_position(cursor_x, cursor_y);
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

    static void add_to_out_list(const char* str) {
        for (const char* p = str; *p; ++p) list::add(infobuf, *p);
    }

    static void add_to_out_list(int num, bool isHex, int size = 8) {
        char buf[12] = {0};
        if (isHex) {
            hex_to_str(num, buf, size);
        } else {
            int_to_str(num, buf);
        }
        for (char* p = buf; *p; ++p) list::add(infobuf, *p);
    }

    void parse_and_exec(list::List<char>* input) {
        uint64_t len = list::size(input);
        if (len == 0) {
            return;
        }

        if (cmdcmp(input, "help", len)) {
            cursor_x = 0;
            cursor_y += 5;
            add_to_out_list("\n\rhelp  - shows this info");
            add_to_out_list("\n\rclear - clear screen");
            add_to_out_list("\n\rcpuid - get cpu info");
            add_to_out_list("\n\rlspci - list of all PCI devices");
            add_to_out_list("\n\rlsusb - list of all USB devices");
            return;
        }
        if (cmdcmp(input, "clear", len)) {
            screen::clear();
            cursor_y = 0;
            return;
        }
        if (cmdcmp(input, "lsusb", len)) {
            USBDeviceList* usbs = xhci::get_device_list();
            add_to_out_list("\n\rUSB devices: ");
            add_to_out_list(usbs->length, false);

            for (int i = 0; i < usbs->length; i++) {
                add_to_out_list("\n\rPort ");
                add_to_out_list(i, false);
                add_to_out_list(": ");
                add_to_out_list(usbs->devices[i].speed > 0 ? "Device connected (" : "Disconnect (");
                add_to_out_list(xhci::get_speed_name(usbs->devices[i].speed));
                add_to_out_list(")");

                cursor_y += 1;
            }

            cursor_y += 1;
            return;
        }
        if (cmdcmp(input, "lspci", len)) {
            uint32_t device_count = pci::device_count();
            add_to_out_list("\n\r PCI devices: ");
            add_to_out_list(device_count, false);

            for (int i = 0; i < device_count; i++) {
                PCIDevice* dev = pci::get_by_id(i);
                add_to_out_list("\n\r Vendor: 0x");
                add_to_out_list(dev->vendor_id, true, 4);
                add_to_out_list(" Device: 0x");
                add_to_out_list(dev->device_id, true, 4);
                add_to_out_list(" Class: 0x");
                add_to_out_list(dev->class_code, true, 2);
                add_to_out_list(" Subclass: 0x");
                add_to_out_list(dev->subclass, true, 2);
                add_to_out_list(" ProgIF: 0x");
                add_to_out_list(dev->prog_if, true, 2);

                cursor_y += 1;
            }

            add_to_out_list("\0");
            cursor_y += 1;
            return;
        }
        if (cmdcmp(input, "cpuid", len)) {
            char name_buf[64];
            name_buf[0] = '\0';
            cpuid::get_cpu_name(name_buf);

            add_to_out_list("\n\r           CPU: ");
            add_to_out_list(name_buf);
            add_to_out_list("\n\r      BaseFreq: ");
            add_to_out_list(cpuid::get_base_freq(), false);
            add_to_out_list(" MHz\n\r       MaxFreq: ");
            add_to_out_list(cpuid::get_max_freq(), false);
            add_to_out_list(" MHz\n\r       BusFreq: ");
            add_to_out_list(cpuid::get_bus_freq(), false);
            CPUTopology topo;
            cpuid::get_cpu_topology(&topo);
            add_to_out_list(" MHz\n\r Logical cores: ");
            add_to_out_list(topo.logical_cores, false);
            add_to_out_list("\n\rPhysical cores: ");
            add_to_out_list(topo.physical_cores, false);
            add_to_out_list("\n\r       Sockets: ");
            add_to_out_list(topo.packages, false);
            add_to_out_list("\n\rHyperthreading: ");
            add_to_out_list(topo.hyperthreading ? "Yes" : "No");
            CacheInfo cache;
            cpuid::get_cache_info(&cache);
            add_to_out_list("\n\r            L1: ");
            add_to_out_list(cache.l1d_size + cache.l1i_size, false);
            add_to_out_list(" KB\n\r            L2: ");
            add_to_out_list(cache.l2_size, false);
            add_to_out_list(" KB\n\r            L3: ");
            add_to_out_list(cache.l3_size, false);
            add_to_out_list(" KB\0");

            cursor_y += 11;
            return;
        }

        cursor_x = 2;
        cursor_y += 1;
        add_to_out_list("\n\rThis is not a command!");
        return;
    }
} // namespace