#include "../../include/stdlib/stdio.h"

void print(const char* str)
{
    screen::write(str);
}

void print(list::List<char>* char_list)
{
    if (!char_list)
        return;

    list::Block<char>* current = char_list->first_block;

    while (current)
    {
        for (uint64_t i = 0; i < current->used; i++)
        {
            print(current->data[i]);
            if (current->data[i] == '\0')
            {
                return;
            }
        }
        current = current->next;
    }
}

void print(char c)
{
    char out[2] = {c, '\0'};
    print(out);
}

void print(int dec)
{
    char temp_str[100];
    int_to_str(dec, temp_str);
    print(temp_str);
}

void print(uint64_t hex, uint64_t size)
{
    char temp_str[size];
    hex_to_str(hex, temp_str, size);
    print("0x");
    print(temp_str);
}