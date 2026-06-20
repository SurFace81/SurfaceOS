#include <stdio.h>
#include <stdlib.h>

int main()
{
    print("Type something and press Enter (ESC to quit):\n\r");

    int cap = 64;
    int pos = 0;
    char* line = (char*)malloc(cap);

    while (1)
    {
        keyboard_event_t e = read_key();

        if (e.type != KEY_PRESS)
            continue;

        if (e.KeyCode == KEY_ESCAPE)
            break;

        if (e.KeyCode == KEY_ENTER)
        {
            line[pos] = '\0';
            print("\n\rYou typed: ");
            print(line);
            print("\n\r");
            pos = 0;
            continue;
        }

        if (e.KeyCode == KEY_BACKSPACE)
        {
            if (pos > 0)
            {
                pos--;
                print("\b \b");
            }
            continue;
        }

        if (e.KeyChar != 0)
        {
            if (pos >= cap - 1)
            {
                int new_cap = cap * 2;
                char* new_line = (char*)malloc(new_cap);
                for (int i = 0; i < pos; i++)
                    new_line[i] = line[i];
                free(line);
                line = new_line;
                cap = new_cap;
                print("[realloc]\n\r");
            }

            line[pos++] = e.KeyChar;
            char buf[2] = {e.KeyChar, '\0'};
            print(buf);
        }
    }

    free(line);
    return 0;
}