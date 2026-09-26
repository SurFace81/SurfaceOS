// sdkcheck: the SDK tables a SurfaceOS program is started with.
//
// Checks what the kernel hands SfMain - signatures, revisions and sizes of
// SfSystem, SfApp, SfConsole, SfFiles, SfMemory, SfTime, SfProcess,
// SfThread and SfSync - that Console->Print works, pages and the heap, the
// clock and sleeping, the command line, threads, mutexes and events. Files
// are sfstest's.
//
// It leaves one thread asleep for good when SfMain returns: ending the
// program has to end that thread too. And it starts itself as a child that
// outlives it ("sdkcheck: the child outlived its parent").
//
// As a child of itself (Process->Start):
//   sdkcheck child <word> <two words>  returns SF_ERROR_BIT | 5 when it got
//                                      exactly those arguments, | 6 if not
//   sdkcheck late                      prints a line after 300 ms
//   sdkcheck reader                    reads a line, says what it got
//                                      ("... aborted" after Ctrl+C)
//
// Run as `sdkcheck keys`, it switches the console to SF_CONSOLE_RAW and
// reports every key ("sdkcheck key: code C mods M char N") until 'q'.
//
// Run as `sdkcheck ticks`, it prints "sdkcheck tick N" every 200 ms for
// half a minute: something to pause (Ctrl+Alt+Z) and watch stand still.
//
// Run as `sdkcheck input`, it starts `sdkcheck reader` with its input
// (SF_START_GIVE_INPUT) and then reads a line of its own: the first line
// typed goes to the child, the second - once the child has ended - back
// to it ("sdkcheck input: child got ...", "... parent got ...").
//
// Run as `sdkcheck <file> <word>`, with a file that does not exist yet
// and a word that is no file, it also checks the argN: roots. The exit status is the
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

static void CheckTime(SfTime* Time)
{
    SfDateTime Now = {};
    Check("GetTime gives a sensible date and time",
          Time->GetTime(Time, &Now) == SF_SUCCESS &&
          Now.Year >= 2024 && Now.Month >= 1 && Now.Month <= 12 &&
          Now.Day >= 1 && Now.Day <= 31 && Now.Hour <= 23 &&
          Now.Minute <= 59 && Now.Second <= 59);

    uint64_t Before = 0, After = 0;
    Check("GetUptime", Time->GetUptime(Time, &Before) == SF_SUCCESS && Before > 0);
    Check("Sleep(0) returns at once", Time->Sleep(Time, 0) == SF_SUCCESS);
    Check("Sleep(300)", Time->Sleep(Time, 300) == SF_SUCCESS);
    Time->GetUptime(Time, &After);
    Check("... takes 300 ms (up to 400)", After - Before >= 300 && After - Before <= 400);
}

static void CheckArgs(SfApp* App, SfProcess* Process, SfFiles* Files)
{
    Check("Args[0] is the program", App->ArgCount >= 1 && App->Args &&
                                    SameText(App->Args[0], "sdkcheck"));

    uint64_t Id = 0;
    Check("GetId", Process->GetId(Process, &Id) == SF_SUCCESS && Id > 0);

    // GetArgs of this process: the same strings, back to back.
    char     Buffer[256];
    uint64_t Size = sizeof(Buffer), Count = 0;
    bool Same = Process->GetArgs(Process, Id, Buffer, &Size, &Count) == SF_SUCCESS &&
                Count == App->ArgCount;
    uint64_t Off = 0;
    for (uint64_t i = 0; Same && i < Count; i++)
    {
        Same = SameText(Buffer + Off, App->Args[i]);
        while (Buffer[Off])
            Off++;
        Off++;
    }
    Check("GetArgs of this process gives App->Args", Same && Off == Size);
    uint64_t Small = 3;
    Check("GetArgs into too small a buffer is SF_BUFFER_TOO_SMALL",
          Process->GetArgs(Process, Id, Buffer, &Small, nullptr) == SF_BUFFER_TOO_SMALL &&
          Small == Size);
    Size = sizeof(Buffer);
    Check("GetArgs of no process is SF_NOT_FOUND",
          Process->GetArgs(Process, 999999, Buffer, &Size, nullptr) == SF_NOT_FOUND);

    if (App->ArgCount != 3)
    {
        Print("  (run as `sdkcheck <new file> <word>` to check arg1: and arg2:)\n");
        return;
    }

    // arg1: the file the console made for the first argument.
    SfFile* File = nullptr;
    const char Text[] = "via arg1";
    uint64_t Len = sizeof(Text) - 1;
    Check("arg1: opens the file argument names",
          Files->Open(Files, "arg1:", SF_FILE_READ | SF_FILE_WRITE, &File) == SF_SUCCESS && File);
    if (File)
    {
        Size = sizeof(Buffer);
        Check("... which the console created empty",
              File->Read(File, Buffer, &Size) == SF_SUCCESS && Size == 0);
        Size = Len;
        File->Write(File, Text, &Size);
        File->SetPosition(File, 0);
        Size = sizeof(Buffer);
        Check("... and can be written and read",
              File->Read(File, Buffer, &Size) == SF_SUCCESS && Size == Len &&
              SameBytes(Buffer, Text, Len));
        File->Close(File);
    }
    SfFile* Out = nullptr;
    Check("arg1:/x of a file is SF_NOT_FOUND",
          Files->Open(Files, "arg1:/x", SF_FILE_READ, &Out) == SF_NOT_FOUND);
    Check("a plain word gets no root: arg2: is SF_NOT_FOUND",
          Files->Open(Files, "arg2:", SF_FILE_READ, &Out) == SF_NOT_FOUND);
}

// --- Threads -------------------------------------------------------------

static SfSystem* System;

struct Worker
{
    uint64_t Index;
    uint64_t Sum;
    bool     HeapOk;
};

// Adds up 1..200000 and churns the heap meanwhile, alongside the others.
static SfStatus WorkerEntry(void* Arg)
{
    Worker*   W   = (Worker*)Arg;
    SfMemory* Mem = System->Memory;
    W->HeapOk = true;
    for (uint64_t i = 1; i <= 200000; i++)
    {
        W->Sum += i;
        if (i % 1000 == 0)
        {
            uint8_t* Block = nullptr;
            uint64_t Size  = 16 + (i / 1000) * 8 + W->Index;
            if (SF_ERROR(Mem->Allocate(Mem, Size, (void**)&Block)))
                W->HeapOk = false;
            else
            {
                for (uint64_t j = 0; j < Size; j++)
                    Block[j] = (uint8_t)W->Index;
                for (uint64_t j = 0; j < Size; j++)
                    W->HeapOk = W->HeapOk && Block[j] == (uint8_t)W->Index;
                Mem->Free(Mem, Block);
            }
        }
    }
    return W->Index;                    // the thread's SfStatus
}

static SfStatus ExitEntry(void*)
{
    System->Thread->Exit(System->Thread, SF_ERROR_BIT | 77);
    return SF_SUCCESS;                  // never reached
}

static SfStatus SleepEntry(void* Arg)
{
    System->Time->Sleep(System->Time, (uint64_t)Arg);
    return SF_SUCCESS;
}

static void CheckThreads(SfThread* Thread)
{
    Worker   Workers[4] = {};
    uint64_t Ids[4]     = {};
    bool Ok = true;
    for (uint64_t i = 0; i < 4; i++)
    {
        Workers[i].Index = i + 1;
        Ok = Ok && Thread->Create(Thread, WorkerEntry, &Workers[i], &Ids[i]) == SF_SUCCESS;
    }
    Check("Create starts 4 threads", Ok);

    bool Statuses = true, Sums = true, Heap = true;
    for (uint64_t i = 0; i < 4; i++)
    {
        SfStatus Status = SF_ABORTED;
        Statuses = Statuses && Thread->Join(Thread, Ids[i], &Status) == SF_SUCCESS &&
                   Status == i + 1;
        Sums = Sums && Workers[i].Sum == 200000ULL * 200001ULL / 2;
        Heap = Heap && Workers[i].HeapOk;
    }
    Check("Join returns what each thread's function returned", Statuses);
    Check("... each thread did its own work", Sums);
    Check("... and the heap stayed whole with all 4 using it", Heap);
    Check("an Id is used up by Join: again is SF_BAD_HANDLE",
          Thread->Join(Thread, Ids[0], nullptr) == SF_BAD_HANDLE);
    Check("an Id that is no thread is SF_BAD_HANDLE",
          Thread->Join(Thread, 60, nullptr) == SF_BAD_HANDLE);

    uint64_t Id = 0;
    SfStatus Status = SF_SUCCESS;
    Check("Exit ends a thread with its status",
          Thread->Create(Thread, ExitEntry, nullptr, &Id) == SF_SUCCESS &&
          Thread->Join(Thread, Id, &Status) == SF_SUCCESS && Status == (SF_ERROR_BIT | 77));

    // Three threads asleep for 300 ms at the same time take 300 ms, not 900.
    SfTime*  Time = System->Time;
    uint64_t Before = 0, After = 0;
    uint64_t Sleepers[3] = {};
    Time->GetUptime(Time, &Before);
    Ok = true;
    for (int i = 0; i < 3; i++)
        Ok = Ok && Thread->Create(Thread, SleepEntry, (void*)300, &Sleepers[i]) == SF_SUCCESS;
    for (int i = 0; i < 3; i++)
        Ok = Ok && Thread->Join(Thread, Sleepers[i], nullptr) == SF_SUCCESS;
    Time->GetUptime(Time, &After);
    Check("three threads sleep side by side (300..500 ms for all)",
          Ok && After - Before >= 300 && After - Before <= 500);
}

// --- Sync ----------------------------------------------------------------

static SfMutex*          CountMutex;
static volatile uint64_t SharedCount;

// Read, dawdle, write: without the mutex the threads would overwrite each
// other's increments whenever the timer switches in between.
static SfStatus CountEntry(void*)
{
    for (int i = 0; i < 20000; i++)
    {
        CountMutex->Lock(CountMutex);
        uint64_t Value = SharedCount;
        for (volatile int j = 0; j < 50; j++)
            ;
        SharedCount = Value + 1;
        CountMutex->Unlock(CountMutex);
    }
    return SF_SUCCESS;
}

static SfEvent* Ping;
static SfEvent* Pong;

static SfStatus PongEntry(void*)
{
    for (int i = 0; i < 100; i++)
    {
        if (Ping->Wait(Ping, 2000) != SF_SUCCESS)
            return SF_TIMEOUT;
        Pong->Set(Pong);
    }
    return SF_SUCCESS;
}

static void CheckSync(SfSync* Sync, SfThread* Thread, SfTime* Time)
{
    Check("CreateMutex", Sync->CreateMutex(Sync, &CountMutex) == SF_SUCCESS && CountMutex &&
                         HeaderOk(&CountMutex->Hdr, SF_MUTEX_SIGNATURE, sizeof(SfMutex)));
    if (!CountMutex)
        return;
    uint64_t Ids[4] = {};
    bool Ok = true;
    for (int i = 0; i < 4; i++)
        Ok = Ok && Thread->Create(Thread, CountEntry, nullptr, &Ids[i]) == SF_SUCCESS;
    for (int i = 0; i < 4; i++)
        Ok = Ok && Thread->Join(Thread, Ids[i], nullptr) == SF_SUCCESS;
    Check("a mutex keeps 4 threads' 80000 increments whole", Ok && SharedCount == 80000);
    Check("Close of a mutex", CountMutex->Close(CountMutex) == SF_SUCCESS);

    // A manual-reset event stays set until Reset.
    SfEvent* Event = nullptr;
    Check("CreateEvent", Sync->CreateEvent(Sync, 0, &Event) == SF_SUCCESS && Event &&
                         HeaderOk(&Event->Hdr, SF_EVENT_SIGNATURE, sizeof(SfEvent)));
    if (!Event)
        return;
    Check("Wait(0) on an event not set is SF_TIMEOUT", Event->Wait(Event, 0) == SF_TIMEOUT);
    Check("after Set every Wait goes through",
          Event->Set(Event) == SF_SUCCESS && Event->Wait(Event, 0) == SF_SUCCESS &&
          Event->Wait(Event, SF_WAIT_FOREVER) == SF_SUCCESS);
    uint64_t Before = 0, After = 0;
    Time->GetUptime(Time, &Before);
    SfStatus Status = (Event->Reset(Event), Event->Wait(Event, 200));
    Time->GetUptime(Time, &After);
    Check("after Reset, Wait(200) times out after 200 ms",
          Status == SF_TIMEOUT && After - Before >= 200 && After - Before <= 300);
    Event->Close(Event);
    Check("an unknown event flag is SF_INVALID_PARAMETER",
          Sync->CreateEvent(Sync, 0x80, &Event) == SF_INVALID_PARAMETER);

    // Auto-reset events: 100 rounds of ping-pong between two threads.
    Ok = Sync->CreateEvent(Sync, SF_EVENT_AUTO_RESET, &Ping) == SF_SUCCESS &&
         Sync->CreateEvent(Sync, SF_EVENT_AUTO_RESET, &Pong) == SF_SUCCESS;
    uint64_t Id = 0;
    Ok = Ok && Thread->Create(Thread, PongEntry, nullptr, &Id) == SF_SUCCESS;
    for (int i = 0; Ok && i < 100; i++)
        Ok = Ping->Set(Ping) == SF_SUCCESS && Pong->Wait(Pong, 2000) == SF_SUCCESS;
    SfStatus PongStatus = SF_ABORTED;
    Ok = Ok && Thread->Join(Thread, Id, &PongStatus) == SF_SUCCESS && PongStatus == SF_SUCCESS;
    Check("auto-reset events: 100 rounds of ping-pong", Ok);
    Check("... and each Set let one Wait through",
          Ping && Ping->Wait(Ping, 0) == SF_TIMEOUT && Pong && Pong->Wait(Pong, 0) == SF_TIMEOUT);
    if (Ping)
        Ping->Close(Ping);
    if (Pong)
        Pong->Close(Pong);
}

// --- Processes -------------------------------------------------------------

static void CheckStart(SfProcess* Process)
{
    const char* Args[] = { "child", "word", "two words" };
    uint64_t Handle = 0;
    SfStatus Status = SF_SUCCESS;
    Check("Start runs a program",
          Process->Start(Process, "sdkcheck", 3, Args, 0, &Handle) == SF_SUCCESS);
    Check("Wait gives what it returned (and it got its arguments)",
          Process->Wait(Process, Handle, &Status) == SF_SUCCESS && Status == (SF_ERROR_BIT | 5));
    Check("a Handle is used up by Wait: again is SF_BAD_HANDLE",
          Process->Wait(Process, Handle, &Status) == SF_BAD_HANDLE);

    uint64_t Handles[3] = {};
    bool Ok = true;
    for (int i = 0; i < 3; i++)
        Ok = Ok && Process->Start(Process, "sdkcheck", 3, Args, 0, &Handles[i]) == SF_SUCCESS;
    for (int i = 0; i < 3; i++)
        Ok = Ok && Process->Wait(Process, Handles[i], &Status) == SF_SUCCESS &&
             Status == (SF_ERROR_BIT | 5);
    Check("three programs side by side", Ok);

    Check("Start of no such program is SF_NOT_FOUND",
          Process->Start(Process, "nosuch", 0, nullptr, 0, &Handle) == SF_NOT_FOUND);
    Check("Start of a path is SF_INVALID_PARAMETER",
          Process->Start(Process, "../apps/sdkcheck", 0, nullptr, 0, &Handle) ==
          SF_INVALID_PARAMETER);
}

// Read a line and report it as "sdkcheck input: <Who> got <line>".
static SfStatus ReadAndReport(SfConsole* Console, const char* Who)
{
    char Line[64];
    SfStatus Status = Console->ReadLine(Console, Line, sizeof(Line), nullptr);
    if (Status == SF_ABORTED)
        Print("sdkcheck input: aborted\n");
    if (SF_ERROR(Status))
        return Status;
    Print("sdkcheck input: ");
    Print(Who);
    Print(" got ");
    Print(Line);
    Print("\n");
    return SF_SUCCESS;
}

// sdkcheck ticks: see the top of the file.
static SfStatus RunTicks(SfTime* Time)
{
    for (uint64_t Tick = 1; Tick <= 150; Tick++)
    {
        Print("sdkcheck tick ");
        PrintNumber(Tick);
        Print("\n");
        Time->Sleep(Time, 200);
    }
    return SF_SUCCESS;
}

// sdkcheck keys: see the top of the file.
static SfStatus RunKeys(SfConsole* Console)
{
    Console->SetMode(Console, SF_CONSOLE_RAW);
    Print("sdkcheck keys: press keys, q ends\n");
    for (;;)
    {
        SfKey Key;
        SfStatus Status = Console->ReadKey(Console, &Key);
        if (SF_ERROR(Status))
            return Status;
        Print("sdkcheck key: code ");
        PrintNumber(Key.Code);
        Print(" mods ");
        PrintNumber(Key.Mods);
        Print(" char ");
        PrintNumber((uint8_t)Key.Char);
        Print("\n");
        if (Key.Char == 'q')
            break;
    }
    Console->SetMode(Console, SF_CONSOLE_LINE);
    return SF_SUCCESS;
}

// The console calls beyond Print: the sizes, the cursor, colours, cells,
// the title and the modes (ReadKey is `sdkcheck keys`).
static void CheckConsole(SfConsole* Console)
{
    uint32_t Columns = 0, Rows = 0;
    Check("GetSize gives a screen of some size",
          Console->GetSize(Console, &Columns, &Rows) == SF_SUCCESS && Columns >= 40 && Rows >= 10);
    Check("SetColor takes colours 0..15",
          Console->SetColor(Console, SF_COLOR_BRIGHT | SF_COLOR_GREEN, SF_COLOR_BLACK) == SF_SUCCESS);
    Check("SetColor of 16 is SF_INVALID_PARAMETER",
          Console->SetColor(Console, 16, SF_COLOR_BLACK) == SF_INVALID_PARAMETER);
    Console->SetColor(Console, SF_COLOR_WHITE, SF_COLOR_BLACK);
    Check("WriteAt on the screen works",
          Console->WriteAt(Console, 0, 0, "sdkcheck was here, cut at the edge") == SF_SUCCESS);
    Check("WriteAt off the screen is SF_INVALID_PARAMETER",
          Console->WriteAt(Console, Columns, 0, "x") == SF_INVALID_PARAMETER);
    SfCell Cells[4] = { { 'o', SF_CELL_COLOR(SF_COLOR_BLACK, SF_COLOR_WHITE) },
                        { 'k', SF_CELL_COLOR(SF_COLOR_BLACK, SF_COLOR_WHITE) },
                        { '!', SF_CELL_COLOR(SF_COLOR_RED, SF_COLOR_WHITE) },
                        { '!', SF_CELL_COLOR(SF_COLOR_RED, SF_COLOR_WHITE) } };
    Check("Draw of 2x2 cells, part of them off the screen, works",
          Console->Draw(Console, Columns - 1, Rows - 1, 2, 2, Cells) == SF_SUCCESS);
    Check("Draw without cells is SF_INVALID_PARAMETER",
          Console->Draw(Console, 0, 0, 1, 1, nullptr) == SF_INVALID_PARAMETER);
    Check("SetCursor off the screen is SF_INVALID_PARAMETER",
          Console->SetCursor(Console, 0, Rows, 1) == SF_INVALID_PARAMETER);
    Check("SetTitle works", Console->SetTitle(Console, "checking") == SF_SUCCESS);
    Check("SetMode of 7 is SF_INVALID_PARAMETER",
          Console->SetMode(Console, 7) == SF_INVALID_PARAMETER);
    char Line[8];
    Check("in SF_CONSOLE_RAW, ReadLine is SF_UNSUPPORTED",
          Console->SetMode(Console, SF_CONSOLE_RAW) == SF_SUCCESS &&
          Console->ReadLine(Console, Line, sizeof(Line), nullptr) == SF_UNSUPPORTED &&
          Console->SetMode(Console, SF_CONSOLE_LINE) == SF_SUCCESS);
}

// sdkcheck input: see the top of the file.
static SfStatus RunInput(SfSystem* Sys)
{
    const char* Args[] = { "reader" };
    uint64_t Handle = 0;
    SfStatus Status = Sys->Process->Start(Sys->Process, "sdkcheck", 1, Args,
                                          SF_START_GIVE_INPUT, &Handle);
    if (SF_ERROR(Status))
        return Status;
    Print("sdkcheck input: the reader has the keys\n");
    SfStatus ChildStatus = SF_ABORTED;
    Sys->Process->Wait(Sys->Process, Handle, &ChildStatus);
    if (SF_ERROR(ChildStatus))
        return ChildStatus;
    return ReadAndReport(Sys->Console, "parent");
}

// sdkcheck child|late|reader: see the top of the file.
static SfStatus RunAsChild(SfApp* App, SfSystem* Sys)
{
    if (SameText(App->Args[1], "reader"))
        return ReadAndReport(Sys->Console, "child");
    if (SameText(App->Args[1], "late"))
    {
        Sys->Time->Sleep(Sys->Time, 300);
        Sys->Console->Print(Sys->Console, "sdkcheck: the child outlived its parent\n");
        return SF_SUCCESS;
    }
    bool Same = App->ArgCount == 4 && SameText(App->Args[0], "sdkcheck") &&
                SameText(App->Args[2], "word") && SameText(App->Args[3], "two words");
    return SF_ERROR_BIT | (Same ? 5 : 6);
}

static void CheckMemory(SfMemory* Memory)
{
    // Pages: zeroed, writable, given back.
    uint8_t* Pages = nullptr;
    Check("AllocatePages gives 3 pages",
          Memory->AllocatePages(Memory, 3, (void**)&Pages) == SF_SUCCESS && Pages &&
          ((uint64_t)Pages & (SF_PAGE_SIZE - 1)) == 0);
    if (Pages)
    {
        bool Zero = true;
        for (uint64_t i = 0; i < 3 * SF_PAGE_SIZE; i++)
            Zero = Zero && Pages[i] == 0;
        Pages[0] = 1;
        Pages[3 * SF_PAGE_SIZE - 1] = 2;
        Check("... zeroed and writable", Zero && Pages[3 * SF_PAGE_SIZE - 1] == 2);
        Check("FreePages gives them back", Memory->FreePages(Memory, Pages, 3) == SF_SUCCESS);
    }
    void* Out = nullptr;
    Check("AllocatePages of 0 pages is SF_INVALID_PARAMETER",
          Memory->AllocatePages(Memory, 0, &Out) == SF_INVALID_PARAMETER);

    // The heap: blocks of many sizes, all apart, aligned, zeroed.
    const int Count = 40;
    uint8_t* Blocks[Count];
    bool Ok = true;
    for (int i = 0; i < Count; i++)
    {
        uint64_t Size = 1 + (uint64_t)i * 97;
        Blocks[i] = nullptr;
        Ok = Ok && Memory->Allocate(Memory, Size, (void**)&Blocks[i]) == SF_SUCCESS &&
             Blocks[i] && ((uint64_t)Blocks[i] & 15) == 0;
        for (uint64_t j = 0; Ok && j < Size; j++)
        {
            Ok = Blocks[i][j] == 0;
            Blocks[i][j] = (uint8_t)i;
        }
    }
    Check("Allocate: 40 blocks, aligned and zeroed", Ok);
    for (int i = 0; Ok && i < Count; i++)
        for (uint64_t j = 0; j < 1 + (uint64_t)i * 97; j++)
            Ok = Ok && Blocks[i][j] == (uint8_t)i;
    Check("... none overlaps another", Ok);

    Ok = true;
    for (int i = 0; i < Count; i += 2)
        Ok = Ok && Memory->Free(Memory, Blocks[i]) == SF_SUCCESS;
    for (int i = 1; i < Count; i += 2)
        Ok = Ok && Memory->Free(Memory, Blocks[i]) == SF_SUCCESS;
    Check("Free gives all of them back", Ok);
    Check("a second Free of a block is SF_INVALID_PARAMETER",
          Memory->Free(Memory, Blocks[0]) == SF_INVALID_PARAMETER);

    // After everything merged back, a block larger than the heap has grown
    // by so far still fits.
    uint8_t* Big = nullptr;
    Check("a 200 KiB block", Memory->Allocate(Memory, 200 * 1024, (void**)&Big) == SF_SUCCESS &&
                             Big && Big[200 * 1024 - 1] == 0);
    if (Big)
        Memory->Free(Memory, Big);
}

extern "C" SfStatus SfMain(SfApp* App, SfSystem* Sys)
{
    if (!Sys || !Sys->Console)
        return SF_INVALID_PARAMETER;        // nothing to report through
    Con = Sys->Console;
    if (App && App->ArgCount >= 2 &&
        (SameText(App->Args[1], "child") || SameText(App->Args[1], "late") ||
         SameText(App->Args[1], "reader")))
        return RunAsChild(App, Sys);
    if (App && App->ArgCount >= 2 && SameText(App->Args[1], "input"))
        return RunInput(Sys);
    if (App && App->ArgCount >= 2 && SameText(App->Args[1], "keys"))
        return RunKeys(Sys->Console);
    if (App && App->ArgCount >= 2 && SameText(App->Args[1], "ticks"))
        return RunTicks(Sys->Time);

    Print("sdkcheck - the tables SfMain gets\n");

    Check("SfSystem: signature, revision 1.x, size",
          HeaderOk(&Sys->Hdr, SF_SYSTEM_SIGNATURE, sizeof(SfSystem)));
    Check("SfSystem has the Console field", SF_HAS_FIELD(Sys, SfSystem, Console));
    Check("SfConsole: signature, revision 1.x, size",
          HeaderOk(&Con->Hdr, SF_CONSOLE_SIGNATURE, sizeof(SfConsole)));
    if (SF_HAS_FIELD(Con, SfConsole, SetTitle))
        CheckConsole(Con);
    Check("SfApp: signature, revision 1.x, size",
          App && HeaderOk(&App->Hdr, SF_APP_SIGNATURE, sizeof(SfApp)));
    Check("App->Name is the program's name", App && SameText(App->Name, "sdkcheck"));

    Check("Print returns SF_SUCCESS", Con->Print(Con, "") == SF_SUCCESS);

    // Loading GS clears its base; the kernel keeps its own GS base (swapgs
    // on every entry) and must not notice.
    asm volatile("mov %0, %%gs" :: "r"((uint16_t)0x2B));
    Check("a program loading GS does not disturb the system",
          Con->Print(Con, "") == SF_SUCCESS);

    // Longer than the kernel's copy chunk (1 KiB): printed in pieces.
    static char Long[2601];
    for (int i = 0; i < 2600; i++)
        Long[i] = (i % 100 == 99) ? '\n' : (char)('a' + i % 26);
    Long[2600] = '\0';
    Check("a 2600-byte Print works", Con->Print(Con, Long) == SF_SUCCESS);

    Check("Print of an unmapped address is SF_INVALID_PARAMETER",
          Con->Print(Con, (const char*)0x1000) == SF_INVALID_PARAMETER);

    Check("SfMemory: signature, revision 1.x, size",
          SF_HAS_FIELD(Sys, SfSystem, Memory) && Sys->Memory &&
          HeaderOk(&Sys->Memory->Hdr, SF_MEMORY_SIGNATURE, sizeof(SfMemory)));
    if (SF_HAS_FIELD(Sys, SfSystem, Memory) && Sys->Memory)
        CheckMemory(Sys->Memory);

    Check("SfProcess: signature, revision 1.x, size",
          SF_HAS_FIELD(Sys, SfSystem, Process) && Sys->Process &&
          HeaderOk(&Sys->Process->Hdr, SF_PROCESS_SIGNATURE, sizeof(SfProcess)));
    if (App && SF_HAS_FIELD(Sys, SfSystem, Process) && Sys->Process && Sys->Files)
        CheckArgs(App, Sys->Process, Sys->Files);
    if (SF_HAS_FIELD(Sys, SfSystem, Process) && Sys->Process)
        CheckStart(Sys->Process);

    Check("SfTime: signature, revision 1.x, size",
          SF_HAS_FIELD(Sys, SfSystem, Time) && Sys->Time &&
          HeaderOk(&Sys->Time->Hdr, SF_TIME_SIGNATURE, sizeof(SfTime)));
    if (SF_HAS_FIELD(Sys, SfSystem, Time) && Sys->Time)
        CheckTime(Sys->Time);

    Check("SfFiles: signature, revision 1.x, size",
          SF_HAS_FIELD(Sys, SfSystem, Files) && Sys->Files &&
          HeaderOk(&Sys->Files->Hdr, SF_FILES_SIGNATURE, sizeof(SfFiles)));

    Check("SfThread: signature, revision 1.x, size",
          SF_HAS_FIELD(Sys, SfSystem, Thread) && Sys->Thread &&
          HeaderOk(&Sys->Thread->Hdr, SF_THREAD_SIGNATURE, sizeof(SfThread)));
    System = Sys;
    Check("SfSync: signature, revision 1.x, size",
          SF_HAS_FIELD(Sys, SfSystem, Sync) && Sys->Sync &&
          HeaderOk(&Sys->Sync->Hdr, SF_SYNC_SIGNATURE, sizeof(SfSync)));
    if (SF_HAS_FIELD(Sys, SfSystem, Thread) && Sys->Thread)
    {
        CheckThreads(Sys->Thread);
        if (SF_HAS_FIELD(Sys, SfSystem, Sync) && Sys->Sync)
            CheckSync(Sys->Sync, Sys->Thread, Sys->Time);

        // Left asleep for good: returning from SfMain ends it.
        uint64_t Id = 0;
        Sys->Thread->Create(Sys->Thread, SleepEntry, (void*)1000000000ULL, &Id);
    }
    if (SF_HAS_FIELD(Sys, SfSystem, Process) && Sys->Process)
    {
        // A child that outlives this program (not waited for).
        const char* Late[] = { "late" };
        uint64_t Handle = 0;
        Sys->Process->Start(Sys->Process, "sdkcheck", 1, Late, 0, &Handle);
    }

    Print("sdkcheck: ");
    PrintNumber(Passed);
    Print(" passed, ");
    PrintNumber(Failed);
    Print(" failed\n");

    return Failed ? (SF_ERROR_BIT | Failed) : SF_SUCCESS;
}
