#ifndef SFOS_FILE_H
#define SFOS_FILE_H

#include "table.h"

// An open file or directory. Paths are relative to the SfFile they are
// opened from and never lead above it. The first SfFile comes from a root
// (Sys->Files, files.h).
//
//   Open         open Path below This with SF_FILE_* Mode; *Out gets the
//                new SfFile. SF_NOT_FOUND, SF_ALREADY_EXISTS (with
//                SF_FILE_CREATE_NEW), SF_ACCESS_DENIED (Path leads above
//                This).
//   Close        close This; the pointer is invalid afterwards.
//   Read         read up to *Size bytes at the position into Buffer; *Size
//                gets the count read, 0 at the end of the file.
//   Write        write *Size bytes from Buffer at the position; *Size gets
//                the count written.
//   GetPosition  *Position gets the byte offset of the next Read/Write.
//   SetPosition  move it; a position past the end is allowed for writing.
typedef struct SfFile SfFile;

struct SfFile
{
    SfTableHeader Hdr;
    SfStatus (*Open)(SfFile* This, const char* Path, uint64_t Mode, SfFile** Out);
    SfStatus (*Close)(SfFile* This);
    SfStatus (*Read)(SfFile* This, void* Buffer, uint64_t* Size);
    SfStatus (*Write)(SfFile* This, const void* Buffer, uint64_t* Size);
    SfStatus (*GetPosition)(SfFile* This, uint64_t* Position);
    SfStatus (*SetPosition)(SfFile* This, uint64_t Position);
};

// Open modes.
#define SF_FILE_READ        0x01ULL
#define SF_FILE_WRITE       0x02ULL
#define SF_FILE_CREATE      0x04ULL     // create when missing
#define SF_FILE_CREATE_NEW  0x08ULL     // create; SF_ALREADY_EXISTS if present
#define SF_FILE_TRUNCATE    0x10ULL     // empty it on open (with SF_FILE_WRITE)

#define SF_FILE_SIGNATURE   SF_SIGNATURE('S', 'F', 'F', 'I', 'L', 'E', 0, 0)
#define SF_FILE_REVISION    SF_REVISION(1, 0)

SF_STATIC_ASSERT(SF_OFFSET_OF(SfFile, Open) == 16, "SfFile layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfFile, SetPosition) == 56, "SfFile layout");
SF_STATIC_ASSERT(sizeof(SfFile) == 64, "SfFile layout");

#endif // SFOS_FILE_H
