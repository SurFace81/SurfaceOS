#ifndef SFOS_TABLE_H
#define SFOS_TABLE_H

#include "status.h"

// Every SDK table (SfSystem, SfApp) and protocol (SfConsole, SfFile) starts
// with this header.
//
//   Signature  eight ASCII characters naming the table, for sanity checks
//   Revision   SF_REVISION(major, minor) of the layout the kernel filled in
//   Size       bytes of the whole table as filled in
//
// Rule: a table only ever grows at the end. A program built against an
// older layout keeps working, and one built against a newer layout can tell
// from Size whether a field it wants is there at all.
typedef struct SfTableHeader
{
    uint64_t Signature;
    uint32_t Revision;
    uint32_t Size;
} SfTableHeader;

SF_STATIC_ASSERT(sizeof(SfTableHeader) == 16, "SfTableHeader layout");

#define SF_REVISION(major, minor)   (((uint32_t)(major) << 16) | (uint32_t)(minor))
#define SF_REVISION_MAJOR(r)        ((uint32_t)(r) >> 16)
#define SF_REVISION_MINOR(r)        ((uint32_t)(r) & 0xFFFF)

// Eight characters, the first one in the lowest byte.
#define SF_SIGNATURE(a, b, c, d, e, f, g, h) \
    ((uint64_t)(uint8_t)(a)         | ((uint64_t)(uint8_t)(b) << 8)  | \
     ((uint64_t)(uint8_t)(c) << 16) | ((uint64_t)(uint8_t)(d) << 24) | \
     ((uint64_t)(uint8_t)(e) << 32) | ((uint64_t)(uint8_t)(f) << 40) | \
     ((uint64_t)(uint8_t)(g) << 48) | ((uint64_t)(uint8_t)(h) << 56))

// Is the field `f` of table `t` (type T) inside what the kernel filled in?
#define SF_HAS_FIELD(t, T, f) \
    ((t)->Hdr.Size >= SF_OFFSET_OF(T, f) + sizeof(((T*)0)->f))

#endif // SFOS_TABLE_H
