#include <stdio.h>

int main()
{
    print("Hello world!\n");

    // Wait for Enter, then return to the console
    char buf[16];
    read_line(buf, sizeof(buf));

    return 0;
}
