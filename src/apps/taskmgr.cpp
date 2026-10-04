// taskmgr: the task manager. Run it as `sudo taskmgr`.
//
// A full-screen program that shows, once a second, the memory, the load of
// every CPU and the running programs with their CPU share, CPU time, memory
// and threads - and ends the chosen one.
//
// How it is built, as an example of sfui: the screen is made of elements
// once - frames, labels, bars and the list of programs, whose rows it draws
// itself - and a timer of one second takes a sample and sets what they show.

#include <sfos.h>
#include <sfui.h>
#include <stdio.h>

static SfSystem* Sys;
static SfAdmin*  Admin;
static SfUi*     Ui;

static const uint8_t NORMAL = SF_CELL_COLOR(SF_COLOR_WHITE, SF_COLOR_BLACK);
static const uint8_t CHOSEN = SF_CELL_COLOR(SF_COLOR_BLACK, SF_COLOR_WHITE);
static const uint8_t DIM    = SF_CELL_COLOR(SF_COLOR_BRIGHT | SF_COLOR_BLACK, SF_COLOR_BLACK);

// --- numbers into text -----------------------------------------------------

// Bytes as "123 KB", "45 MB" or "2.5 GB".
static void Size(char* Out, uint64_t Size, uint64_t Bytes)
{
    const uint64_t K = 1024, M = K * K, G = M * K;
    if (Bytes >= 10 * G || (Bytes >= G && Bytes % G == 0))
        snprintf(Out, Size, "%llu GB", Bytes / G);
    else if (Bytes >= G)
        snprintf(Out, Size, "%llu.%llu GB", Bytes / G, Bytes % G * 10 / G);
    else if (Bytes >= 10 * M)
        snprintf(Out, Size, "%llu MB", Bytes / M);
    else
        snprintf(Out, Size, "%llu KB", Bytes / K);
}

// Milliseconds as "h:mm:ss".
static void Duration(char* Out, uint64_t Size, uint64_t Ms)
{
    uint64_t S = Ms / 1000;
    snprintf(Out, Size, "%llu:%02llu:%02llu", S / 3600, S / 60 % 60, S % 60);
}

// --- what is running -------------------------------------------------------

static const uint32_t MAX_PROGRAMS = 64;

struct Program
{
    SfProcessStats Stats;
    uint32_t       Load;            // % of one CPU over the last second
};

static SfSystemInfo  Info, LastInfo;
static SfProcessInfo List[MAX_PROGRAMS];
static Program       Programs[MAX_PROGRAMS], LastPrograms[MAX_PROGRAMS];
static uint32_t      ProgramCount, LastCount;
static uint64_t      Now, LastNow;          // uptime of the samples, ms
static bool          HaveLast;

static uint32_t Percent(uint64_t Part, uint64_t Whole)
{
    if (!Whole)
        return 0;
    uint64_t P = (Part * 100 + Whole / 2) / Whole;
    return P > 100 ? 100 : (uint32_t)P;
}

static uint32_t CpuLoad(uint32_t Cpu)
{
    if (!HaveLast)
        return Percent(Info.CpuBusy[Cpu], Info.CpuTotal[Cpu]);
    return Percent(Info.CpuBusy[Cpu] - LastInfo.CpuBusy[Cpu],
                   Info.CpuTotal[Cpu] - LastInfo.CpuTotal[Cpu]);
}

static void Sample()
{
    LastInfo = Info;
    LastCount = ProgramCount;
    for (uint32_t i = 0; i < ProgramCount; i++)
        LastPrograms[i] = Programs[i];
    LastNow  = Now;
    HaveLast = ProgramCount != 0;

    Sys->Time->GetUptime(Sys->Time, &Now);
    Admin->GetSystemInfo(Admin, &Info);

    uint64_t Count = MAX_PROGRAMS;
    Admin->ListProcesses(Admin, List, &Count);
    if (Count > MAX_PROGRAMS)
        Count = MAX_PROGRAMS;
    ProgramCount = 0;
    for (uint64_t i = 0; i < Count; i++)
    {
        Program* P = &Programs[ProgramCount];
        if (SF_ERROR(Admin->GetProcessInfo(Admin, List[i].Id, &P->Stats)))
            continue;               // it ended meanwhile
        P->Load = 0;
        for (uint32_t j = 0; j < LastCount && HaveLast; j++)
            if (LastPrograms[j].Stats.Id == P->Stats.Id)
                P->Load = Percent(P->Stats.CpuTime - LastPrograms[j].Stats.CpuTime,
                                  Now - LastNow);
        ProgramCount++;
    }
}

// --- the screen ------------------------------------------------------------

static uint32_t   Columns, Rows;
static SfElement* Clock;            // uptime and time, in the header
static SfElement* MemoryBar;
static SfElement* MemoryPercent;
static SfElement* MemoryText;
static SfElement* CpuFrame;
static SfElement* CpuAll;           // " all 12% " in the processor frame's top line
static SfElement* CpuBars[SF_MAX_CPUS];
static SfElement* CpuPercents[SF_MAX_CPUS];
static SfElement* ProgramFrame;
static SfElement* ProgramList;
static SfElement* Message;          // over the footer, until the next key

// A bar's colour: green, yellow past 60%, red past 85%.
static void SetBar(SfElement* Bar, uint32_t Percent)
{
    SfSetValue(Bar, Percent);
    SfSetColors(Bar, Percent > 85 ? SF_COLOR_BRIGHT | SF_COLOR_RED
                   : Percent > 60 ? SF_COLOR_BRIGHT | SF_COLOR_YELLOW
                   : SF_COLOR_BRIGHT | SF_COLOR_GREEN, SF_COLOR_BLACK);
}

static SfElement* AddBar(sint32_t X, sint32_t Y, sint32_t Width)
{
    SfElement* Bar = SfAddBar(Ui, X, Y, Width);
    SfSetFocusColors(Bar, SF_COLOR_BRIGHT | SF_COLOR_BLACK, SF_COLOR_BLACK);
    return Bar;
}

static SfElement* AddFrame(sint32_t Y, sint32_t Height, const char* Title)
{
    SfElement* F = SfAddFrame(Ui, 0, Y, 0, Height, Title);
    SfSetColors(F, SF_COLOR_CYAN, SF_COLOR_BLACK);
    SfSetFocusColors(F, SF_COLOR_BRIGHT | SF_COLOR_WHITE, SF_COLOR_BLACK);
    return F;
}

static SfElement* AddBright(sint32_t X, sint32_t Y, sint32_t Width, const char* Text)
{
    SfElement* L = SfAddLabel(Ui, X, Y, Width, Text);
    SfSetColors(L, SF_COLOR_BRIGHT | SF_COLOR_WHITE, SF_COLOR_BLACK);
    return L;
}

// One row of the list of programs.
static void DrawProgram(SfUi* Ui, SfElement*, uint32_t Index, uint32_t X, uint32_t Y,
                        uint32_t Width, bool Cursor)
{
    const Program* P = &Programs[Index];
    const SfProcessStats* S = &P->Stats;
    uint8_t Color = Cursor ? CHOSEN : (S->Flags & SF_PROCESS_PAUSED) ? DIM : NORMAL;

    char Where[8], Load[8], Time[16], Memory[16], Line[160];
    if (S->Screen)
        snprintf(Where, sizeof(Where), "F%u", S->Screen);
    else
        snprintf(Where, sizeof(Where), "bg");
    snprintf(Load, sizeof(Load), "%u%%", P->Load);
    Duration(Time, sizeof(Time), S->CpuTime);
    Size(Memory, sizeof(Memory), S->Memory);
    snprintf(Line, sizeof(Line), " %5llu  %-20.20s%-7s%-6s%6s%11s%10s%9u%s", S->Id, S->Name,
             Where, (S->Flags & SF_PROCESS_PAUSED) ? "paused" : "runs", Load, Time, Memory,
             S->Threads, (S->Flags & SF_PROCESS_ADMIN) ? "  admin" : "");
    SfTextIn(Ui, X, Y, Line, Width, Color);
}

// What the elements show, from the last sample.
static void Refresh()
{
    char Line[96];
    SfDateTime T;
    Sys->Time->GetTime(Sys->Time, &T);
    char Up[16];
    Duration(Up, sizeof(Up), Now);
    snprintf(Line, sizeof(Line), "up %s   %02u:%02u:%02u ", Up, T.Hour, T.Minute, T.Second);
    SfSetText(Clock, Line);

    uint64_t Used = Info.MemoryTotal - Info.MemoryFree;
    uint32_t P = Percent(Used, Info.MemoryTotal);
    SetBar(MemoryBar, P);
    snprintf(Line, sizeof(Line), "%u%%", P);
    SfSetText(MemoryPercent, Line);
    char A[16], B[16], C[16];
    Size(A, sizeof(A), Used);
    Size(B, sizeof(B), Info.MemoryTotal);
    Size(C, sizeof(C), Info.MemoryFree);
    snprintf(Line, sizeof(Line), "%s of %s used, %s free", A, B, C);
    SfSetText(MemoryText, Line);

    uint64_t Busy = 0;
    for (uint32_t i = 0; i < Info.CpuCount && i < SF_MAX_CPUS; i++)
    {
        uint32_t Load = CpuLoad(i);
        Busy += Load;
        SetBar(CpuBars[i], Load);
        snprintf(Line, sizeof(Line), "%u%%", Load);
        SfSetText(CpuPercents[i], Line);
    }
    int n = snprintf(Line, sizeof(Line), " all %u%% ",
                     Info.CpuCount ? (uint32_t)(Busy / Info.CpuCount) : 0);
    SfSetText(CpuAll, Line);
    SfSetPlace(CpuAll, -(n + 2), 4, n, 1);

    uint32_t Threads = 0;
    for (uint32_t i = 0; i < ProgramCount; i++)
        Threads += Programs[i].Stats.Threads;
    snprintf(Line, sizeof(Line), "Programs: %u, threads: %u", ProgramCount, Threads);
    SfSetText(ProgramFrame, Line);
}

// A new sample; the cursor stays on the program it was on, or on the one
// now in its place when that one ended.
static void OnTick(SfUi*)
{
    uint32_t At = SfGetCursor(ProgramList);
    uint64_t Chosen = At < ProgramCount ? Programs[At].Stats.Id : 0;
    Sample();
    SfSetCount(ProgramList, ProgramCount);
    for (uint32_t i = 0; i < ProgramCount; i++)
        if (Programs[i].Stats.Id == Chosen)
            At = i;
    SfSetCursor(ProgramList, At);
    Refresh();
}

static void Say(const char* Text)
{
    SfSetText(Message, Text);
    SfSetVisible(Message, true);
}

static void EndChosen()
{
    uint32_t At = SfGetCursor(ProgramList);
    if (At >= ProgramCount)
        return;
    const SfProcessStats* S = &Programs[At].Stats;
    char Line[96];
    snprintf(Line, sizeof(Line), "[%llu] %s", S->Id, S->Name);
    if (!SfConfirm(Ui, "End the program", Line, nullptr))
        return;
    snprintf(Line, sizeof(Line), "[%llu] %s %s", S->Id, S->Name,
             SF_ERROR(Admin->EndProcess(Admin, S->Id)) ? "is gone already" : "ended");
    Say(Line);
}

// Every key the list gets first: the message goes.
static bool ListKey(SfUi*, SfElement*, SfKey)
{
    SfSetVisible(Message, false);
    return false;
}

static bool OnKey(SfUi* Ui, SfElement*, SfKey K)
{
    if (K.Char == 'q' || K.Char == 'Q' || K.Code == SF_KEY_ESCAPE || K.Code == SF_KEY_F10 ||
        ((K.Mods & SF_MOD_CTRL) && K.Char == 3))
        SfUiEnd(Ui, 1);
    else if (K.Code == SF_KEY_DELETE)
        EndChosen();
    else
        return false;
    return true;
}

// The elements, once: the processor frame is as high as the CPUs need.
static void Build()
{
    SfElement* Header = SfAddLabel(Ui, 0, 0, 0, " SurfaceOS task manager");
    SfSetColors(Header, SF_COLOR_BLACK, SF_COLOR_CYAN);
    Clock = SfAddLabel(Ui, -32, 0, 0, "");
    SfSetColors(Clock, SF_COLOR_BLACK, SF_COLOR_CYAN);
    SfSetAlign(Clock, SF_ALIGN_RIGHT);

    AddFrame(1, 3, "Memory");
    uint32_t BarWidth = Columns > 70 ? Columns - 50 : 10;
    MemoryBar = AddBar(2, 2, (sint32_t)BarWidth);
    MemoryPercent = AddBright((sint32_t)BarWidth + 3, 2, 5, "");
    SfSetAlign(MemoryPercent, SF_ALIGN_RIGHT);
    MemoryText = SfAddLabel(Ui, (sint32_t)BarWidth + 10, 2, -1, "");

    // As many columns of CPUs as fit, each "CPU 12 [bar] 100%".
    const uint32_t Cell = 34;
    uint32_t PerRow = (Columns - 2) / Cell;
    if (!PerRow)
        PerRow = 1;
    uint32_t Count = Info.CpuCount < SF_MAX_CPUS ? Info.CpuCount : SF_MAX_CPUS;
    uint32_t Lines = (Count + PerRow - 1) / PerRow;
    CpuFrame = AddFrame(4, (sint32_t)Lines + 3, "Processor");
    char Line[64];
    snprintf(Line, sizeof(Line), "%s, %u %s", Info.CpuName[0] ? Info.CpuName : "CPU",
             Info.CpuCount, Info.CpuCount == 1 ? "core" : "cores");
    SfAddLabel(Ui, 2, 5, -1, Line);
    for (uint32_t i = 0; i < Count; i++)
    {
        sint32_t X = 2 + (sint32_t)((i % PerRow) * Cell), Y = 6 + (sint32_t)(i / PerRow);
        snprintf(Line, sizeof(Line), "CPU %2u", i);
        SfAddLabel(Ui, X, Y, 6, Line);
        CpuBars[i] = AddBar(X + 7, Y, Cell - 14);
        CpuPercents[i] = AddBright(X + Cell - 7, Y, 5, "");
        SfSetAlign(CpuPercents[i], SF_ALIGN_RIGHT);
    }
    CpuAll = AddBright(-12, 4, 9, "");

    // The programs: from under the processor to the line above the footer.
    sint32_t Y = 4 + (sint32_t)Lines + 3;
    ProgramFrame = AddFrame(Y, -1, "Programs");
    AddBright(1, Y + 1, -1,
              "    ID  NAME                WHERE  STATE    CPU   CPU TIME    MEMORY  THREADS");
    ProgramList = SfAddList(Ui, 1, Y + 2, -1, -2);
    SfOnDrawItem(ProgramList, DrawProgram);
    SfOnKey(ProgramList, ListKey);

    // The footer: the keys, bright, and what they do.
    char Arrows[] = { ' ', SF_ARROW_UP, SF_ARROW_DOWN, 0 };
    AddBright(0, -1, 3, Arrows);
    SfAddLabel(Ui, 3, -1, 10, " choose");
    AddBright(13, -1, 3, "Del");
    SfAddLabel(Ui, 16, -1, 19, " end the program");
    AddBright(35, -1, 1, "q");
    SfAddLabel(Ui, 36, -1, 0, " quit");
    Message = AddBright(0, -1, 0, "");
    SfSetVisible(Message, false);
}

SfStatus SfMain(SfApp*, SfSystem* System)
{
    Sys   = System;
    Admin = System->Admin;
    SfConsole* Con = System->Console;
    if (!Admin || !SF_HAS_FIELD(Admin, SfAdmin, GetProcessInfo))
    {
        Con->Print(Con, "taskmgr needs the admin right: run it as  sudo taskmgr\n");
        return SF_ACCESS_DENIED;
    }
    Ui = SfUiOpen(Sys);
    if (!Ui)
        return SF_UNSUPPORTED;
    SfUiSize(Ui, &Columns, &Rows);

    Sample();
    Build();
    SfSetCount(ProgramList, ProgramCount);
    uint64_t Self = 0;
    Sys->Process->GetId(Sys->Process, &Self);       // start on itself
    for (uint32_t i = 0; i < ProgramCount; i++)
        if (Programs[i].Stats.Id == Self)
            SfSetCursor(ProgramList, i);
    Refresh();

    SfUiOnKey(Ui, OnKey);
    SfUiOnTimer(Ui, 1000, OnTick);
    SfUiRun(Ui);
    SfUiClose(Ui);
    return SF_SUCCESS;
}
