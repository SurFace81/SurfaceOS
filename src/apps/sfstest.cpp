// sfstest: files through the SurfaceOS SDK.
//
//   sfstest          the suite, in the program's own folder data:/
//   sfstest verify   read back what the last run left in data:/persist.bin
//                    (after a restart: the data must have reached the disk)
//
// Every check prints [ ok ]/[FAIL]; the exit status is the number of
// failures (0: all passed).

#include <sfos.h>

static SfConsole* Con;
static SfFiles*   Files;
static SfMemory*  Memory;
static uint64_t   Passed;
static uint64_t   Failed;

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

static void Section(const char* Name)
{
    Print("\n");
    Print(Name);
    Print("\n");
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

static uint64_t TextLength(const char* Text)
{
    uint64_t n = 0;
    while (Text[n])
        n++;
    return n;
}

// Byte i of the test pattern with seed Seed: reads back exactly.
static uint8_t PatternByte(uint64_t Seed, uint64_t i)
{
    return (uint8_t)((i * 131 + Seed * 7 + (i >> 9)) & 0xFF);
}

static void Fill(uint8_t* Buffer, uint64_t Seed, uint64_t Start, uint64_t Size)
{
    for (uint64_t i = 0; i < Size; i++)
        Buffer[i] = PatternByte(Seed, Start + i);
}

static bool Matches(const uint8_t* Buffer, uint64_t Seed, uint64_t Start, uint64_t Size)
{
    for (uint64_t i = 0; i < Size; i++)
        if (Buffer[i] != PatternByte(Seed, Start + i))
            return false;
    return true;
}

static SfFile* Open(const char* Path, uint64_t Mode)
{
    SfFile* File = nullptr;
    if (SF_ERROR(Files->Open(Files, Path, Mode, &File)))
        return nullptr;
    return File;
}

static SfStatus OpenStatus(const char* Path, uint64_t Mode)
{
    SfFile*  File = nullptr;
    SfStatus Status = Files->Open(Files, Path, Mode, &File);
    if (!SF_ERROR(Status))
        File->Close(File);
    return Status;
}

static bool WriteAll(SfFile* File, const void* Buffer, uint64_t Size)
{
    uint64_t Done = Size;
    return File->Write(File, Buffer, &Done) == SF_SUCCESS && Done == Size;
}

// Read up to Size bytes; how many came, or ~0 on an error.
static uint64_t ReadSome(SfFile* File, void* Buffer, uint64_t Size)
{
    uint64_t Done = Size;
    return File->Read(File, Buffer, &Done) == SF_SUCCESS ? Done : ~0ULL;
}

// Write Text as the whole content of Path.
static bool WriteFile(const char* Path, const char* Text)
{
    SfFile* File = Open(Path, SF_FILE_WRITE | SF_FILE_CREATE | SF_FILE_TRUNCATE);
    if (!File)
        return false;
    bool Ok = WriteAll(File, Text, TextLength(Text));
    return File->Close(File) == SF_SUCCESS && Ok;
}

// Is the whole content of Path exactly Text?
static bool FileIs(const char* Path, const char* Text)
{
    SfFile* File = Open(Path, SF_FILE_READ);
    if (!File)
        return false;
    char Buffer[128];
    uint64_t n = ReadSome(File, Buffer, sizeof(Buffer) - 1);
    File->Close(File);
    if (n == ~0ULL)
        return false;
    Buffer[n] = '\0';
    return SameText(Buffer, Text);
}

// ===========================================================================

static void TestBasic()
{
    Section("open / write / read / close");

    const char* Text = "hello from sfstest\n";
    SfFile* File = Open("data:/basic.txt", SF_FILE_WRITE | SF_FILE_CREATE | SF_FILE_TRUNCATE);
    Check("create data:/basic.txt", File != nullptr);
    if (!File)
        return;
    Check("Write writes it all", WriteAll(File, Text, TextLength(Text)));
    char Buffer[64];
    uint64_t Size = sizeof(Buffer);
    Check("Read of a file opened for writing only is SF_ACCESS_DENIED",
          File->Read(File, Buffer, &Size) == SF_ACCESS_DENIED);
    Check("Close", File->Close(File) == SF_SUCCESS);

    File = Open("data:/basic.txt", SF_FILE_READ);
    Check("reopen for reading", File != nullptr);
    if (!File)
        return;
    uint64_t n = ReadSome(File, Buffer, sizeof(Buffer) - 1);
    Check("Read gives the whole content", n == TextLength(Text));
    Buffer[n == ~0ULL ? 0 : n] = '\0';
    Check("... unchanged", SameText(Buffer, Text));
    Check("Read at the end gives 0 bytes", ReadSome(File, Buffer, sizeof(Buffer)) == 0);
    Size = 1;
    Check("Write to a file opened for reading only is SF_ACCESS_DENIED",
          File->Write(File, "x", &Size) == SF_ACCESS_DENIED);
    File->Close(File);
}

static void TestBigFile()
{
    Section("3 MiB file, odd block sizes, random positions");

    const uint64_t Total = 3 * 1024 * 1024;
    const uint64_t Block = 4097;
    const uint64_t Chunk = 64 * 1024;

    uint8_t* Buffer = nullptr;
    if (SF_ERROR(Memory->Allocate(Memory, Chunk, (void**)&Buffer)))
    {
        Check("a 64 KiB buffer from the heap", false);
        return;
    }

    SfFile* File = Open("data:/big.bin", SF_FILE_WRITE | SF_FILE_CREATE | SF_FILE_TRUNCATE);
    Check("create data:/big.bin", File != nullptr);
    if (!File)
    {
        Memory->Free(Memory, Buffer);
        return;
    }
    bool Ok = true;
    for (uint64_t Off = 0; Ok && Off < Total; Off += Block)
    {
        uint64_t n = Total - Off < Block ? Total - Off : Block;
        Fill(Buffer, 1, Off, n);
        Ok = WriteAll(File, Buffer, n);
    }
    Check("3 MiB written in 4097-byte blocks", Ok);
    uint64_t Position = 0;
    Check("GetPosition is 3 MiB", File->GetPosition(File, &Position) == SF_SUCCESS &&
                                  Position == Total);
    File->Close(File);

    File = Open("data:/big.bin", SF_FILE_READ);
    Check("reopen big.bin", File != nullptr);
    if (!File)
    {
        Memory->Free(Memory, Buffer);
        return;
    }
    uint64_t Seed = 12345;
    Ok = true;
    for (int i = 0; Ok && i < 40; i++)
    {
        Seed = Seed * 6364136223846793005ULL + 1442695040888963407ULL;
        uint64_t Off = (Seed >> 20) % (Total - 1000);
        Ok = File->SetPosition(File, Off) == SF_SUCCESS &&
             ReadSome(File, Buffer, 1000) == 1000 && Matches(Buffer, 1, Off, 1000);
    }
    Check("40 reads of 1000 bytes at random positions match", Ok);

    File->SetPosition(File, 0);
    uint64_t Read = 0;
    Ok = true;
    for (;;)
    {
        uint64_t n = ReadSome(File, Buffer, Chunk);
        if (n == ~0ULL)
            Ok = false;
        if (n == ~0ULL || n == 0)
            break;
        Ok = Ok && Matches(Buffer, 1, Read, n);
        Read += n;
    }
    Check("reading all of it in 64 KiB pieces matches", Ok && Read == Total);
    File->Close(File);
    Memory->Free(Memory, Buffer);
}

static void TestModes()
{
    Section("open modes");

    WriteFile("data:/modes.txt", "keep me");
    Check("CREATE_NEW of an existing file is SF_ALREADY_EXISTS",
          OpenStatus("data:/modes.txt", SF_FILE_WRITE | SF_FILE_CREATE_NEW) == SF_ALREADY_EXISTS);
    Check("CREATE of an existing file keeps its content",
          OpenStatus("data:/modes.txt", SF_FILE_WRITE | SF_FILE_CREATE) == SF_SUCCESS &&
          FileIs("data:/modes.txt", "keep me"));
    Check("TRUNCATE empties it",
          OpenStatus("data:/modes.txt", SF_FILE_WRITE | SF_FILE_TRUNCATE) == SF_SUCCESS &&
          FileIs("data:/modes.txt", ""));
    Check("without CREATE a missing file is SF_NOT_FOUND",
          OpenStatus("data:/nothing.txt", SF_FILE_READ | SF_FILE_WRITE) == SF_NOT_FOUND);
    Check("a mode with neither READ nor WRITE is SF_INVALID_PARAMETER",
          OpenStatus("data:/modes.txt", SF_FILE_CREATE) == SF_INVALID_PARAMETER);
    Check("an unknown mode bit is SF_INVALID_PARAMETER",
          OpenStatus("data:/modes.txt", SF_FILE_READ | 0x100) == SF_INVALID_PARAMETER);

    SfFile* Dir = Open("data:/", SF_FILE_READ);
    Check("the folder opens for reading", Dir != nullptr);
    if (Dir)
    {
        char Buffer[16];
        uint64_t Size = sizeof(Buffer);
        Check("... but Read of a folder is SF_INVALID_PARAMETER",
              Dir->Read(Dir, Buffer, &Size) == SF_INVALID_PARAMETER);
        Check("... and SetPosition on it is SF_UNSUPPORTED",
              Dir->SetPosition(Dir, 1) == SF_UNSUPPORTED);
        SfFile* File = nullptr;
        Check("File->Open below the folder",
              Dir->Open(Dir, "modes.txt", SF_FILE_READ, &File) == SF_SUCCESS && File);
        if (File)
            File->Close(File);
        Dir->Close(Dir);
    }
    Check("a folder does not open for writing",
          OpenStatus("data:/", SF_FILE_WRITE) == SF_INVALID_PARAMETER);
}

static void TestPositions()
{
    Section("positions and holes");

    SfFile* File = Open("data:/hole.bin",
                        SF_FILE_READ | SF_FILE_WRITE | SF_FILE_CREATE | SF_FILE_TRUNCATE);
    Check("create hole.bin", File != nullptr);
    if (!File)
        return;
    uint64_t Position = 0;
    Check("SetPosition past the end, then Write",
          File->SetPosition(File, 5000) == SF_SUCCESS && WriteAll(File, "x", 1) &&
          File->GetPosition(File, &Position) == SF_SUCCESS && Position == 5001);

    uint8_t Buffer[5001];
    File->SetPosition(File, 0);
    bool Ok = ReadSome(File, Buffer, sizeof(Buffer)) == sizeof(Buffer) && Buffer[5000] == 'x';
    for (uint64_t i = 0; Ok && i < 5000; i++)
        Ok = Buffer[i] == 0;
    Check("the hole reads as zeroes", Ok);
    Check("a Read past the end gives 0 bytes",
          File->SetPosition(File, 9000) == SF_SUCCESS && ReadSome(File, Buffer, 10) == 0);

    File->SetPosition(File, 2);
    WriteAll(File, "ab", 2);
    File->SetPosition(File, 0);
    Check("a Write in the middle changes just those bytes",
          ReadSome(File, Buffer, 5) == 5 && Buffer[0] == 0 && Buffer[2] == 'a' &&
          Buffer[3] == 'b' && Buffer[4] == 0);
    File->Close(File);
}

static void TestLongNames()
{
    Section("long file names");

    Check("a name with spaces and capitals",
          WriteFile("data:/Long File Name.txt", "long name") &&
          FileIs("data:/Long File Name.txt", "long name"));
    Check("found again in another case", FileIs("data:/long file name.TXT", "long name"));

    char Path[64] = "data:/a file with a rather long name, number 00.dat";
    uint64_t Digits = TextLength(Path) - 6;     // the "00" before ".dat"
    bool Ok = true;
    for (int i = 0; Ok && i < 100; i++)
    {
        Path[Digits]     = (char)('0' + i / 10);
        Path[Digits + 1] = (char)('0' + i % 10);
        Ok = WriteFile(Path, Path + Digits);
    }
    for (int i = 0; Ok && i < 100; i++)
    {
        Path[Digits]     = (char)('0' + i / 10);
        Path[Digits + 1] = (char)('0' + i % 10);
        Ok = FileIs(Path, Path + Digits);
    }
    Check("100 long-named files in one folder, each with its own content", Ok);
}

static void TestSandbox()
{
    Section("roots: nothing leads out");

    WriteFile("data:/inside.txt", "inside");
    Check("'.' and repeated slashes are fine",
          FileIs("data:/./inside.txt", "inside") && FileIs("data://inside.txt", "inside"));
    Check("data:/.. is SF_ACCESS_DENIED",
          OpenStatus("data:/..", SF_FILE_READ) == SF_ACCESS_DENIED);
    Check("data:/../<another program>/ is SF_ACCESS_DENIED",
          OpenStatus("data:/../sdkcheck/check.txt", SF_FILE_READ) == SF_ACCESS_DENIED);
    Check("data:/./../.. is SF_ACCESS_DENIED",
          OpenStatus("data:/./../..", SF_FILE_READ) == SF_ACCESS_DENIED);
    Check("creating above the root is SF_ACCESS_DENIED",
          OpenStatus("data:/../escape.txt", SF_FILE_WRITE | SF_FILE_CREATE) == SF_ACCESS_DENIED);

    SfFile* Dir = Open("data:/", SF_FILE_READ);
    if (Dir)
    {
        SfFile* Out = nullptr;
        Check("a folder: '..' is SF_ACCESS_DENIED",
              Dir->Open(Dir, "../sfstest/inside.txt", SF_FILE_READ, &Out) == SF_ACCESS_DENIED);
        Check("a folder: an absolute path is SF_ACCESS_DENIED",
              Dir->Open(Dir, "/apps/sfstest", SF_FILE_READ, &Out) == SF_ACCESS_DENIED);
        Dir->Close(Dir);
    }

    Check("an unknown root is SF_NOT_FOUND",
          OpenStatus("disk:/sfos/KERNEL.BIN", SF_FILE_READ) == SF_NOT_FOUND);
    Check("arg1: without arguments is SF_NOT_FOUND",
          OpenStatus("arg1:", SF_FILE_READ) == SF_NOT_FOUND);
    Check("a path without a root is SF_INVALID_PARAMETER",
          OpenStatus("/files/sfstest/inside.txt", SF_FILE_READ) == SF_INVALID_PARAMETER);
    Check("an empty root name is SF_INVALID_PARAMETER",
          OpenStatus(":/inside.txt", SF_FILE_READ) == SF_INVALID_PARAMETER);

    Section("tmp:/ - shared, not listable");

    Check("tmp:/ itself does not open (no listing)",
          OpenStatus("tmp:/", SF_FILE_READ) == SF_ACCESS_DENIED);
    Check("... nor through '.'", OpenStatus("tmp:/.", SF_FILE_READ) == SF_ACCESS_DENIED);
    Check("tmp:/.. is SF_ACCESS_DENIED", OpenStatus("tmp:/..", SF_FILE_READ) == SF_ACCESS_DENIED);

    char First[32], Second[32];
    SfFile* A = nullptr;
    SfFile* B = nullptr;
    Check("CreateUnique twice gives two names",
          Files->CreateUnique(Files, &A, First, sizeof(First)) == SF_SUCCESS &&
          Files->CreateUnique(Files, &B, Second, sizeof(Second)) == SF_SUCCESS &&
          !SameText(First, Second));
    if (A && B)
    {
        WriteAll(A, "first", 5);
        WriteAll(B, "second", 6);
        A->Close(A);
        B->Close(B);
        Check("each opens by its path with its own content",
              FileIs(First, "first") && FileIs(Second, "second"));
        Check("a unique file can be created anew only by CreateUnique",
              OpenStatus(First, SF_FILE_WRITE | SF_FILE_CREATE_NEW) == SF_ALREADY_EXISTS);
    }
    char Tiny[4];
    SfFile* C = nullptr;
    Check("CreateUnique into too small a path buffer is SF_INVALID_PARAMETER",
          Files->CreateUnique(Files, &C, Tiny, sizeof(Tiny)) == SF_INVALID_PARAMETER);
    Check("a file of a chosen name in tmp:/ works too",
          WriteFile("tmp:/sfstest.txt", "chosen") && FileIs("tmp:/sfstest.txt", "chosen"));
}

// ===========================================================================
// Persistence: the suite leaves persist.bin behind; `sfstest verify` after
// a restart reads it back.

static const uint64_t PersistSize = 200 * 1024 + 17;
static const uint64_t PersistSeed = 7;

static void WritePersist()
{
    Section("for `sfstest verify`: data:/persist.bin");

    uint8_t Buffer[4096];
    SfFile* File = Open("data:/persist.bin", SF_FILE_WRITE | SF_FILE_CREATE | SF_FILE_TRUNCATE);
    bool Ok = File != nullptr;
    for (uint64_t Off = 0; Ok && Off < PersistSize; Off += sizeof(Buffer))
    {
        uint64_t n = PersistSize - Off < sizeof(Buffer) ? PersistSize - Off : sizeof(Buffer);
        Fill(Buffer, PersistSeed, Off, n);
        Ok = WriteAll(File, Buffer, n);
    }
    if (File)
        Ok = File->Close(File) == SF_SUCCESS && Ok;
    Check("persist.bin written", Ok);
    Ok = WriteFile("data:/persist.txt", "survives a restart");
    Check("persist.txt written", Ok);
}

static void Verify()
{
    Section("after a restart");

    uint8_t Buffer[4096];
    SfFile* File = Open("data:/persist.bin", SF_FILE_READ);
    Check("persist.bin is there", File != nullptr);
    if (File)
    {
        uint64_t Read = 0;
        bool Ok = true;
        for (;;)
        {
            uint64_t n = ReadSome(File, Buffer, sizeof(Buffer));
            if (n == ~0ULL)
                Ok = false;
            if (n == ~0ULL || n == 0)
                break;
            Ok = Ok && Matches(Buffer, PersistSeed, Read, n);
            Read += n;
        }
        File->Close(File);
        Check("... with every byte", Ok && Read == PersistSize);
    }
    Check("persist.txt is there", FileIs("data:/persist.txt", "survives a restart"));
    Check("the 100 long-named files are there",
          FileIs("data:/a file with a rather long name, number 42.dat", "42.dat"));
    Check("tmp:/ was emptied at boot",
          OpenStatus("tmp:/sfstest.txt", SF_FILE_READ) == SF_NOT_FOUND);
}

extern "C" SfStatus SfMain(SfApp* App, SfSystem* Sys)
{
    Con    = Sys->Console;
    Files  = Sys->Files;
    Memory = Sys->Memory;

    bool VerifyMode = App->ArgCount >= 2 && SameText(App->Args[1], "verify");
    const char* Name = VerifyMode ? "sfstest verify" : "sfstest";
    Print(Name);
    Print(" - files through the SDK\n");

    if (VerifyMode)
        Verify();
    else
    {
        TestBasic();
        TestBigFile();
        TestModes();
        TestPositions();
        TestLongNames();
        TestSandbox();
        WritePersist();
    }

    Print("\n");
    Print(Name);
    Print(": ");
    PrintNumber(Passed);
    Print(" passed, ");
    PrintNumber(Failed);
    Print(" failed\n");
    return Failed ? (SF_ERROR_BIT | Failed) : SF_SUCCESS;
}
