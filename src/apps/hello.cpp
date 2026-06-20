#include <stdio.h>

int main()
{
    char path[128];
    print("Save to: ");
    read_line(path, 128);

    char text[4096];
    print("Text: ");
    uint32_t len = read_line(text, sizeof(text));

    uint32_t result = write_file(path, (const uint8_t*)text, len);

    if (result == (uint32_t)-1)
        print("Error writing file\n");
    else
        print("Saved\n");

    return 0;
}