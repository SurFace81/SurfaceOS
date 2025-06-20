#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "../cpu/types.h"

#define KEYBOARD_DATA_PORT      0x60
#define KEYBOARD_STATUS_PORT    0x64
#define KEYBOARD_COMMAND_PORT   0x64

#define KEYBOARD_BUFFER_SIZE    256

#define KEY_RELEASED_MASK       0x80

#define LSHIFT_SCANCODE         0x2A
#define RSHIFT_SCANCODE         0x36
#define LCTRL_SCANCODE          0x1D
#define LALT_SCANCODE           0x38
#define CAPS_LOCK_SCANCODE      0x3A
#define NUM_LOCK_SCANCODE       0x45
#define SCROLL_LOCK_SCANCODE    0x46

#define EXTENDED_SCANCODE       0xE0

typedef struct {
    UINT8 buffer[KEYBOARD_BUFFER_SIZE];
    UINT32 head;
    UINT32 tail;
    UINT32 count;
} keyboard_buffer_t;

typedef struct {
    UINT8 shift_pressed;
    UINT8 ctrl_pressed;
    UINT8 alt_pressed;
    UINT8 caps_lock;
    UINT8 num_lock;
    UINT8 scroll_lock;
    UINT8 extended_code;
} keyboard_state_t;

#ifdef __cplusplus
extern "C" {
#endif

void keyboard_init(void);
void keyboard_handler(void);
char keyboard_getchar(void);
UINT8 keyboard_scancode_to_ascii(UINT8 scancode);

#ifdef __cplusplus
}
#endif

#endif