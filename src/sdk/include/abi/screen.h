#ifndef ABI_SCREEN_H
#define ABI_SCREEN_H

#include "types.h"

enum color_t
{
    COLOR_BLUE         = 0x000000FF,
    COLOR_GREEN        = 0x0000FF00,
    COLOR_CYAN         = 0x0000FFFF,
    COLOR_RED          = 0x00FF0000,
    COLOR_MAGENTA      = 0x00FF00FF,
    COLOR_YELLOW       = 0x00FFFF00,
    COLOR_WHITE        = 0x00FFFFFF,
    COLOR_GRAY         = 0x9E9E9EA8,
    COLOR_LIGHT_BLUE   = 0x0000AFFF,
    COLOR_LIGHT_GREEN  = 0x0000FFAA,
    COLOR_LIGHT_RED    = 0x00FF4444,
    COLOR_BRIGHT_WHITE = 0x00F0F0F0,
};

struct screen_size_t
{
    uint32_t cols;
    uint32_t rows;
};

#endif