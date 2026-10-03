// cmd: the console (/sfos/CMD.BIN), one on every screen.
//
// A program like any other, started by the kernel with the admin right on
// each screen and started again whenever it ends. It reads a line and
// either does it itself (the commands below) or runs the program it names
// - from /apps, or by its path - handing it the keys until it ends, or is
// paused (Ctrl+Alt+Z) or sent elsewhere; an `&` before it runs it in the
// background instead. A word with spaces goes in quotes ("my file.txt").
// What a command prints longer than the screen is shown a page at a time.
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

// What a command prints is gathered (Gather) and shown once it is done:
// all at once when it fits on the screen, else a page at a time (Page).
static const uint64_t OUT_SIZE = 256 * 1024;
static char     Out[OUT_SIZE];
static uint64_t OutLength;
static uint64_t OutColumn;              // tabs become spaces up to a stop of 8
static bool     Gather;

static void Put(char C)
{
    if (OutLength + 1 >= OUT_SIZE)
        return;                         // too much: the rest is lost
    Out[OutLength++] = C;
    OutColumn = C == '\n' ? 0 : OutColumn + 1;
}

static void Print(const char* Text)
{
    if (!Gather)
    {
        Con->Print(Con, Text);
        return;
    }
    for (; *Text; Text++)
    {
        if (*Text == '\t')
            do
                Put(' ');
            while (OutColumn % 8);
        else if (*Text != '\r')
            Put(*Text);
    }
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

static uint64_t Length(const char* S);

// The gathered output as screen rows: where each starts in Out, and how
// long it is (a line longer than the screen is wide takes several).
static const uint64_t MAX_ROWS = 32768;
static uint32_t RowStart[MAX_ROWS];
static uint16_t RowLength[MAX_ROWS];

static uint64_t SplitRows(uint32_t Columns)
{
    uint64_t Rows = 0;
    for (uint64_t i = 0; i < OutLength && Rows < MAX_ROWS;)
    {
        uint64_t Start = i;
        while (i < OutLength && Out[i] != '\n' && i - Start < Columns)
            i++;
        RowStart[Rows]  = (uint32_t)Start;
        RowLength[Rows] = (uint16_t)(i - Start);
        Rows++;
        if (i < OutLength && Out[i] == '\n')
            i++;
    }
    return Rows;
}

static void DrawRow(uint32_t Row, uint32_t Columns, const char* Text, uint64_t Length)
{
    char Line[512];
    uint64_t n = 0;
    for (; n < Length && n < Columns && n + 1 < sizeof(Line); n++)
        Line[n] = Text[n];
    for (; n < Columns && n + 1 < sizeof(Line); n++)
        Line[n] = ' ';
    Line[n] = '\0';
    Con->WriteAt(Con, 0, Row, Line);
}

// Rows of output on a screen of Height rows: Height - 1 of them at a time,
// ";" in the last row ("(END)" at the end). PageUp/PageDown (and Space) move
// a page, the arrows (and Enter) a row, Home/End to either end; q, Esc or
// Ctrl+C leave it as it is, the prompt below.
static void Page(uint64_t Rows, uint32_t Columns, uint32_t Height)
{
    uint64_t View = Height - 1;
    uint64_t Top  = 0;
    uint64_t Last = Rows - View;
    Con->SetCursor(Con, 0, 0, 0);
    for (;;)
    {
        for (uint64_t r = 0; r < View; r++)
            DrawRow((uint32_t)r, Columns, Out + RowStart[Top + r], RowLength[Top + r]);
        const char* Mark = Top == Last ? "(END)" : ";";
        DrawRow((uint32_t)View, Columns, Mark, Length(Mark));

        SfKey Key;
        if (SF_ERROR(Con->ReadKey(Con, &Key)) || Key.Code == SF_KEY_ESCAPE ||
            Key.Char == 'q' || Key.Char == 'Q')
            break;
        switch (Key.Code)
        {
            case SF_KEY_PAGE_DOWN:
            case SF_KEY_SPACE:  Top = Top + View < Last ? Top + View : Last; break;
            case SF_KEY_PAGE_UP: Top = Top > View ? Top - View : 0;          break;
            case SF_KEY_DOWN:
            case SF_KEY_ENTER:  Top = Top < Last ? Top + 1 : Last;           break;
            case SF_KEY_UP:     Top = Top ? Top - 1 : 0;                     break;
            case SF_KEY_HOME:   Top = 0;                                     break;
            case SF_KEY_END:    Top = Last;                                  break;
        }
    }
    DrawRow((uint32_t)View, Columns, "", 0);
    Con->SetCursor(Con, 0, (uint32_t)View, 1);
}

// Show what the command printed, and print straight away from now on.
static void ShowOutput()
{
    Gather = false;
    Out[OutLength] = '\0';
    uint32_t Columns = 80, Height = 25;
    Con->GetSize(Con, &Columns, &Height);
    uint64_t Rows = OutLength && Height > 1 ? SplitRows(Columns) : 0;
    if (Rows < Height)
        Con->Print(Con, Out);
    else
        Page(Rows, Columns, Height);
    OutLength = OutColumn = 0;
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

static void Cd(const char** Args, uint64_t Count)
{
    if (Count < 2)
    {
        Print(Cwd);
        Print("\n");
        return;
    }
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
    uint64_t n = Length(Buffer);
    if (n && Buffer[n - 1] != '\n')
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
    ShowOutput();
    Admin->Restart(Admin);
    Print("Restart failed\n");
}

static void Shutdown(const char**, uint64_t)
{
    Print("Writing the disks back and powering off...\n");
    ShowOutput();
    Admin->ShutDown(Admin);
    Print("Power-off failed. It is now safe to turn off the computer.\n");
}

// --- running programs ------------------------------------------------------

// A program this console follows: the one on its screen (Here) - started
// here, or brought here with fg - and those it started that went elsewhere
// (Away: `&`, bg, fg on another screen), to say how they end. Handle is 0
// for one it did not start.
struct Job
{
    uint64_t Id;                // 0: none
    uint64_t Handle;
    char     Name[32];
    bool     Paused;
};
static Job      Here;
static Job      Away[16];
static uint32_t MyScreen;       // 1..9

static const uint64_t MAX_PROGRAMS = 64;
static SfProcessInfo Programs[MAX_PROGRAMS];

// The running programs into Programs; how many.
static uint64_t ListPrograms()
{
    uint64_t Count = MAX_PROGRAMS;
    Admin->ListProcesses(Admin, Programs, &Count);
    return Count < MAX_PROGRAMS ? Count : MAX_PROGRAMS;
}

static const SfProcessInfo* FindProgram(uint64_t Id)
{
    uint64_t Count = ListPrograms();
    for (uint64_t i = 0; i < Count; i++)
        if (Programs[i].Id == Id)
            return &Programs[i];
    return nullptr;
}

static void PrintJob(const Job* J)
{
    Print("[");
    PrintNumber(J->Id);
    Print("] ");
    Print(J->Name);
}

// J has ended: how, when it did not go well (or always, for one Away).
static void Ended(Job* J, bool Always)
{
    SfStatus Result = SF_SUCCESS;
    if (J->Handle)
        Sys->Process->Wait(Sys->Process, J->Handle, &Result);
    // A crash the kernel has told of on this screen already.
    if ((Result == SF_SUCCESS || Result == SF_CRASHED) && !Always)
        return;
    if (!Always && !J->Paused)
        Print("\n");                   // its output may not have ended the line
    PrintJob(J);
    if (Result == SF_SUCCESS)
        Print(": done\n");
    else if (Result == SF_ABORTED)
        Print(": ended before it finished\n");
    else if (Result == SF_CRASHED)
        Print(": crashed\n");
    else if (!Same(Why(Result), "failed"))
    {
        Print(": ended: ");
        Print(Why(Result));
        Print("\n");
    }
    else
    {
        Print(": ended with status 0x");
        PrintHex(Result, 16);
        Print("\n");
    }
    J->Id = 0;
}

static void SendAway(Job* J)
{
    for (auto& A : Away)
        if (!A.Id)
        {
            A = *J;
            break;                      // no room: how it ends goes unsaid
        }
    J->Id = 0;
}

static Job* FindAway(uint64_t Id)
{
    for (auto& A : Away)
        if (A.Id && A.Id == Id)
            return &A;
    return nullptr;
}

// Say how the programs that went elsewhere ended, those that did.
static void CheckAway()
{
    for (auto& A : Away)
        if (A.Id && !FindProgram(A.Id))
            Ended(&A, true);
}

// Wait while the program on this screen has the keys: until it ends, is
// paused (Ctrl+Alt+Z) or goes elsewhere. With the keys here already, just
// see what became of it.
static void WaitHere()
{
    if (!Here.Id)
        return;
    Con->WaitInput(Con);
    const SfProcessInfo* P = FindProgram(Here.Id);
    if (!P)
    {
        Ended(&Here, false);
        Here.Id = 0;
        Here.Paused = false;
        return;
    }
    if (P->Screen == MyScreen)
    {
        if (P->Paused && !Here.Paused)
        {
            Print("\n");
            PrintJob(&Here);
            Print(": paused. fg or Ctrl+Alt+Z goes on, bg sends it to the background,\n"
                  "Ctrl+Alt+C ends it\n");
        }
        Here.Paused = P->Paused;
        return;
    }
    PrintJob(&Here);
    if (P->Screen)
    {
        Print(": moved to F");
        PrintNumber(P->Screen);
        Print("\n");
    }
    else
        Print(": runs in the background\n");
    SendAway(&Here);
}

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

// Run the program Words[0] with the rest as its arguments: here, or in
// the background (`&` before it).
static void Run(const char** Words, uint64_t Count, bool AsAdmin, bool Background)
{
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

    if (!Background && Here.Id)
    {
        for (uint64_t i = 0; i < ArgCount; i++)
            if (Files[i])
                Files[i]->Close(Files[i]);
        return Print("A paused program is on this screen: fg, bg or kill it first\n");
    }

    uint64_t Flags = (Background ? SF_START_BACKGROUND : SF_START_GIVE_INPUT) |
                     (AsAdmin ? SF_START_ADMIN : 0);
    if (!Background)
        Con->Clear(Con);                // the screen is the program's now
    uint64_t Handle = 0;
    SfStatus Status = Sys->Process->Start(Sys->Process, Program, ArgCount, Words + 1, Files,
                                          Flags, &Handle);
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

    // Its name as the console calls it: the last part of the path.
    Job J = {};
    J.Handle = Handle;
    Sys->Process->IdOf(Sys->Process, Handle, &J.Id);
    const char* Name = Words[0];
    for (const char* c = Words[0]; *c; c++)
        if (*c == '/' || *c == ':')
            Name = c + 1;
    Copy(J.Name, Name, sizeof(J.Name));
    if (Background)
    {
        PrintJob(&J);
        Print(" runs in the background, its output logged in its data folder\n");
        SendAway(&J);
    }
    else
        Here = J;                       // the main loop waits for it
}

// The program the id in Args[1] names, or the one paused on this screen.
static uint64_t JobArg(const char* Command, const char** Args, uint64_t Count)
{
    if (Count > 1)
    {
        bool Ok;
        uint64_t Id = ParseNumber(Args[1], &Ok);
        if (Ok && Id)
            return Id;
        Print(Command);
        Print(": not a program number (ps lists them)\n");
        return 0;
    }
    if (!Here.Id)
    {
        Print(Command);
        Print(": no paused program here; ");
        Print(Command);
        Print(" <id> takes one by number (ps lists them)\n");
    }
    return Here.Id;
}

// Where the program went, for a Job: this console's, or a fresh one.
static Job TakeJob(uint64_t Id)
{
    Job J = {};
    if (Here.Id == Id)
    {
        J = Here;
        Here.Id = 0;
    }
    else if (Job* A = FindAway(Id))
    {
        J = *A;
        A->Id = 0;
    }
    else
    {
        J.Id = Id;
        const SfProcessInfo* P = FindProgram(Id);
        Copy(J.Name, P ? P->Name : "?", sizeof(J.Name));
    }
    J.Paused = false;
    return J;
}

static void JobFail(const char* Command, uint64_t Id, SfStatus Status)
{
    Print(Command);
    Print(": ");
    PrintNumber(Id);
    Print(Status == SF_NOT_FOUND     ? ": no such program\n"
        : Status == SF_IN_USE        ? ": this screen has a program already\n"
        : Status == SF_ACCESS_DENIED ? ": a console stays where it is\n"
        : Status == SF_OUT_OF_RESOURCES ? ": no hidden screen left\n"
        : ": cannot do it\n");
}

static void Fg(const char** Args, uint64_t Count)
{
    uint64_t Id = JobArg("fg", Args, Count);
    if (!Id)
        return;
    SfStatus Status = Admin->Foreground(Admin, Id);
    if (SF_ERROR(Status))
        return JobFail("fg", Id, Status);
    Here = TakeJob(Id);                 // the main loop waits for it
}

static void Bg(const char** Args, uint64_t Count)
{
    uint64_t Id = JobArg("bg", Args, Count);
    if (!Id)
        return;
    SfStatus Status = Admin->Background(Admin, Id);
    if (SF_ERROR(Status))
        return JobFail("bg", Id, Status);
    Job J = TakeJob(Id);
    PrintJob(&J);
    Print(" runs in the background, its output logged in its data folder\n");
    if (J.Handle)
        SendAway(&J);
}

static void Ps(const char**, uint64_t)
{
    uint64_t Count = ListPrograms();
    Print("   ID  WHERE  STATE   NAME\n");
    for (uint64_t i = 0; i < Count; i++)
    {
        const SfProcessInfo* P = &Programs[i];
        PrintNumber(P->Id, 5);
        Print("  ");
        if (P->Screen)
        {
            Print("F");
            PrintNumber(P->Screen);
            Print("    ");
        }
        else
            Print("bg    ");
        Print(P->Paused ? " paused  " : " runs    ");
        Print(P->Name);
        Print("\n");
    }
}

static void Kill(const char** Args, uint64_t Count)
{
    if (Count < 2)
        return Print("Usage: kill <id...>  (ps lists them)\n");
    for (uint64_t i = 1; i < Count; i++)
    {
        bool Ok;
        uint64_t Id = ParseNumber(Args[i], &Ok);
        SfStatus Status = Ok ? Admin->EndProcess(Admin, Id) : SF_NOT_FOUND;
        if (SF_ERROR(Status))
        {
            Print("kill: ");
            Print(Args[i]);
            Print(": no such program\n");
        }
    }
}

static bool Background;          // the line began with `&`

static void Sudo(const char** Args, uint64_t Count)
{
    if (Count < 2)
        return Print("Usage: [&] sudo <program> [args]\n");
    Run(Args + 1, Count - 1, true, Background);
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
    { "ps",       Ps,       "the running programs" },
    { "kill",     Kill,     "<id...>  end programs at once" },
    { "fg",       Fg,       "[id]  go on with a paused program here, or bring one here" },
    { "bg",       Bg,       "[id]  send a program (the paused one here) to the background" },
    { "sudo",     Sudo,     "<program> [args]  run it with the admin right" },
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
    Print("  [&] <program> [args]   run a program from /apps, or by its path;\n"
          "                         & runs it in the background\n"
          "  \"a b\"                  a name or path with spaces\n"
          "  Up/Down                the lines typed before\n"
          "  Ctrl+Alt+C ends the programs on this screen, Ctrl+Alt+Z pauses them\n"
          "  Longer output: PageUp/PageDown or the arrows move it, q leaves\n");
}

// Split Line in place into words; how many there are (at most Max).
// Quotes keep the spaces between them in one word and are dropped:
// "my file.txt", /mount/"usb stick"/a.
static uint64_t Split(char* Line, const char** Words, uint64_t Max)
{
    uint64_t Count = 0;
    char* p = Line;
    while (Count < Max)
    {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        Words[Count++] = p;
        char* w = p;                    // where the word goes, without its quotes
        bool Quoted = false;
        for (; *p && (Quoted || *p != ' '); p++)
        {
            if (*p == '"')
                Quoted = !Quoted;
            else
                *w++ = *p;
        }
        char Stop = *p;
        *w = '\0';
        if (Stop)
            p++;
    }
    return Count;
}

extern "C" SfStatus SfMain(SfApp*, SfSystem* System)
{
    Sys   = System;
    Con   = System->Console;
    Admin = System->Admin;
    if (!Admin)
    {
        Print("cmd: started without the admin right\n");
        return SF_ACCESS_DENIED;
    }
    Open("/", SF_FILE_READ, &CwdFile);
    uint64_t MyId = 0;
    Sys->Process->GetId(Sys->Process, &MyId);
    if (const SfProcessInfo* Me = FindProgram(MyId))
        MyScreen = Me->Screen;
    Print("Type help for the commands\n");

    for (;;)
    {
        WaitHere();
        CheckAway();
        Print(Cwd);
        Print("> ");
        char Line[256];
        if (SF_ERROR(Con->ReadLine(Con, Line, sizeof(Line), nullptr)))
        {
            // Ctrl+C or Ctrl+D: a fresh line. Or the paused program went on
            // (Ctrl+Alt+Z), taking the keys: wait for it again.
            const SfProcessInfo* P = Here.Id ? FindProgram(Here.Id) : nullptr;
            if (P && !P->Paused)
                Here.Paused = false;
            continue;
        }

        WaitHere();                     // a paused one may have been ended
        const char* Words[32];
        const char** Word = Words;
        uint64_t Count = Split(Line, Words, 32);
        // "& program" or "&program": in the background.
        Background = Count && Word[0][0] == '&';
        if (Background && !Word[0][1])
            Word++, Count--;
        else if (Background)
            Word[0]++;
        if (Count == 0)
            continue;
        bool Done = false;
        Gather = true;
        for (const auto& c : Commands)
            if (Same(Word[0], c.Name))
            {
                if (Background && c.Run != Sudo)
                    Print("& runs programs, not console commands\n");
                else
                    c.Run(Word, Count);
                Done = true;
                break;
            }
        if (!Done)
            Run(Word, Count, false, Background);
        ShowOutput();
    }
}
