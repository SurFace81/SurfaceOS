#include <stdio.h>
#include <stdlib.h>

int main()
{   
    print("Hello world!!!");
    while (read_key().KeyCode != KEY_ESCAPE) {}

    clear();

    return 0;
}