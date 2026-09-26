// Files: SfFiles (the roots) and SfFile objects.
//
// An SfFile is a block on the program's heap: the protocol table the
// program sees, followed by the kernel's handle number of the file.

#include "runtime.h"

struct FileObject
{
    SfFile   Public;
    uint64_t Handle;
};

static FileObject* Object(SfFile* This)
{
    return (FileObject*)This;
}

static SfStatus FileOpen(SfFile* This, const char* Path, uint64_t Mode, SfFile** Out);

static SfStatus FileClose(SfFile* This)
{
    SfStatus Status = SfCall(SFCALL_CLOSE, Object(This)->Handle);
    MemoryFree((SfMemory*)&SdkMemory, This);
    return Status;
}

static SfStatus FileRead(SfFile* This, void* Buffer, uint64_t* Size)
{
    return SfCall(SFCALL_FILE_READ, Object(This)->Handle, (uint64_t)Buffer, (uint64_t)Size);
}

static SfStatus FileWrite(SfFile* This, const void* Buffer, uint64_t* Size)
{
    return SfCall(SFCALL_FILE_WRITE, Object(This)->Handle, (uint64_t)Buffer, (uint64_t)Size);
}

static SfStatus FileGetPosition(SfFile* This, uint64_t* Position)
{
    return SfCall(SFCALL_FILE_GET_POSITION, Object(This)->Handle, (uint64_t)Position);
}

static SfStatus FileSetPosition(SfFile* This, uint64_t Position)
{
    return SfCall(SFCALL_FILE_SET_POSITION, Object(This)->Handle, Position);
}

static const SfFile FileTable =
{
    { SF_FILE_SIGNATURE, SF_FILE_REVISION, sizeof(SfFile) },
    FileOpen,
    FileClose,
    FileRead,
    FileWrite,
    FileGetPosition,
    FileSetPosition,
};

// Wrap a freshly opened handle in an SfFile. When there is no memory for
// it, the handle is closed again.
static SfStatus NewFile(SfStatus Status, uint64_t Handle, SfFile** Out)
{
    if (SF_ERROR(Status))
        return Status;

    FileObject* File = nullptr;
    Status = MemoryAllocate((SfMemory*)&SdkMemory, sizeof(FileObject), (void**)&File);
    if (SF_ERROR(Status))
    {
        SfCall(SFCALL_CLOSE, Handle);
        return Status;
    }
    File->Public = FileTable;
    File->Handle = Handle;
    *Out = &File->Public;
    return SF_SUCCESS;
}

static SfStatus FileOpen(SfFile* This, const char* Path, uint64_t Mode, SfFile** Out)
{
    if (!Out)
        return SF_INVALID_PARAMETER;
    uint64_t Handle = 0;
    SfStatus Status = SfCall(SFCALL_FILE_OPEN, Object(This)->Handle, (uint64_t)Path, Mode,
                             (uint64_t)&Handle);
    return NewFile(Status, Handle, Out);
}

SfStatus FilesOpen(SfFiles*, const char* Path, uint64_t Mode, SfFile** Out)
{
    if (!Out)
        return SF_INVALID_PARAMETER;
    uint64_t Handle = 0;
    SfStatus Status = SfCall(SFCALL_FILES_OPEN, (uint64_t)Path, Mode, (uint64_t)&Handle);
    return NewFile(Status, Handle, Out);
}

SfStatus FilesCreateUnique(SfFiles*, SfFile** Out, char* Path, uint64_t PathSize)
{
    if (!Out)
        return SF_INVALID_PARAMETER;
    uint64_t Handle = 0;
    SfStatus Status = SfCall(SFCALL_FILES_CREATE_UNIQUE, (uint64_t)&Handle, (uint64_t)Path,
                             PathSize);
    return NewFile(Status, Handle, Out);
}
