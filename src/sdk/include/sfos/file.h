#ifndef SFOS_FILE_H
#define SFOS_FILE_H

#include "table.h"
#include "time.h"

/// An entry of a folder (ReadDir), or what a file is (GetInfo).
typedef struct SfDirEntry
{
    char       Name[256];       ///< NUL-terminated; empty from GetInfo
    uint64_t   Size;            ///< bytes; 0 for a folder
    uint32_t   Flags;           ///< SF_DIR_ENTRY_*
    SfDateTime Modified;        ///< when it last changed
    uint32_t   Reserved;
} SfDirEntry;

/// SfDirEntry Flags: the entry is a folder.
#define SF_DIR_ENTRY_FOLDER 0x1

/// An open file or folder.
///
/// Paths are relative to the SfFile they are opened from and never lead above
/// it. The first SfFile comes from a root (Sys->Files, files.h).
typedef struct SfFile SfFile;

struct SfFile
{
    SfTableHeader Hdr;

    /// Opens Path below This with SF_FILE_* Mode; *Out gets the new
    /// SfFile.
    ///
    /// SF_NOT_FOUND, SF_ALREADY_EXISTS (with SF_FILE_CREATE_NEW),
    /// SF_ACCESS_DENIED (Path leads above This).
    SfStatus (*Open)(SfFile* This, const char* Path, uint64_t Mode, SfFile** Out);

    /// Closes This; the pointer is invalid afterwards.
    SfStatus (*Close)(SfFile* This);

    /// Reads up to *Size bytes at the position into Buffer; *Size gets
    /// the count read, 0 at the end of the file.
    SfStatus (*Read)(SfFile* This, void* Buffer, uint64_t* Size);

    /// Writes *Size bytes from Buffer at the position; *Size gets the
    /// count written.
    SfStatus (*Write)(SfFile* This, const void* Buffer, uint64_t* Size);

    /// *Position gets the byte offset of the next Read or Write.
    SfStatus (*GetPosition)(SfFile* This, uint64_t* Position);

    /// Moves the position; one past the end is allowed for writing.
    SfStatus (*SetPosition)(SfFile* This, uint64_t Position);

    /// This is a folder: *Entry gets its next entry ("." and ".." are
    /// left out), SF_END_OF_FILE after the last.
    ///
    /// SetPosition(This, 0) starts again.
    SfStatus (*ReadDir)(SfFile* This, SfDirEntry* Entry);

    /// *Info gets what This is: its size, whether it is a folder, when
    /// it last changed (Name is left empty).
    SfStatus (*GetInfo)(SfFile* This, SfDirEntry* Info);
};

/// Open modes.
///
/// Open mode: for reading.
#define SF_FILE_READ        0x01ULL
/// Open mode: for writing.
#define SF_FILE_WRITE       0x02ULL
/// Open mode: create the file when it is missing.
#define SF_FILE_CREATE      0x04ULL
/// Open mode: create the file; SF_ALREADY_EXISTS when it is there.
#define SF_FILE_CREATE_NEW  0x08ULL
/// Open mode: empty the file on open (with SF_FILE_WRITE).
#define SF_FILE_TRUNCATE    0x10ULL

#define SF_FILE_SIGNATURE   SF_SIGNATURE('S', 'F', 'F', 'I', 'L', 'E', 0, 0)

SF_STATIC_ASSERT(SF_OFFSET_OF(SfFile, Open) == 16, "SfFile layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfFile, SetPosition) == 56, "SfFile layout");
SF_STATIC_ASSERT(SF_OFFSET_OF(SfFile, GetInfo) == 72, "SfFile layout");
SF_STATIC_ASSERT(sizeof(SfFile) == 80, "SfFile layout");
SF_STATIC_ASSERT(sizeof(SfDirEntry) == 280, "SfDirEntry layout");

#endif // SFOS_FILE_H
