// threadtest: how a program with several threads ends.
//
//   threadtest fault     a thread touches address 0 while the others spin
//                        and sleep: the whole program ends (SIGSEGV)
//   threadtest spin      threads spin and sleep until Ctrl+C, which ends
//                        the whole program
//   threadtest lastexit  the first thread leaves with Exit; the program
//                        lives on until the last thread ends, with its
//                        status (exit status 42)
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
        Print("threadtest spin: press Ctrl+C to end all the threads\n");
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
    else
        Print("usage: threadtest fault | spin | lastexit\n");
    return SF_INVALID_PARAMETER;
}
