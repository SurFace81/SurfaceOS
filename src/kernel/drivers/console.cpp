#include "../../include/drivers/console.h"

#define BUFFER_SIZE 256

UINT32 cursor_x = 0;
UINT32 cursor_y = 0;
UINT32 max_cols = 0;
UINT32 max_rows = 0;

alignas(16) char INPUT_BUFFER[BUFFER_SIZE] = {0};
static int  inPtr = 0;

namespace commands {
    const char* parse_and_exec(const char* input, int len);
}

namespace console {
    static void input(keyboard_event_t e);

    void init(void) {
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
            if (inPtr > 0) {
                inPtr -= 1;
            }

            screen::backspace(cursor_x, cursor_y);
        }
        else if (e.KeyCode == Keys::ENTER) {
            print(commands::parse_and_exec(INPUT_BUFFER, inPtr));
            inPtr = 0;
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
                if (inPtr < BUFFER_SIZE) {
                    INPUT_BUFFER[inPtr] = e.KeyChar;
                    inPtr += 1;
                }
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
    static bool cmdcmp(const char* str1, const char* str2, int len) {
        for (int i = 0; i < len; i++) {
            if (str1[i] != str2[i]) {
                return false;
            }
        }
        return true;
    }

    const char* parse_and_exec(const char* input, int len) {
        if (len == 0) {
            cursor_x = 2;
            cursor_y += 1;
            return "\n\rThis is not a command!";
        }

        if (cmdcmp(input, "help", len)) {
            cursor_x = 0;
            cursor_y += 1;
            return "\n\rCLEAR - clear screen";
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