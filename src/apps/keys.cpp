// keys: report every key the driver sees.
//
// This is how "all keys work" is checked. It reads raw events, so it tests
// the keyboard driver on its own, without the escape encoder or the line
// discipline in the way. Each press prints one line:
//
//   key code=63 char=--  mods=CTRL,SHIFT   name=F5
//
// Run "keys" and press things; Esc quits (it is still the session's kill
// key at this point in the stage).

#include <stdio.h>
#include <string.h>
#include <abi/keyboard.h>
#include <key.h>

static const char* key_name(uint8_t c)
{
    switch (c)
    {
        case KEY_ESCAPE:       return "Escape";
        case KEY_BACKSPACE:    return "Backspace";
        case KEY_TAB:          return "Tab";
        case KEY_ENTER:        return "Enter";
        case KEY_SPACE:        return "Space";
        case KEY_CAPS_LOCK:    return "CapsLock";
        case KEY_NUM_LOCK:     return "NumLock";
        case KEY_SCROLL_LOCK:  return "ScrollLock";
        case KEY_LEFT_SHIFT:   return "LShift";
        case KEY_RIGHT_SHIFT:  return "RShift";
        case KEY_LEFT_CTRL:    return "LCtrl";
        case KEY_RIGHT_CTRL:   return "RCtrl";
        case KEY_LEFT_ALT:     return "LAlt";
        case KEY_RIGHT_ALT:    return "RAlt/AltGr";
        case KEY_F1:           return "F1";
        case KEY_F2:           return "F2";
        case KEY_F3:           return "F3";
        case KEY_F4:           return "F4";
        case KEY_F5:           return "F5";
        case KEY_F6:           return "F6";
        case KEY_F7:           return "F7";
        case KEY_F8:           return "F8";
        case KEY_F9:           return "F9";
        case KEY_F10:          return "F10";
        case KEY_F11:          return "F11";
        case KEY_F12:          return "F12";
        case KEY_HOME:         return "Home";
        case KEY_END:          return "End";
        case KEY_PAGE_UP:      return "PageUp";
        case KEY_PAGE_DOWN:    return "PageDown";
        case KEY_INSERT:       return "Insert";
        case KEY_DELETE:       return "Delete";
        case KEY_ARROW_UP:     return "Up";
        case KEY_ARROW_DOWN:   return "Down";
        case KEY_ARROW_LEFT:   return "Left";
        case KEY_ARROW_RIGHT:  return "Right";
        case KEY_PRINT_SCREEN: return "PrintScreen";
        case KEY_PAUSE:        return "Pause";
        case KEY_MENU:         return "Menu";
        case KEY_LEFT_WIN:     return "LWin";
        case KEY_RIGHT_WIN:    return "RWin";
        case KEY_KP_ENTER:     return "KP_Enter";
        case KEY_KP_SLASH:     return "KP_/";
        case KEY_KP_STAR:      return "KP_*";
        case KEY_KP_MINUS:     return "KP_-";
        case KEY_KP_PLUS:      return "KP_+";
        case KEY_KP_PERIOD:    return "KP_.";
        case KEY_KP_0:         return "KP_0";
        case KEY_KP_1:         return "KP_1";
        case KEY_KP_2:         return "KP_2";
        case KEY_KP_3:         return "KP_3";
        case KEY_KP_4:         return "KP_4";
        case KEY_KP_5:         return "KP_5";
        case KEY_KP_6:         return "KP_6";
        case KEY_KP_7:         return "KP_7";
        case KEY_KP_8:         return "KP_8";
        case KEY_KP_9:         return "KP_9";
        default:               return "";
    }
}

static void print_mods(uint8_t m)
{
    bool first = true;
    struct { uint8_t bit; const char* name; } tbl[] = {
        { KMOD_CTRL,   "CTRL"   },
        { KMOD_SHIFT,  "SHIFT"  },
        { KMOD_ALT,    "ALT"    },
        { KMOD_ALTGR,  "ALTGR"  },
        { KMOD_CAPS,   "CAPS"   },
        { KMOD_NUM,    "NUM"    },
        { KMOD_SCROLL, "SCROLL" },
    };
    for (int i = 0; i < 7; i++)
    {
        if (!(m & tbl[i].bit))
            continue;
        if (!first) print("+");
        print(tbl[i].name);
        first = false;
    }
    if (first) print("-");
}

// "keys decode": the same keyboard seen through the SDK decoder instead of
// raw driver events. This is the path an application actually uses - raw
// mode, escape sequences on the wire, structured keys out the other end.
static int decode_mode()
{
    if (key_raw_mode(1) != 0)
    {
        print("keys: cannot enter raw mode\n");
        return 1;
    }
    // Announce *after* the switch: the banner is what a test waits for, and
    // a key struck before raw mode is on would be eaten by the line editor.
    print("keys decode: raw mode, Esc quits\n\n");

    char name[40];
    for (;;)
    {
        struct key_event e;
        if (key_read(&e) != 0)
            break;

        print("dec code=");
        print_i64((sint64_t)e.code);
        print(" mods=");
        print_i64(e.mods);
        print(" name=");
        print(key_name(&e, name, sizeof(name)));
        print("\n");

        if (e.code == KEY_ESCAPE)
            break;
    }

    key_raw_mode(0);
    print("keys decode: done\n");
    return 0;
}

int main(int argc, char** argv)
{
    if (argc >= 2 && strcmp(argv[1], "decode") == 0)
        return decode_mode();

    print("keys: press any key, Esc quits\n\n");

    for (;;)
    {
        keyboard_event_t e = read_key();

        // Releases are reported by the driver too; only presses are
        // interesting here, and printing both halves the readable output.
        if (e.type != KEY_PRESS)
            continue;

        print("key code=");
        print_i64(e.KeyCode);

        print(" char=");
        if (e.KeyChar >= 0x20 && e.KeyChar < 0x7F)
        {
            char s[2] = { e.KeyChar, '\0' };
            print(s);
        }
        else if (e.KeyChar == 0)
        {
            print("--");
        }
        else
        {
            // A control code. print_hex64 supplies its own "0x".
            print_hex64((unsigned char)e.KeyChar);
        }

        print(" mods=");
        print_mods(e.Mods);

        const char* n = key_name(e.KeyCode);
        if (n[0])
        {
            print(" name=");
            print(n);
        }
        print("\n");

        if (e.KeyCode == KEY_ESCAPE)
            break;
    }

    print("keys: done\n");
    return 0;
}
