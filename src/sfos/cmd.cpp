// cmd: the console of screens 2..9 (/sfos/CMD.BIN).
//
// A program like any other, started by the kernel with the admin right on
// each of those screens and started again whenever it ends. It reads a
// line and runs the program it names, from /apps, handing it the keys
// until it ends; a last `&` runs it in the background instead. More
// commands come over from the kernel's console on screen 1 step by step.

#include <sfos.h>

static SfSystem*  Sys;
static SfConsole* Con;

static void Print(const char* Text)
{
    Con->Print(Con, Text);
}

static void PrintHex(uint64_t Value)
{
    char Buffer[19] = "0x";
    int  Digits = 1;
    for (uint64_t v = Value >> 4; v; v >>= 4)
        Digits++;
    for (int i = Digits - 1; i >= 0; i--, Value >>= 4)
        Buffer[2 + i] = "0123456789ABCDEF"[Value & 0xF];
    Buffer[2 + Digits] = '\0';
    Print(Buffer);
}

static bool Same(const char* A, const char* B)
{
    while (*A && *A == *B)
        A++, B++;
    return *A == *B;
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

static void Help()
{
    Print("  <program> [args]     run a program from /apps; it gets the keys\n"
          "  <program> [args] &   run it in the background, its output logged\n"
          "                       in its data folder\n"
          "  help                 this list\n"
          "  Ctrl+Alt+C ends the programs on this screen, Ctrl+Alt+Z pauses them\n");
}

static void Run(const char** Words, uint64_t Count)
{
    bool Background = Count > 1 && Same(Words[Count - 1], "&");
    if (Background)
        Count--;

    uint64_t Handle = 0;
    SfStatus Status = Sys->Process->Start(Sys->Process, Words[0], Count - 1, Words + 1,
                                          Background ? SF_START_BACKGROUND : SF_START_GIVE_INPUT,
                                          Background ? nullptr : &Handle);
    if (Status == SF_NOT_FOUND || Status == SF_INVALID_PARAMETER)
    {
        Print(Words[0]);
        Print(": no such program\n");
        return;
    }
    if (SF_ERROR(Status))
    {
        Print(Words[0]);
        Print(": cannot start it\n");
        return;
    }
    if (Background)
    {
        Print(Words[0]);
        Print(" runs in the background\n");
        return;
    }

    SfStatus Result = SF_SUCCESS;
    Sys->Process->Wait(Sys->Process, Handle, &Result);
    if (Result == SF_ABORTED)
    {
        Print(Words[0]);
        Print(": ended before it finished\n");
    }
    else if (Result != SF_SUCCESS)
    {
        Print(Words[0]);
        Print(": ended with status ");
        PrintHex(Result);
        Print("\n");
    }
}

extern "C" SfStatus SfMain(SfApp*, SfSystem* System)
{
    Sys = System;
    Con = System->Console;
    Print("SurfaceOS console - type a program's name, or help\n");

    for (;;)
    {
        Print("> ");
        char Line[256];
        SfStatus Status = Con->ReadLine(Con, Line, sizeof(Line), nullptr);
        if (SF_ERROR(Status))
            continue;                   // Ctrl+C or Ctrl+D: a fresh line

        const char* Words[16];
        uint64_t Count = Split(Line, Words, 16);
        if (Count == 0)
            continue;
        if (Same(Words[0], "help"))
            Help();
        else
            Run(Words, Count);
    }
}
