// threadtest: how a program with several threads ends.
//
//   threadtest fault     a thread touches address 0 while the others spin
//                        and sleep: the whole program ends (SIGSEGV)
//   threadtest spin      threads spin and sleep until the kill key
//                        (Ctrl+Alt+Backspace) ends the whole program
//   threadtest lastexit  the first thread leaves with Exit; the program
//                        lives on until the last thread ends, with its
//                        status (exit status 42)
//   threadtest group     starts two `threadtest spin` of its own and
//                        sleeps: the kill key ends all three programs
//   threadtest stress    3 rounds of 8 threads at once, each mixing a
//                        mutex-guarded counter, heap blocks, pages taken and
//                        given back (TLB shootdowns while the others run)
//                        and short-lived threads of its own, beside two
//                        child programs; everything is checked at the end
//                        ("threadtest stress: N passed, M failed")
//
// Each mode prints what it expects; the exit status tells what happened.

#include <sfos.h>

static SfSystem* System;

static void Print(const char* Text)
{
    System->Console->Print(System->Console, Text);
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

static SfStatus Spin(void*)
{
    for (volatile uint64_t i = 0;; i++)
        ;
    return SF_SUCCESS;
}

static SfStatus SleepLong(void*)
{
    System->Time->Sleep(System->Time, 1000000000ULL);
    return SF_SUCCESS;
}

static SfStatus Fault(void*)
{
    System->Time->Sleep(System->Time, 300);
    Print("threadtest: a thread touches address 0 now\n");
    *(volatile uint64_t*)0 = 1;
    return SF_SUCCESS;
}

static SfStatus Last(void*)
{
    System->Time->Sleep(System->Time, 500);
    Print("threadtest: the last thread ends the program with status 42\n");
    return SF_ERROR_BIT | 42;
}

// --- stress ------------------------------------------------------------------

static const int      StressThreads = 8;
static const uint64_t StressRounds  = 400;

static SfMutex*          Guard;
static volatile uint64_t Counter;
static uint64_t          Passed, Failed;

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

struct StressWorker
{
    uint64_t Index;
    uint64_t Seed;
    bool     Ok;
};

static uint64_t Next(uint64_t* Seed)
{
    *Seed = *Seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return *Seed >> 33;
}

static SfStatus ShortLived(void* Arg)
{
    return (uint64_t)Arg * 3;
}

static SfStatus StressEntry(void* Arg)
{
    StressWorker* W   = (StressWorker*)Arg;
    SfMemory*     Mem = System->Memory;
    SfThread*     Thr = System->Thread;
    W->Ok = true;

    for (uint64_t Round = 0; Round < StressRounds; Round++)
    {
        Guard->Lock(Guard);
        uint64_t Value = Counter;
        Counter = Value + 1;
        Guard->Unlock(Guard);

        // A heap block of some size, filled and checked.
        uint64_t Size = 1 + Next(&W->Seed) % 3000;
        uint8_t* Block = nullptr;
        if (SF_ERROR(Mem->Allocate(Mem, Size, (void**)&Block)))
            W->Ok = false;
        else
        {
            uint8_t Mark = (uint8_t)(W->Index * 31 + Round);
            for (uint64_t i = 0; i < Size; i++)
                Block[i] = Mark;
            for (uint64_t i = 0; i < Size; i++)
                W->Ok = W->Ok && Block[i] == Mark;
            Mem->Free(Mem, Block);
        }

        // Pages taken and given back: the other threads of this program
        // run meanwhile, so their CPUs' TLBs are flushed.
        if (Round % 20 == 0)
        {
            uint64_t* Pages = nullptr;
            if (SF_ERROR(Mem->AllocatePages(Mem, 2, (void**)&Pages)))
                W->Ok = false;
            else
            {
                for (uint64_t i = 0; i < 2 * 512; i++)
                    Pages[i] = W->Index + i;
                for (uint64_t i = 0; i < 2 * 512; i++)
                    W->Ok = W->Ok && Pages[i] == W->Index + i;
                Mem->FreePages(Mem, Pages, 2);
            }
        }

        // A thread of its own, joined at once.
        if (Round % 50 == 0)
        {
            uint64_t Id = 0;
            SfStatus Status = SF_ABORTED;
            W->Ok = W->Ok && Thr->Create(Thr, ShortLived, (void*)(Round + 1), &Id) == SF_SUCCESS &&
                    Thr->Join(Thr, Id, &Status) == SF_SUCCESS && Status == (Round + 1) * 3;
        }
    }
    return W->Ok ? SF_SUCCESS : SF_ABORTED;
}

static SfStatus Stress(SfSystem* Sys)
{
    Print("threadtest stress - 3 rounds of 8 threads and 2 child programs\n");
    if (SF_ERROR(Sys->Sync->CreateMutex(Sys->Sync, &Guard)))
    {
        Print("threadtest stress: no mutex\n");
        return SF_OUT_OF_RESOURCES;
    }

    for (int Pass = 1; Pass <= 3; Pass++)
    {
        Counter = 0;
        const char* ChildArgs[] = { "child" };
        uint64_t Children[2] = {};
        bool Started = true;
        for (int i = 0; i < 2; i++)
            Started = Started && Sys->Process->Start(Sys->Process, "threadtest", 1, ChildArgs, 0,
                                                     &Children[i]) == SF_SUCCESS;

        StressWorker Workers[StressThreads];
        uint64_t Ids[StressThreads] = {};
        bool Created = true;
        for (int i = 0; i < StressThreads; i++)
        {
            Workers[i].Index = (uint64_t)i + 1;
            Workers[i].Seed  = (uint64_t)Pass * 1000 + (uint64_t)i;
            Workers[i].Ok    = false;
            Created = Created && Sys->Thread->Create(Sys->Thread, StressEntry, &Workers[i],
                                                     &Ids[i]) == SF_SUCCESS;
        }

        bool Joined = true, Each = true;
        for (int i = 0; i < StressThreads; i++)
        {
            SfStatus Status = SF_ABORTED;
            Joined = Joined && Ids[i] && Sys->Thread->Join(Sys->Thread, Ids[i], &Status) == SF_SUCCESS;
            Each = Each && Status == SF_SUCCESS && Workers[i].Ok;
        }
        bool ChildrenOk = Started;
        for (int i = 0; ChildrenOk && i < 2; i++)
        {
            SfStatus Status = SF_ABORTED;
            ChildrenOk = Sys->Process->Wait(Sys->Process, Children[i], &Status) == SF_SUCCESS &&
                         Status == (SF_ERROR_BIT | 7);
        }

        Print("pass ");
        PrintNumber((uint64_t)Pass);
        Print(":\n");
        Check("8 threads started and joined", Created && Joined);
        Check("each thread's heap blocks, pages and own threads checked out", Each);
        Check("the mutex kept every increment", Counter == StressThreads * StressRounds);
        Check("2 child programs ran beside them", ChildrenOk);
    }
    Guard->Close(Guard);

    Print("threadtest stress: ");
    PrintNumber(Passed);
    Print(" passed, ");
    PrintNumber(Failed);
    Print(" failed\n");
    return Failed ? (SF_ERROR_BIT | Failed) : SF_SUCCESS;
}

// threadtest child: a child program of stress; its heap and a thread.
static SfStatus Child(SfSystem* Sys)
{
    uint8_t* Block = nullptr;
    uint64_t Id = 0;
    SfStatus Status = SF_ABORTED;
    bool Ok = !SF_ERROR(Sys->Memory->Allocate(Sys->Memory, 5000, (void**)&Block)) &&
              Sys->Thread->Create(Sys->Thread, ShortLived, (void*)5, &Id) == SF_SUCCESS &&
              Sys->Thread->Join(Sys->Thread, Id, &Status) == SF_SUCCESS && Status == 15;
    if (Block)
        Sys->Memory->Free(Sys->Memory, Block);
    return Ok ? (SF_ERROR_BIT | 7) : (SF_ERROR_BIT | 8);
}

static void Start(SfThreadEntry Entry)
{
    uint64_t Id = 0;
    if (SF_ERROR(System->Thread->Create(System->Thread, Entry, nullptr, &Id)))
        Print("threadtest: Create failed\n");
}

extern "C" SfStatus SfMain(SfApp* App, SfSystem* Sys)
{
    System = Sys;
    const char* Mode = App->ArgCount >= 2 ? App->Args[1] : "";

    if (SameText(Mode, "fault"))
    {
        Print("threadtest fault: expect the program to end with CPU exception 14\n");
        Start(Spin);
        Start(SleepLong);
        Start(Fault);
        SleepLong(nullptr);
    }
    else if (SameText(Mode, "spin"))
    {
        Print("threadtest spin: press Ctrl+Alt+Backspace to end all the threads\n");
        Start(Spin);
        Start(Spin);
        Start(SleepLong);
        SleepLong(nullptr);
    }
    else if (SameText(Mode, "lastexit"))
    {
        Print("threadtest lastexit: the first thread leaves now\n");
        Start(Last);
        Sys->Thread->Exit(Sys->Thread, SF_SUCCESS);
    }
    else if (SameText(Mode, "group"))
    {
        Print("threadtest group: two more threadtest programs; Ctrl+Alt+Backspace ends all three\n");
        const char* Spin[] = { "spin" };
        for (int i = 0; i < 2; i++)
        {
            uint64_t Handle = 0;
            if (SF_ERROR(Sys->Process->Start(Sys->Process, "threadtest", 1, Spin, 0, &Handle)))
                Print("threadtest: Start failed\n");
        }
        SleepLong(nullptr);
    }
    else if (SameText(Mode, "stress"))
        return Stress(Sys);
    else if (SameText(Mode, "child"))
        return Child(Sys);
    else
        Print("usage: threadtest fault | spin | lastexit | group | stress\n");
    return SF_INVALID_PARAMETER;
}
