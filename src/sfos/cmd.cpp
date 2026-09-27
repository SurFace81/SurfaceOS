// cmd: the console (/sfos/CMD.BIN), one on every screen.
//
// A program like any other, started by the kernel with the admin right on
// each screen and started again whenever it ends. It reads a line and
// either does it itself (the commands below) or runs the program it names
// - from /apps, or by its path - handing it the keys until it ends; a last
// `&` runs it in the background instead.
//
// Paths: the current folder is an absolute path of the whole disk
// ("/apps"), and every path typed is taken from there; the files are
// reached through the root disk:/ (sfos/admin.h). The current folder is
// kept open, so a volume the console stands on counts as in use.

#include <sfos.h>

static SfSystem*  Sys;
static SfConsole* Con;
static SfAdmin*   Admin;

static const uint64_t PATH_SIZE = 512;
static char    Cwd[PATH_SIZE] = "/";
static SfFile* CwdFile;

// --- text ------------------------------------------------------------------

static void Print(const char* Text)
{
    Con->Print(Con, Text);
}

static void PrintChar(char C)
{
    char Text[2] = { C, 0 };
    Print(Text);
}

static void PrintNumber(uint64_t Value, int Width = 0, char Pad = ' ')
{
    char Buffer[24];
    int  Pos = 23;
    Buffer[Pos] = '\0';
    do
    {
        Buffer[--Pos] = (char)('0' + Value % 10);
        Value /= 10;
    } while (Value);
    while (23 - Pos < Width)
        Buffer[--Pos] = Pad;
    Print(Buffer + Pos);
}

static void PrintHex(uint64_t Value, int Digits)
{
    char Buffer[17];
    for (int i = Digits - 1; i >= 0; i--, Value >>= 4)
        Buffer[i] = "0123456789ABCDEF"[Value & 0xF];
    Buffer[Digits] = '\0';
    Print(Buffer);
}

static uint64_t Length(const char* S)
{
    uint64_t n = 0;
    while (S[n])
        n++;
    return n;
}

static bool Same(const char* A, const char* B)
{
    while (*A && *A == *B)
        A++, B++;
    return *A == *B;
}

static void Copy(char* Dst, const char* Src, uint64_t Size)
{
    uint64_t i = 0;
    for (; Src[i] && i + 1 < Size; i++)
        Dst[i] = Src[i];
    Dst[i] = '\0';
}

static uint64_t ParseNumber(const char* Text, bool* Ok)
{
    uint64_t Value = 0;
    *Ok = *Text != '\0';
    for (; *Text; Text++)
    {
        if (*Text < '0' || *Text > '9')
            *Ok = false;
        Value = Value * 10 + (uint64_t)(*Text - '0');
    }
    return Value;
}

// Why something failed, in words.
static const char* Why(SfStatus Status)
{
    switch (Status)
    {
        case SF_NOT_FOUND:        return "not found";
        case SF_ACCESS_DENIED:    return "not allowed";
        case SF_ALREADY_EXISTS:   return "already exists";
        case SF_IN_USE:           return "in use";
        case SF_OUT_OF_RESOURCES: return "out of memory or space";
        case SF_INVALID_PARAMETER:return "invalid";
        case SF_DEVICE_ERROR:     return "device error";
        default:                  return "failed";
    }
}

static void Fail(const char* What, const char* Name, SfStatus Status)
{
    Print(What);
    Print(" ");
    Print(Name);
    Print(": ");
    Print(Why(Status));
    Print("\n");
}

// --- paths -----------------------------------------------------------------

// Path, taken from the current folder, as a clean absolute path in Out
// ("/", "/apps", ...): ".", ".." and double slashes resolved.
static void Absolute(const char* Path, char* Out)
{
    char Buffer[PATH_SIZE * 2];
    uint64_t n = 0;
    if (Path[0] != '/')
    {
        for (const char* c = Cwd; *c && n < sizeof(Buffer) - 2; c++)
            Buffer[n++] = *c;
        Buffer[n++] = '/';
    }
    for (const char* c = Path; *c && n < sizeof(Buffer) - 1; c++)
        Buffer[n++] = *c;
    Buffer[n] = '\0';

    // Walk the parts; Out holds what is left of them.
    uint64_t o = 0;
    Out[o++] = '/';
    for (char* p = Buffer; *p;)
    {
        while (*p == '/')
            p++;
        char* Part = p;
        while (*p && *p != '/')
            p++;
        uint64_t Len = (uint64_t)(p - Part);
        if (Len == 0 || (Len == 1 && Part[0] == '.'))
            continue;
        if (Len == 2 && Part[0] == '.' && Part[1] == '.')
        {
            while (o > 1 && Out[o - 1] != '/')
                o--;
            if (o > 1)
                o--;                    // the slash before it
            continue;
        }
        if (o > 1 && o < PATH_SIZE - 1)
            Out[o++] = '/';
        for (uint64_t i = 0; i < Len && o < PATH_SIZE - 1; i++)
            Out[o++] = Part[i];
    }
    Out[o] = '\0';
}

// "disk:" + the absolute path: what the SDK calls take.
static void DiskPath(const char* Path, char* Out)
{
    char Abs[PATH_SIZE];
    Absolute(Path, Abs);
    Copy(Out, "disk:", PATH_SIZE + 8);
    Copy(Out + 5, Abs, PATH_SIZE);
}

static SfStatus Open(const char* Path, uint64_t Mode, SfFile** Out)
{
    char Full[PATH_SIZE + 8];
    DiskPath(Path, Full);
    return Sys->Files->Open(Sys->Files, Full, Mode, Out);
}

static bool IsFolder(SfFile* File)
{
    SfDirEntry Info;
    return File->GetInfo(File, &Info) == SF_SUCCESS && (Info.Flags & SF_DIR_ENTRY_FOLDER);
}

// --- commands --------------------------------------------------------------

typedef void (*Command)(const char** Args, uint64_t Count);

static void Help(const char**, uint64_t);

static void Cls(const char**, uint64_t)
{
    Con->Clear(Con);
}

static void Pwd(const char**, uint64_t)
{
    Print(Cwd);
    Print("\n");
}

static void Cd(const char** Args, uint64_t Count)
{
    if (Count < 2)
        return Pwd(Args, Count);
    SfFile* Dir = nullptr;
    SfStatus Status = Open(Args[1], SF_FILE_READ, &Dir);
    if (SF_ERROR(Status) || !IsFolder(Dir))
    {
        if (Dir)
            Dir->Close(Dir);
        Fail("cd:", Args[1], SF_ERROR(Status) ? Status : SF_NOT_FOUND);
        return;
    }
    if (CwdFile)
        CwdFile->Close(CwdFile);
    CwdFile = Dir;
    char Abs[PATH_SIZE];
    Absolute(Args[1], Abs);
    Copy(Cwd, Abs, sizeof(Cwd));
}

static void PrintDate(const SfDateTime* T)
{
    PrintNumber(T->Day, 2, '0');    PrintChar('.');
    PrintNumber(T->Month, 2, '0');  PrintChar('.');
    PrintNumber(T->Year, 4, '0');   PrintChar(' ');
    PrintNumber(T->Hour, 2, '0');   PrintChar(':');
    PrintNumber(T->Minute, 2, '0');
}

static void PrintEntry(const SfDirEntry* E)
{
    Print("  ");
    PrintDate(&E->Modified);
    if (E->Flags & SF_DIR_ENTRY_FOLDER)
        Print("       <DIR>  ");
    else
    {
        Print("  ");
        PrintNumber(E->Size, 10);
        Print("  ");
    }
    Print(E->Name);
    Print("\n");
}

static void Ls(const char** Args, uint64_t Count)
{
    const char* Path = Count > 1 ? Args[1] : ".";
    SfFile* Dir = nullptr;
    SfStatus Status = Open(Path, SF_FILE_READ, &Dir);
    if (SF_ERROR(Status))
        return Fail("ls:", Path, Status);
    if (!IsFolder(Dir))
    {
        SfDirEntry Info;
        Dir->GetInfo(Dir, &Info);
        Copy(Info.Name, Path, sizeof(Info.Name));
        PrintEntry(&Info);
    }
    else
    {
        SfDirEntry Entry;
        while ((Status = Dir->ReadDir(Dir, &Entry)) == SF_SUCCESS)
            PrintEntry(&Entry);
        if (Status != SF_END_OF_FILE)
            Fail("ls:", Path, Status);
    }
    Dir->Close(Dir);
}

static bool Printable(uint8_t C)
{
    return (C >= 0x20 && C <= 0x7E) || C == '\n' || C == '\r' || C == '\t';
}

// The start of a file, up to Size bytes; its length, or -1 (reported).
static sint64_t ReadStart(const char* What, const char* Path, uint8_t* Buffer, uint64_t Size)
{
    SfFile* File = nullptr;
    SfStatus Status = Open(Path, SF_FILE_READ, &File);
    if (!SF_ERROR(Status) && IsFolder(File))
        Status = SF_INVALID_PARAMETER;
    uint64_t n = Size;
    if (!SF_ERROR(Status))
        Status = File->Read(File, Buffer, &n);
    if (File)
        File->Close(File);
    if (SF_ERROR(Status))
    {
        Fail(What, Path, Status);
        return -1;
    }
    return (sint64_t)n;
}

static void Cat(const char** Args, uint64_t Count)
{
    if (Count < 2)
        return Print("Usage: cat <file>\n");
    const uint64_t Max = 4096;
    static uint8_t Buffer[Max + 1];
    sint64_t n = ReadStart("cat:", Args[1], Buffer, Max);
    if (n < 0)
        return;
    for (sint64_t i = 0; i < n; i++)
        if (!Printable(Buffer[i]))
        {
            Print("Binary file, use xxd to view\n");
            return;
        }
    Buffer[n] = 0;
    Print((const char*)Buffer);
    if ((uint64_t)n == Max)
        Print("\n[the first 4096 bytes]");
    Print("\n");
    PrintNumber((uint64_t)n);
    Print(" bytes\n");
}

static void Xxd(const char** Args, uint64_t Count)
{
    if (Count < 2)
        return Print("Usage: xxd <file>\n");
    const uint64_t Max = 512;
    uint8_t Buffer[Max];
    sint64_t n = ReadStart("xxd:", Args[1], Buffer, Max);
    if (n < 0)
        return;
    for (sint64_t Off = 0; Off < n; Off += 16)
    {
        PrintHex((uint64_t)Off, 8);
        Print("  ");
        for (sint64_t i = 0; i < 16; i++)
        {
            if (Off + i < n)
            {
                PrintHex(Buffer[Off + i], 2);
                PrintChar(' ');
            }
            else
                Print("   ");
            if (i == 7)
                PrintChar(' ');
        }
        Print(" |");
        for (sint64_t i = 0; i < 16 && Off + i < n; i++)
        {
            uint8_t b = Buffer[Off + i];
            PrintChar(b >= 0x20 && b <= 0x7E ? (char)b : '.');
        }
        Print("|\n");
    }
    if ((uint64_t)n == Max)
        Print("[the first 512 bytes]\n");
    PrintNumber((uint64_t)n);
    Print(" bytes\n");
}

static void Write(const char** Args, uint64_t Count)
{
    if (Count < 3)
        return Print("Usage: write <file> <text...>\n");
    char Data[512];
    uint64_t n = 0;
    for (uint64_t i = 2; i < Count; i++)
    {
        if (i > 2 && n < sizeof(Data))
            Data[n++] = ' ';
        for (const char* c = Args[i]; *c && n < sizeof(Data); c++)
            Data[n++] = *c;
    }

    SfFile* File = nullptr;
    SfStatus Status = Open(Args[1], SF_FILE_WRITE | SF_FILE_CREATE | SF_FILE_TRUNCATE, &File);
    uint64_t Written = n;
    if (!SF_ERROR(Status))
        Status = File->Write(File, Data, &Written);
    if (File)
        File->Close(File);
    if (SF_ERROR(Status))
        return Fail("write:", Args[1], Status);
    PrintNumber(Written);
    Print(" bytes written to ");
    Print(Args[1]);
    Print("\n");
}

// Copy file From to To (made or emptied). SF_SUCCESS or why not.
static SfStatus CopyFile(const char* From, const char* To)
{
    SfFile* Src = nullptr;
    SfFile* Dst = nullptr;
    SfStatus Status = Open(From, SF_FILE_READ, &Src);
    if (!SF_ERROR(Status) && IsFolder(Src))
        Status = SF_INVALID_PARAMETER;
    if (!SF_ERROR(Status))
        Status = Open(To, SF_FILE_WRITE | SF_FILE_CREATE | SF_FILE_TRUNCATE, &Dst);

    static uint8_t Buffer[32768];
    while (!SF_ERROR(Status))
    {
        uint64_t n = sizeof(Buffer);
        Status = Src->Read(Src, Buffer, &n);
        if (SF_ERROR(Status) || n == 0)
            break;
        uint64_t w = n;
        Status = Dst->Write(Dst, Buffer, &w);
        if (!SF_ERROR(Status) && w != n)
            Status = SF_OUT_OF_RESOURCES;
    }
    if (Dst)
        Dst->Close(Dst);
    if (Src)
        Src->Close(Src);
    return Status;
}

static void Cp(const char** Args, uint64_t Count)
{
    if (Count < 3)
        return Print("Usage: cp <source> <destination>\n");
    SfStatus Status = CopyFile(Args[1], Args[2]);
    if (SF_ERROR(Status))
        return Fail("cp:", Args[1], Status);
    Print("Copied ");
    Print(Args[1]);
    Print(" -> ");
    Print(Args[2]);
    Print("\n");
}

static void Mv(const char** Args, uint64_t Count)
{
    if (Count < 3)
        return Print("Usage: mv <source> <destination>\n");
    char From[PATH_SIZE + 8], To[PATH_SIZE + 8];
    DiskPath(Args[1], From);
    DiskPath(Args[2], To);
    SfStatus Status = Sys->Files->Rename(Sys->Files, From, To);
    // Another volume: copy it over, then remove the original.
    if (Status == SF_ACCESS_DENIED)
    {
        Status = CopyFile(Args[1], Args[2]);
        if (!SF_ERROR(Status))
            Status = Sys->Files->Delete(Sys->Files, From);
    }
    if (SF_ERROR(Status))
        return Fail("mv:", Args[1], Status);
    Print("Moved ");
    Print(Args[1]);
    Print(" -> ");
    Print(Args[2]);
    Print("\n");
}

static void Mkdir(const char** Args, uint64_t Count)
{
    if (Count < 2)
        return Print("Usage: mkdir <folder>\n");
    char Full[PATH_SIZE + 8];
    DiskPath(Args[1], Full);
    SfStatus Status = Sys->Files->CreateDirectory(Sys->Files, Full);
    if (SF_ERROR(Status))
        Fail("mkdir:", Args[1], Status);
}

// rm a file, rmdir an empty folder: both are Delete.
static void Remove(const char** Args, uint64_t Count, bool Folder)
{
    if (Count < 2)
        return Print(Folder ? "Usage: rmdir <folder>\n" : "Usage: rm <file>\n");
    SfFile* File = nullptr;
    SfStatus Status = Open(Args[1], SF_FILE_READ, &File);
    bool IsDir = !SF_ERROR(Status) && IsFolder(File);
    if (File)
        File->Close(File);
    if (!SF_ERROR(Status) && IsDir != Folder)
        Status = SF_INVALID_PARAMETER;
    if (!SF_ERROR(Status))
    {
        char Full[PATH_SIZE + 8];
        DiskPath(Args[1], Full);
        Status = Sys->Files->Delete(Sys->Files, Full);
    }
    if (SF_ERROR(Status))
        Fail(Folder ? "rmdir:" : "rm:", Args[1], Status);
}

static void Rm(const char** Args, uint64_t Count)    { Remove(Args, Count, false); }
static void Rmdir(const char** Args, uint64_t Count) { Remove(Args, Count, true); }

// What the kernel reports on Topic (and its argument).
static void Report(const char* Topic, const char* Arg = nullptr)
{
    char Line[64];
    Copy(Line, Topic, sizeof(Line));
    if (Arg)
    {
        uint64_t n = Length(Line);
        Line[n++] = ' ';
        Copy(Line + n, Arg, sizeof(Line) - n);
    }
    static char Buffer[65536];
    uint64_t Size = sizeof(Buffer);
    SfStatus Status = Admin->Report(Admin, Line, Buffer, &Size);
    if (SF_ERROR(Status) && Status != SF_BUFFER_TOO_SMALL)
        return Fail("report", Topic, Status);
    Print(Buffer);
    Print("\n");
}

static void Cpuid(const char**, uint64_t)    { Report("cpuid"); }
static void Lspci(const char**, uint64_t)    { Report("lspci"); }
static void Lsusb(const char**, uint64_t)    { Report("lsusb"); }
static void Usbports(const char**, uint64_t) { Report("usbports"); }
static void Lsblk(const char**, uint64_t)    { Report("lsblk"); }
static void Acpi(const char**, uint64_t)     { Report("acpi"); }
static void Meminfo(const char**, uint64_t)  { Report("meminfo"); }
static void Dmesg(const char**, uint64_t)    { Report("dmesg"); }

static void Usbinfo(const char** Args, uint64_t Count)
{
    if (Count < 2)
        return Print("Usage: usbinfo <index>\n");
    Report("usbinfo", Args[1]);
}

static void Mount(const char** Args, uint64_t Count)
{
    if (Count < 2)
        return Report("mount");
    SfStatus Status = Admin->Mount(Admin, Args[1]);
    if (Status == SF_ALREADY_EXISTS)
    {
        Print("All of ");
        Print(Args[1]);
        Print(" is mounted already\n");
    }
    else if (SF_ERROR(Status))
        Fail("mount", Args[1], Status);
    else
    {
        Print(Args[1]);
        Print(" is under /mount (mount lists where)\n");
    }
}

static void Umount(const char** Args, uint64_t Count)
{
    if (Count < 2)
        return Print("Usage: umount <device|/mount/folder>\n");
    // "/mount/usb1p1" is device usb1p1.
    char Abs[PATH_SIZE];
    Absolute(Args[1], Abs);
    const char* Device = Args[1];
    const char* Prefix = "/mount/";
    uint64_t i = 0;
    while (Prefix[i] && Abs[i] == Prefix[i])
        i++;
    if (!Prefix[i])
        Device = Abs + i;
    SfStatus Status = Admin->Unmount(Admin, Device);
    if (SF_ERROR(Status))
        return Fail("umount", Device, Status);
    Print(Device);
    Print(": unmounted\n");
}

static void Sync(const char**, uint64_t)
{
    SfStatus Status = Admin->Sync(Admin);
    Print(SF_ERROR(Status) ? "Sync failed\n" : "Synchronized\n");
}

static void Time(const char**, uint64_t)
{
    SfDateTime T;
    Sys->Time->GetTime(Sys->Time, &T);
    PrintNumber(T.Hour, 2, '0');   PrintChar(':');
    PrintNumber(T.Minute, 2, '0'); PrintChar(':');
    PrintNumber(T.Second, 2, '0'); Print("  ");
    PrintNumber(T.Day, 2, '0');    PrintChar('.');
    PrintNumber(T.Month, 2, '0');  PrintChar('.');
    PrintNumber(T.Year);
    Print("\n");
}

static void Uptime(const char**, uint64_t)
{
    uint64_t Ms = 0;
    Sys->Time->GetUptime(Sys->Time, &Ms);
    uint64_t Sec = Ms / 1000;
    Print("Uptime: ");
    PrintNumber(Sec / 3600);          PrintChar(':');
    PrintNumber(Sec / 60 % 60, 2, '0'); PrintChar(':');
    PrintNumber(Sec % 60, 2, '0');    PrintChar('.');
    PrintNumber(Ms % 1000, 3, '0');
    Print("\n");
}

// "12:30:05" or "26.09.2026": three numbers split by Sep.
static bool ParseTriple(const char* Text, char Sep, uint64_t* A, uint64_t* B, uint64_t* C)
{
    char Buf[16];
    Copy(Buf, Text, sizeof(Buf));
    char* Parts[3];
    int n = 0;
    Parts[n++] = Buf;
    for (char* p = Buf; *p && n < 3; p++)
        if (*p == Sep)
        {
            *p = '\0';
            Parts[n++] = p + 1;
        }
    bool Ok1, Ok2, Ok3;
    if (n < 3)
        return false;
    *A = ParseNumber(Parts[0], &Ok1);
    *B = ParseNumber(Parts[1], &Ok2);
    *C = ParseNumber(Parts[2], &Ok3);
    return Ok1 && Ok2 && Ok3;
}

static void Settime(const char** Args, uint64_t Count)
{
    if (Count < 2)
        return Print("Usage: settime HH:MM:SS [DD.MM.YYYY]\n");
    SfDateTime T;
    Sys->Time->GetTime(Sys->Time, &T);
    uint64_t h, m, s, d, mo, y;
    if (!ParseTriple(Args[1], ':', &h, &m, &s))
        return Print("Invalid time, use HH:MM:SS\n");
    T.Hour = (uint8_t)h; T.Minute = (uint8_t)m; T.Second = (uint8_t)s;
    if (Count > 2)
    {
        if (!ParseTriple(Args[2], '.', &d, &mo, &y))
            return Print("Invalid date, use DD.MM.YYYY\n");
        T.Day = (uint8_t)d; T.Month = (uint8_t)mo; T.Year = (uint16_t)y;
    }
    SfStatus Status = Admin->SetTime(Admin, &T);
    if (SF_ERROR(Status))
        return Print("Invalid time or date\n");
    Time(Args, Count);
}

static void Reboot(const char**, uint64_t)
{
    Print("Writing the disks back and restarting...\n");
    Admin->Restart(Admin);
    Print("Restart failed\n");
}

static void Shutdown(const char**, uint64_t)
{
    Print("Writing the disks back and powering off...\n");
    Admin->ShutDown(Admin);
    Print("Power-off failed. It is now safe to turn off the computer.\n");
}

// --- running programs ------------------------------------------------------

// What argument Arg names, for the program's argN: root: an existing file
// or folder, or - when it looks like a file name (a '.' or a '/') and its
// folder exists - a new empty file. nullptr when it is plain text.
static SfFile* OpenArg(const char* Arg)
{
    SfFile* File = nullptr;
    if (Open(Arg, SF_FILE_READ, &File) == SF_SUCCESS)
        return File;
    bool LooksLikeFile = false;
    for (const char* c = Arg; *c; c++)
        LooksLikeFile |= *c == '.' || *c == '/';
    if (LooksLikeFile &&
        Open(Arg, SF_FILE_READ | SF_FILE_WRITE | SF_FILE_CREATE, &File) == SF_SUCCESS)
        return File;
    return nullptr;
}

static void Run(const char** Words, uint64_t Count, bool AsAdmin)
{
    bool Background = Count > 1 && Same(Words[Count - 1], "&");
    if (Background)
        Count--;

    // A name with a slash is a path; a plain one is looked up in /apps.
    char Program[PATH_SIZE + 8];
    Copy(Program, Words[0], sizeof(Program));
    for (const char* c = Words[0]; *c; c++)
        if (*c == '/')
        {
            DiskPath(Words[0], Program);
            break;
        }

    const uint64_t MaxArgs = 32;
    SfFile* Files[MaxArgs] = {};
    uint64_t ArgCount = Count - 1 < MaxArgs ? Count - 1 : MaxArgs;
    for (uint64_t i = 0; i < ArgCount; i++)
        Files[i] = OpenArg(Words[i + 1]);

    uint64_t Flags = (Background ? SF_START_BACKGROUND : SF_START_GIVE_INPUT) |
                     (AsAdmin ? SF_START_ADMIN : 0);
    if (!Background)
        Con->Clear(Con);                // the screen is the program's now
    uint64_t Handle = 0;
    SfStatus Status = Sys->Process->Start(Sys->Process, Program, ArgCount, Words + 1, Files,
                                          Flags, Background ? nullptr : &Handle);
    for (uint64_t i = 0; i < ArgCount; i++)
        if (Files[i])
            Files[i]->Close(Files[i]);

    if (SF_ERROR(Status))
    {
        Print(Words[0]);
        Print(Status == SF_NOT_FOUND || Status == SF_INVALID_PARAMETER
              ? ": no such command or program\n" : ": cannot start it\n");
        return;
    }
    if (Background)
    {
        Print(Words[0]);
        Print(" runs in the background, its output logged in its data folder\n");
        return;
    }

    SfStatus Result = SF_SUCCESS;
    Sys->Process->Wait(Sys->Process, Handle, &Result);
    if (Result == SF_ABORTED)
    {
        Print("\n");
        Print(Words[0]);
        Print(": ended before it finished\n");
    }
    else if (Result != SF_SUCCESS)
    {
        Print("\n");
        Print(Words[0]);
        Print(": ended with status 0x");
        PrintHex(Result, 16);
        Print("\n");
    }
}

static void AdminRun(const char** Args, uint64_t Count)
{
    if (Count < 2)
        return Print("Usage: admin <program> [args] [&]\n");
    Run(Args + 1, Count - 1, true);
}

static const struct
{
    const char* Name;
    Command     Run;
    const char* Help;
} Commands[] = {
    { "help",     Help,     "this list" },
    { "cls",      Cls,      "clear the screen" },
    { "ls",       Ls,       "[path]  what is in a folder" },
    { "cd",       Cd,       "[folder]  go there (alone: where you are)" },
    { "pwd",      Pwd,      "where you are" },
    { "cat",      Cat,      "<file>  show a text file" },
    { "xxd",      Xxd,      "<file>  show a file in hex" },
    { "write",    Write,    "<file> <text...>  write text into a file" },
    { "cp",       Cp,       "<from> <to>  copy a file" },
    { "mv",       Mv,       "<from> <to>  move or rename" },
    { "rm",       Rm,       "<file>  remove a file" },
    { "mkdir",    Mkdir,    "<folder>  make a folder" },
    { "rmdir",    Rmdir,    "<folder>  remove an empty folder" },
    { "mount",    Mount,    "[device]  put a disk under /mount (alone: list)" },
    { "umount",   Umount,   "<device|/mount/x>  take it away" },
    { "sync",     Sync,     "write everything back to the disks" },
    { "lsblk",    Lsblk,    "disks and partitions" },
    { "time",     Time,     "the date and time" },
    { "settime",  Settime,  "HH:MM:SS [DD.MM.YYYY]  set the clock" },
    { "uptime",   Uptime,   "time since boot" },
    { "meminfo",  Meminfo,  "memory" },
    { "cpuid",    Cpuid,    "the processor" },
    { "lspci",    Lspci,    "PCI devices" },
    { "lsusb",    Lsusb,    "USB devices" },
    { "usbports", Usbports, "USB root ports" },
    { "usbinfo",  Usbinfo,  "<index>  one USB device" },
    { "acpi",     Acpi,     "ACPI tables" },
    { "dmesg",    Dmesg,    "the kernel's log" },
    { "admin",    AdminRun, "<program> [args]  run it with the admin right" },
    { "reboot",   Reboot,   "restart the machine" },
    { "shutdown", Shutdown, "power it off" },
};

static void Help(const char**, uint64_t)
{
    for (const auto& c : Commands)
    {
        Print("  ");
        Print(c.Name);
        for (uint64_t n = Length(c.Name); n < 10; n++)
            PrintChar(' ');
        Print(c.Help);
        Print("\n");
    }
    Print("  <program> [args] [&]   run a program from /apps, or by its path;\n"
          "                         & runs it in the background\n"
          "  Ctrl+Alt+C ends the programs on this screen, Ctrl+Alt+Z pauses them\n");
}

// Split Line in place into words; how many there are (at most Max).
static uint64_t Split(char* Line, const char** Words, uint64_t Max)
{
    uint64_t Count = 0;
    for (char* p = Line; *p && Count < Max;)
    {
        while (*p == ' ')
            *p++ = '\0';
        if (!*p)
            break;
        Words[Count++] = p;
        while (*p && *p != ' ')
            p++;
    }
    return Count;
}

extern "C" SfStatus SfMain(SfApp*, SfSystem* System)
{
    Sys   = System;
    Con   = System->Console;
    Admin = SF_HAS_FIELD(System, SfSystem, Admin) ? System->Admin : nullptr;
    if (!Admin)
    {
        Print("cmd: started without the admin right\n");
        return SF_ACCESS_DENIED;
    }
    Open("/", SF_FILE_READ, &CwdFile);
    Print("Type help for the commands\n");

    for (;;)
    {
        Print(Cwd);
        Print("> ");
        char Line[256];
        if (SF_ERROR(Con->ReadLine(Con, Line, sizeof(Line), nullptr)))
            continue;                   // Ctrl+C or Ctrl+D: a fresh line

        const char* Words[32];
        uint64_t Count = Split(Line, Words, 32);
        if (Count == 0)
            continue;
        bool Done = false;
        for (const auto& c : Commands)
            if (Same(Words[0], c.Name))
            {
                c.Run(Words, Count);
                Done = true;
                break;
            }
        if (!Done)
            Run(Words, Count, false);
    }
}
