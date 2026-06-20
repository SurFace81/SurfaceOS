#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main()
{   
    print("Hello world!!!");
    keyboard_event_t e;
    do { e = read_key(); } while (e.type != KEY_PRESS || e.KeyCode != KEY_ESCAPE);

    clear();

    return 0;
}