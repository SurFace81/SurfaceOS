#ifndef KEY_H
#define KEY_H

#include <abi/types.h>
#include <abi/keyboard.h>

// Structured key input.
//
// The terminal delivers bytes: printable characters as themselves, and
// everything else as an escape sequence. This turns them back into a key
// and a modifier mask, so an application can switch on a key rather than
// parse escapes.
//
// There is deliberately no binding registry here. A switch over `code` and
// `mods` is clearer than a table of callbacks for anything this SDK is
// likely to run.
//
// Codes below 0x80 that are not listed here are the character itself.
// Everything else reuses the KEY_* values from abi/keyboard.h, so the same
// names work whether the event came from here or from read_key().

// Modifier bits. These match KMOD_* in abi/keyboard.h.
#define KEY_MOD_SHIFT   KMOD_SHIFT
#define KEY_MOD_CTRL    KMOD_CTRL
#define KEY_MOD_ALT     KMOD_ALT

struct key_event
{
    uint32_t code;      // a character, or one of the KEY_* codes
    uint8_t  mods;      // KEY_MOD_*
};

#ifdef __cplusplus
extern "C" {
#endif

// Put the terminal into (or out of) raw mode and enable full-fidelity key
// reporting. In raw mode nothing is echoed, no line editing happens, and
// the interrupt key stops ending the program - so an application that
// turns it on must have a way out of its own.
//
// Returns 0 on success, -1 on failure.
int key_raw_mode(int on);

// Read one key. Blocks. Returns 0 on success, -1 on error or EOF.
//
// A lone Escape cannot be told from the start of a sequence until either
// the next byte arrives or enough time passes without one, so this waits
// briefly before reporting KEY_ESCAPE. That delay is the reason raw mode
// sets VTIME.
int key_read(struct key_event* out);

// A printable name for a key, e.g. "Ctrl+Shift+F5". `buf` should be at
// least 32 bytes. Returns buf.
char* key_name(const struct key_event* e, char* buf, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif // KEY_H
