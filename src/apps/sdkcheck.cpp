// sdkcheck: the SDK tables a SurfaceOS program is started with.
//
// Checks what the kernel hands SfMain - signatures, revisions and sizes of
// SfSystem, SfApp, SfConsole and SfFiles - that Console->Print works, and
// the roots data:/ and tmp:/ with files in them. The exit status is the
// number of failed checks (0: all passed).

#include <sfos.h>

static SfConsole* Con;
static uint64_t Passed;
static uint64_t Failed;

static void Print(const char* Text)
{
    Con->Print(Con, Text);
}

static void PrintNumber(uint64_t Value)
{
    char Buffer[24];
    int  Pos = 23;
    Buffer[Pos] = '\0';
    do
    {
        Buffer[--Pos] = (char)('0' + Value % 10);
        Value /= 10;
    } while (Value);
    Print(&Buffer[Pos]);
}

static void Check(const char* Name, bool Ok)
{
    Print(Ok ? "  [ ok ] " : "  [FAIL] ");
    Print(Name);
    Print("\n");
    if (Ok)
        Passed++;
    else
        Failed++;
}

static bool SameText(const char* A, const char* B)
{
    while (*A && *A == *B)
    {
        A++;
        B++;
    }
    return *A == *B;
}

static bool HeaderOk(const SfTableHeader* Hdr, uint64_t Signature, uint32_t Size)
{
    return Hdr->Signature == Signature &&
           SF_REVISION_MAJOR(Hdr->Revision) == 1 &&
           Hdr->Size >= Size;
}

static bool SameBytes(const char* A, const char* B, uint64_t Size)
{
    for (uint64_t i = 0; i < Size; i++)
        if (A[i] != B[i])
            return false;
    return true;
}

// Write Text to a file and read it back through a second open.
static void CheckRoots(SfFiles* Files)
{
    const char Text[] = "written by sdkcheck";
    const uint64_t Len = sizeof(Text) - 1;

    SfFile*  File = nullptr;
    uint64_t Size = Len;
    Check("data:/ creates a file",
          Files->Open(Files, "data:/check.txt",
                      SF_FILE_WRITE | SF_FILE_CREATE | SF_FILE_TRUNCATE, &File) == SF_SUCCESS &&
          File && HeaderOk(&File->Hdr, SF_FILE_SIGNATURE, sizeof(SfFile)));
    if (!File)
        return;
    Check("Write writes all of it",
          File->Write(File, Text, &Size) == SF_SUCCESS && Size == Len);
    uint64_t Position = 0;
    Check("GetPosition is after the text",
          File->GetPosition(File, &Position) == SF_SUCCESS && Position == Len);
    Check("Close", File->Close(File) == SF_SUCCESS);
    Check("a closed SfFile is SF_BAD_HANDLE",
          File->GetPosition(File, &Position) == SF_BAD_HANDLE);

    // Through the root folder, relative to it.
    SfFile* Dir = nullptr;
    Check("data:/ alone opens the folder",
          Files->Open(Files, "data:/", SF_FILE_READ, &Dir) == SF_SUCCESS && Dir);
    if (!Dir)
        return;
    char Buffer[64] = {};
    Size = sizeof(Buffer);
    File = nullptr;
    Check("File->Open below the folder",
          Dir->Open(Dir, "check.txt", SF_FILE_READ, &File) == SF_SUCCESS && File);
    if (File)
    {
        Check("SetPosition + Read read the text back",
              File->SetPosition(File, 8) == SF_SUCCESS &&
              File->Read(File, Buffer, &Size) == SF_SUCCESS &&
              Size == Len - 8 && SameBytes(Buffer, Text + 8, Size));
        Size = sizeof(Buffer);
        Check("Read at the end gives 0 bytes",
              File->Read(File, Buffer, &Size) == SF_SUCCESS && Size == 0);
        File->Close(File);
    }
    SfFile* Out = nullptr;
    Check("a folder does not lead above itself",
          Dir->Open(Dir, "../sdkcheck/check.txt", SF_FILE_READ, &Out) == SF_ACCESS_DENIED);
    Check("nor does an absolute path",
          Dir->Open(Dir, "/apps/sdkcheck", SF_FILE_READ, &Out) == SF_ACCESS_DENIED);
    Dir->Close(Dir);

    Check("data:/.. is SF_ACCESS_DENIED",
          Files->Open(Files, "data:/../other/x", SF_FILE_READ, &Out) == SF_ACCESS_DENIED);
    Check("a missing file is SF_NOT_FOUND",
          Files->Open(Files, "data:/missing.txt", SF_FILE_READ, &Out) == SF_NOT_FOUND);
    Check("CREATE_NEW of an existing file is SF_ALREADY_EXISTS",
          Files->Open(Files, "data:/check.txt", SF_FILE_WRITE | SF_FILE_CREATE_NEW, &Out) ==
          SF_ALREADY_EXISTS);
    Check("an unknown root is SF_NOT_FOUND",
          Files->Open(Files, "disk:/sfos/KERNEL.BIN", SF_FILE_READ, &Out) == SF_NOT_FOUND);
    Check("a path without a root is SF_INVALID_PARAMETER",
          Files->Open(Files, "/apps/sdkcheck", SF_FILE_READ, &Out) == SF_INVALID_PARAMETER);

    // tmp:/ - a unique name, then the same file by that name.
    char Path[32];
    File = nullptr;
    Check("CreateUnique makes a file in tmp:/",
          Files->CreateUnique(Files, &File, Path, sizeof(Path)) == SF_SUCCESS && File &&
          SameBytes(Path, "tmp:/", 5));
    if (!File)
        return;
    Size = Len;
    File->Write(File, Text, &Size);
    File->Close(File);
    SfFile* Again = nullptr;
    Check("... and it opens by the path it got",
          Files->Open(Files, Path, SF_FILE_READ, &Again) == SF_SUCCESS && Again);
    if (Again)
    {
        Size = sizeof(Buffer);
        Check("... with what was written",
              Again->Read(Again, Buffer, &Size) == SF_SUCCESS && Size == Len);
        Again->Close(Again);
    }
    SfFile* Other = nullptr;
    char Path2[32];
    Check("the next CreateUnique gets another name",
          Files->CreateUnique(Files, &Other, Path2, sizeof(Path2)) == SF_SUCCESS &&
          !SameText(Path, Path2));
    if (Other)
        Other->Close(Other);
}

extern "C" SfStatus SfMain(SfApp* App, SfSystem* Sys)
{
    if (!Sys || !Sys->Console)
        return SF_INVALID_PARAMETER;        // nothing to report through
    Con = Sys->Console;

    Print("sdkcheck - the tables SfMain gets\n");

    Check("SfSystem: signature, revision 1.x, size",
          HeaderOk(&Sys->Hdr, SF_SYSTEM_SIGNATURE, sizeof(SfSystem)));
    Check("SfSystem has the Console field", SF_HAS_FIELD(Sys, SfSystem, Console));
    Check("SfConsole: signature, revision 1.x, size",
          HeaderOk(&Con->Hdr, SF_CONSOLE_SIGNATURE, sizeof(SfConsole)));
    Check("SfApp: signature, revision 1.x, size",
          App && HeaderOk(&App->Hdr, SF_APP_SIGNATURE, sizeof(SfApp)));
    Check("App->Name is the program's name", App && SameText(App->Name, "sdkcheck"));

    Check("Print returns SF_SUCCESS", Con->Print(Con, "") == SF_SUCCESS);

    // Longer than the kernel's copy chunk (1 KiB): printed in pieces.
    static char Long[2601];
    for (int i = 0; i < 2600; i++)
        Long[i] = (i % 100 == 99) ? '\n' : (char)('a' + i % 26);
    Long[2600] = '\0';
    Check("a 2600-byte Print works", Con->Print(Con, Long) == SF_SUCCESS);

    Check("Print of an unmapped address is SF_INVALID_PARAMETER",
          Con->Print(Con, (const char*)0x1000) == SF_INVALID_PARAMETER);

    Check("SfFiles: signature, revision 1.x, size",
          SF_HAS_FIELD(Sys, SfSystem, Files) && Sys->Files &&
          HeaderOk(&Sys->Files->Hdr, SF_FILES_SIGNATURE, sizeof(SfFiles)));
    if (SF_HAS_FIELD(Sys, SfSystem, Files) && Sys->Files)
        CheckRoots(Sys->Files);

    Print("sdkcheck: ");
    PrintNumber(Passed);
    Print(" passed, ");
    PrintNumber(Failed);
    Print(" failed\n");

    return Failed ? (SF_ERROR_BIT | Failed) : SF_SUCCESS;
}
