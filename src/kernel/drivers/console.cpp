#include "../../include/drivers/console.h"

namespace {
    UINT32 cursor_x = 0;
    UINT32 cursor_y = 0;
    UINT32 max_cols = 0;
    UINT32 max_rows = 0;

    list::List<char>* INPUT_BUFFER;
}

namespace commands {
    const char* parse_and_exec(list::List<char>* input);
}

namespace console {
    static void input(keyboard_event_t e);

    void init(void) {
        INPUT_BUFFER = list::create<char>();
        max_cols = Screen.Width / Screen.SymbolSizeX;
        max_rows = Screen.Height / Screen.SymbolSizeY;

        keyboard::set_keyboard_callback(input);

        print("\n\tSurfaceOS v0.1 (C) 2025\n\r");
        print("\tMem: ");
        print((int)(memory::getMemorySize() / 1048576 + 1));
        print(" Mb");
        print("\n\r------------------------------------------------\n\n\r> ");
        
        cursor_x = 2; cursor_y = 5;
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
            cursor_y += 2;
            return "\n\rhelp  - shows this info\n\rclear - clear screen";
        }
        if (cmdcmp(input, "clear", len)) {
            screen::clear();
            cursor_x = 2;
            cursor_y = 0;
            return "";
        }

        cursor_x = 2;
        cursor_y += 1;
        return "\n\rThis is not a command!";
    }
} // namespace