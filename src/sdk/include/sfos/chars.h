#ifndef SFOS_CHARS_H
#define SFOS_CHARS_H

// The characters of the screen beyond ASCII. A screen cell holds one byte,
// drawn from the system font: code page 437, the PC's own. A program's
// source is UTF-8, where "─" is three bytes, so these codes go in by name:
//
//     SfCell Line[] = { { SF_BOX_TOP_LEFT, Color }, { SF_BOX_H, Color }, ... };
//     char Title[] = { SF_BOX_T_LEFT, ' ', 'N', 'o', 't', 'e', 's', ' ',
//                      SF_BOX_T_RIGHT, 0 };
//
// Lines: SF_BOX_* single, SF_BOX2_* double; they join up cell to cell.
// T_DOWN is the one with a stem going down (a top edge meeting a line
// below), and so on. The codes below 0x20 (arrows, triangles) are control
// characters to Print in SF_CONSOLE_LINE: WriteAt and Draw show them.

#define SF_BOX_H                ((char)0xC4)    // ─
#define SF_BOX_V                ((char)0xB3)    // │
#define SF_BOX_TOP_LEFT         ((char)0xDA)    // ┌
#define SF_BOX_TOP_RIGHT        ((char)0xBF)    // ┐
#define SF_BOX_BOTTOM_LEFT      ((char)0xC0)    // └
#define SF_BOX_BOTTOM_RIGHT     ((char)0xD9)    // ┘
#define SF_BOX_T_DOWN           ((char)0xC2)    // ┬
#define SF_BOX_T_UP             ((char)0xC1)    // ┴
#define SF_BOX_T_RIGHT          ((char)0xC3)    // ├
#define SF_BOX_T_LEFT           ((char)0xB4)    // ┤
#define SF_BOX_CROSS            ((char)0xC5)    // ┼

#define SF_BOX2_H               ((char)0xCD)    // ═
#define SF_BOX2_V               ((char)0xBA)    // ║
#define SF_BOX2_TOP_LEFT        ((char)0xC9)    // ╔
#define SF_BOX2_TOP_RIGHT       ((char)0xBB)    // ╗
#define SF_BOX2_BOTTOM_LEFT     ((char)0xC8)    // ╚
#define SF_BOX2_BOTTOM_RIGHT    ((char)0xBC)    // ╝
#define SF_BOX2_T_DOWN          ((char)0xCB)    // ╦
#define SF_BOX2_T_UP            ((char)0xCA)    // ╩
#define SF_BOX2_T_RIGHT         ((char)0xCC)    // ╠
#define SF_BOX2_T_LEFT          ((char)0xB9)    // ╣
#define SF_BOX2_CROSS           ((char)0xCE)    // ╬

// Blocks and shades: bars, scroll bars, shadows.
#define SF_BLOCK_FULL           ((char)0xDB)    // █
#define SF_BLOCK_UPPER          ((char)0xDF)    // ▀
#define SF_BLOCK_LOWER          ((char)0xDC)    // ▄
#define SF_BLOCK_LEFT           ((char)0xDD)    // ▌
#define SF_BLOCK_RIGHT          ((char)0xDE)    // ▐
#define SF_SHADE_LIGHT          ((char)0xB0)    // ░
#define SF_SHADE_MEDIUM         ((char)0xB1)    // ▒
#define SF_SHADE_DARK           ((char)0xB2)    // ▓

// Arrows and triangles (WriteAt and Draw only, see above).
#define SF_ARROW_UP             ((char)0x18)    // ↑
#define SF_ARROW_DOWN           ((char)0x19)    // ↓
#define SF_ARROW_RIGHT          ((char)0x1A)    // →
#define SF_ARROW_LEFT           ((char)0x1B)    // ←
#define SF_TRIANGLE_UP          ((char)0x1E)    // ▲
#define SF_TRIANGLE_DOWN        ((char)0x1F)    // ▼
#define SF_TRIANGLE_RIGHT       ((char)0x10)    // ►
#define SF_TRIANGLE_LEFT        ((char)0x11)    // ◄

#endif // SFOS_CHARS_H
