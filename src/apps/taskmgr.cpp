// taskmgr: the task manager. Run it as `sudo taskmgr`.
//
// A full-screen program (SF_CONSOLE_RAW) that shows, once a second, the
// memory, the load of every CPU and the running programs with their CPU
// share, CPU time, memory and threads - and ends the chosen one.
//
// How it is built, as an example of the console protocol: the whole screen
// is put together in Cells and drawn with one Draw call. It waits for a
// key with WaitAny, with a timeout - its tick - and ReadKey takes the key
// once one is there.

#include <sfos.h>

static SfSystem*  Sys;
static SfConsole* Con;
static SfAdmin*   Admin;

// --- the screen ------------------------------------------------------------

static const uint32_t MAX_COLUMNS = 256;
static const uint32_t MAX_ROWS    = 128;
static SfCell   Cells[MAX_COLUMNS * MAX_ROWS];
static uint32_t Columns, Rows;

static const uint8_t NORMAL   = SF_CELL_COLOR(SF_COLOR_WHITE, SF_COLOR_BLACK);
static const uint8_t BRIGHT   = SF_CELL_COLOR(SF_COLOR_BRIGHT | SF_COLOR_WHITE, SF_COLOR_BLACK);
static const uint8_t FRAME    = SF_CELL_COLOR(SF_COLOR_CYAN, SF_COLOR_BLACK);
static const uint8_t HEADER   = SF_CELL_COLOR(SF_COLOR_BLACK, SF_COLOR_CYAN);
static const uint8_t CHOSEN   = SF_CELL_COLOR(SF_COLOR_BLACK, SF_COLOR_WHITE);
static const uint8_t DIM      = SF_CELL_COLOR(SF_COLOR_BRIGHT | SF_COLOR_BLACK, SF_COLOR_BLACK);
static const uint8_t WARNING  = SF_CELL_COLOR(SF_COLOR_BRIGHT | SF_COLOR_YELLOW, SF_COLOR_BLACK);

static void Put(uint32_t X, uint32_t Y, char C, uint8_t Color)
{
    if (X < Columns && Y < Rows)
    {
        Cells[Y * Columns + X].Char  = C;
        Cells[Y * Columns + X].Color = Color;
    }
}

// Text at (X, Y); where it ends.
static uint32_t Text(uint32_t X, uint32_t Y, const char* S, uint8_t Color)
{
    for (; *S; S++, X++)
        Put(X, Y, *S, Color);
    return X;
}

static void Fill(uint32_t X, uint32_t Y, uint32_t Width, char C, uint8_t Color)
{
    for (uint32_t i = 0; i < Width; i++)
        Put(X + i, Y, C, Color);
}

// A frame of Width x Height at (X, Y) with Title in its top line.
static void Frame(uint32_t X, uint32_t Y, uint32_t Width, uint32_t Height, const char* Title)
{
    Put(X, Y, SF_BOX_TOP_LEFT, FRAME);
    Fill(X + 1, Y, Width - 2, SF_BOX_H, FRAME);
    Put(X + Width - 1, Y, SF_BOX_TOP_RIGHT, FRAME);
    for (uint32_t y = Y + 1; y < Y + Height - 1; y++)
    {
        Put(X, y, SF_BOX_V, FRAME);
        Fill(X + 1, y, Width - 2, ' ', NORMAL);
        Put(X + Width - 1, y, SF_BOX_V, FRAME);
    }
    Put(X, Y + Height - 1, SF_BOX_BOTTOM_LEFT, FRAME);
    Fill(X + 1, Y + Height - 1, Width - 2, SF_BOX_H, FRAME);
    Put(X + Width - 1, Y + Height - 1, SF_BOX_BOTTOM_RIGHT, FRAME);

    Put(X + 2, Y, ' ', BRIGHT);
    uint32_t End = Text(X + 3, Y, Title, BRIGHT);
    Put(End, Y, ' ', BRIGHT);
}

// A bar Width cells long, Percent of it full: green, yellow past 60%, red
// past 85%.
static void Bar(uint32_t X, uint32_t Y, uint32_t Width, uint32_t Percent)
{
    uint8_t Ink = Percent > 85 ? SF_COLOR_BRIGHT | SF_COLOR_RED
                : Percent > 60 ? SF_COLOR_BRIGHT | SF_COLOR_YELLOW
                : SF_COLOR_BRIGHT | SF_COLOR_GREEN;
    uint32_t Full = (Percent * Width + 50) / 100;
    for (uint32_t i = 0; i < Width; i++)
        Put(X + i, Y, i < Full ? SF_BLOCK_FULL : SF_SHADE_LIGHT,
            SF_CELL_COLOR(i < Full ? Ink : SF_COLOR_BRIGHT | SF_COLOR_BLACK, SF_COLOR_BLACK));
}

// --- numbers into text -----------------------------------------------------

// Value in decimal, right-aligned in Width (0: as long as it is) into Out.
static char* Number(char* Out, uint64_t Value, uint32_t Width = 0)
{
    char Digits[24];
    uint32_t n = 0;
    do
    {
        Digits[n++] = (char)('0' + Value % 10);
        Value /= 10;
    } while (Value);
    for (uint32_t i = n; i < Width; i++)
        *Out++ = ' ';
    while (n)
        *Out++ = Digits[--n];
    *Out = '\0';
    return Out;
}

static char* Append(char* Out, const char* S)
{
    while (*S)
        *Out++ = *S++;
    *Out = '\0';
    return Out;
}

static char* TwoDigits(char* Out, uint64_t Value)
{
    *Out++ = (char)('0' + Value / 10 % 10);
    *Out++ = (char)('0' + Value % 10);
    *Out = '\0';
    return Out;
}

// Bytes as "123 KB", "45 MB" or "2.5 GB".
static char* Size(char* Out, uint64_t Bytes)
{
    const uint64_t K = 1024, M = K * K, G = M * K;
    if (Bytes >= 10 * G || (Bytes >= G && Bytes % G == 0))
        return Append(Number(Out, Bytes / G), " GB");
    if (Bytes >= G)
    {
        Out = Number(Out, Bytes / G);
        *Out++ = '.';
        *Out++ = (char)('0' + Bytes % G * 10 / G);
        return Append(Out, " GB");
    }
    if (Bytes >= 10 * M)
        return Append(Number(Out, Bytes / M), " MB");
    return Append(Number(Out, Bytes / K), " KB");
}

// Milliseconds as "h:mm:ss".
static char* Duration(char* Out, uint64_t Ms)
{
    uint64_t S = Ms / 1000;
    Out = Number(Out, S / 3600);
    *Out++ = ':';
    Out = TwoDigits(Out, S / 60 % 60);
    *Out++ = ':';
    return TwoDigits(Out, S % 60);
}

// Right-align Text in Width into Out.
static char* Right(char* Out, const char* S, uint32_t Width)
{
    uint32_t n = 0;
    while (S[n])
        n++;
    for (uint32_t i = n; i < Width; i++)
        *Out++ = ' ';
    return Append(Out, S);
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

static uint64_t Chosen;             // the Id of the chosen program

static void Sample()
{
    // The chosen program gone, the one now in its place is chosen.
    uint32_t Was = 0;
    for (uint32_t i = 0; i < ProgramCount; i++)
        if (Programs[i].Stats.Id == Chosen)
            Was = i;

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

    bool Found = false;
    for (uint32_t i = 0; i < ProgramCount; i++)
        Found |= Programs[i].Stats.Id == Chosen;
    if (!Found && ProgramCount)
        Chosen = Programs[Was < ProgramCount ? Was : ProgramCount - 1].Stats.Id;
}

// --- drawing ---------------------------------------------------------------

static uint32_t Top;                // the first program row shown
static char     Message[128];
static uint8_t  MessageColor = NORMAL;
static bool     Asking;             // "End ...? y/n"

static uint32_t ChosenIndex()
{
    for (uint32_t i = 0; i < ProgramCount; i++)
        if (Programs[i].Stats.Id == Chosen)
            return i;
    return 0;
}

static void DrawHeader()
{
    Fill(0, 0, Columns, ' ', HEADER);
    Text(1, 0, "SurfaceOS task manager", HEADER);

    char Line[64];
    SfDateTime T;
    Sys->Time->GetTime(Sys->Time, &T);
    char* p = Append(Line, "up ");
    p = Duration(p, Now);
    p = Append(p, "   ");
    p = TwoDigits(p, T.Hour);   *p++ = ':';
    p = TwoDigits(p, T.Minute); *p++ = ':';
    p = TwoDigits(p, T.Second);
    Text(Columns - (uint32_t)(p - Line) - 1, 0, Line, HEADER);
}

static uint32_t DrawMemory(uint32_t Y)
{
    Frame(0, Y, Columns, 3, "Memory");
    uint64_t Used = Info.MemoryTotal - Info.MemoryFree;
    uint32_t P = Percent(Used, Info.MemoryTotal);

    char Line[96];
    char* p = Size(Line, Used);
    p = Append(p, " of ");
    p = Size(p, Info.MemoryTotal);
    p = Append(p, " used, ");
    p = Size(p, Info.MemoryFree);
    Append(p, " free");

    uint32_t BarWidth = Columns > 70 ? Columns - 50 : 10;
    Bar(2, Y + 1, BarWidth, P);
    Number(Line + 80, P, 4);
    Append(Line + 84, "%");
    Text(2 + BarWidth + 1, Y + 1, Line + 80, BRIGHT);
    Text(2 + BarWidth + 8, Y + 1, Line, NORMAL);
    return Y + 3;
}

static uint32_t DrawCpus(uint32_t Y)
{
    // As many columns of CPUs as fit, each "CPU 12 [bar] 100%".
    const uint32_t Cell = 34;
    uint32_t PerRow = (Columns - 2) / Cell;
    if (!PerRow)
        PerRow = 1;
    uint32_t Lines = (Info.CpuCount + PerRow - 1) / PerRow;
    Frame(0, Y, Columns, Lines + 3, "Processor");

    char Line[96];
    char* p = Append(Line, Info.CpuName[0] ? Info.CpuName : "CPU");
    p = Append(p, ", ");
    p = Number(p, Info.CpuCount);
    Append(p, Info.CpuCount == 1 ? " core" : " cores");
    Text(2, Y + 1, Line, NORMAL);

    uint64_t Busy = 0;
    for (uint32_t i = 0; i < Info.CpuCount; i++)
    {
        uint32_t X = 2 + (i % PerRow) * Cell, Row = Y + 2 + i / PerRow;
        p = Append(Line, "CPU ");
        Number(p, i, 2);
        Text(X, Row, Line, NORMAL);
        uint32_t Load = CpuLoad(i);
        Busy += Load;
        Bar(X + 7, Row, Cell - 14, Load);
        Number(Line, Load, 4);
        Append(Line + 4, "%");
        Text(X + Cell - 7, Row, Line, BRIGHT);
    }

    p = Append(Line, " all ");
    p = Number(p, Info.CpuCount ? (uint32_t)(Busy / Info.CpuCount) : 0);
    Append(p, "% ");
    Text(Columns - (uint32_t)(p - Line) - 3, Y, Line, BRIGHT);
    return Y + Lines + 3;
}

static void DrawPrograms(uint32_t Y)
{
    uint32_t Height = Rows - 1 - Y;
    if (Height < 4)
        return;
    uint32_t Threads = 0;
    for (uint32_t i = 0; i < ProgramCount; i++)
        Threads += Programs[i].Stats.Threads;
    char Title[64];
    char* p = Append(Title, "Programs: ");
    p = Number(p, ProgramCount);
    p = Append(p, ", threads: ");
    Number(p, Threads);
    Frame(0, Y, Columns, Height, Title);

    //        ID  NAME   WHERE STATE    CPU  CPU TIME  MEMORY THREADS
    Text(2, Y + 1, "   ID  NAME                WHERE  STATE    CPU   CPU TIME    MEMORY  THREADS",
         BRIGHT);

    uint32_t Shown = Height - 3;
    uint32_t Index = ChosenIndex();
    if (Index < Top)
        Top = Index;
    if (Index >= Top + Shown)
        Top = Index - Shown + 1;
    if (Top + Shown > ProgramCount)
        Top = ProgramCount > Shown ? ProgramCount - Shown : 0;

    for (uint32_t r = 0; r < Shown && Top + r < ProgramCount; r++)
    {
        const SfProcessStats* S = &Programs[Top + r].Stats;
        uint32_t Row = Y + 2 + r;
        bool Mine = S->Id == Chosen;
        uint8_t Color = Mine ? CHOSEN : (S->Flags & SF_PROCESS_PAUSED) ? DIM : NORMAL;
        Fill(1, Row, Columns - 2, ' ', Color);

        char Line[128], Part[32];
        p = Number(Line, S->Id, 5);
        p = Append(p, "  ");
        char* Name = p;
        p = Append(p, S->Name);
        while (p < Name + 20)
            *p++ = ' ';
        *p = '\0';
        if (S->Screen)
        {
            p = Append(p, "F");
            p = Number(p, S->Screen);
            p = Append(p, S->Screen < 10 ? "     " : "    ");
        }
        else
            p = Append(p, "bg     ");
        p = Append(p, (S->Flags & SF_PROCESS_PAUSED) ? "paused" : "runs  ");
        Append(Number(Part, Programs[Top + r].Load), "%");
        p = Right(p, Part, 6);
        Duration(Part, S->CpuTime);
        p = Right(p, Part, 11);
        Size(Part, S->Memory);
        p = Right(p, Part, 10);
        Number(Part, S->Threads);
        p = Right(p, Part, 9);
        if (S->Flags & SF_PROCESS_ADMIN)
            Append(p, "  admin");
        Text(2, Row, Line, Color);
    }
}

static void DrawFooter()
{
    uint32_t Y = Rows - 1;
    Fill(0, Y, Columns, ' ', NORMAL);
    if (Message[0])
    {
        Text(1, Y, Message, MessageColor);
        return;
    }
    char Keys[] = { ' ', SF_ARROW_UP, SF_ARROW_DOWN, 0 };
    uint32_t X = Text(0, Y, Keys, BRIGHT);
    X = Text(X, Y, " choose   ", NORMAL);
    X = Text(X, Y, "Del", BRIGHT);
    X = Text(X, Y, " end the program   ", NORMAL);
    X = Text(X, Y, "q", BRIGHT);
    Text(X, Y, " quit", NORMAL);
}

static void Draw()
{
    DrawHeader();
    uint32_t Y = DrawMemory(1);
    Y = DrawCpus(Y);
    DrawPrograms(Y);
    DrawFooter();
    Con->Draw(Con, 0, 0, Columns, Rows, Cells);
}

// --- keys ------------------------------------------------------------------

// The next key, when one comes within TimeoutMs milliseconds.
static bool NextKey(SfKey* Key, uint64_t TimeoutMs)
{
    SfWaitItem Item = { SF_WAIT_KEY, 0, nullptr };
    return Sys->Sync->WaitAny(Sys->Sync, 1, &Item, TimeoutMs, nullptr) == SF_SUCCESS &&
           Con->ReadKey(Con, Key) == SF_SUCCESS;
}

static void Say(const char* Text, uint8_t Color)
{
    char* p = Message;
    for (uint32_t i = 0; Text[i] && i + 1 < sizeof(Message); i++)
        *p++ = Text[i];
    *p = '\0';
    MessageColor = Color;
}

// One key; false when it is time to go.
static bool OnKey(const SfKey& Key)
{
    uint32_t Index = ChosenIndex();
    const SfProcessStats* S = ProgramCount ? &Programs[Index].Stats : nullptr;

    if (Asking)
    {
        Asking = false;
        Message[0] = '\0';
        if (S && (Key.Char == 'y' || Key.Char == 'Y' || Key.Code == SF_KEY_ENTER))
        {
            char Line[96];
            char* p = Append(Line, "[");
            p = Number(p, S->Id);
            p = Append(p, "] ");
            p = Append(p, S->Name);
            if (SF_ERROR(Admin->EndProcess(Admin, S->Id)))
                Append(p, " is gone already");
            else
                Append(p, " ended");
            Say(Line, BRIGHT);
        }
        return true;
    }

    Message[0] = '\0';
    if (Key.Char == 'q' || Key.Char == 'Q' || Key.Code == SF_KEY_ESCAPE ||
        Key.Code == SF_KEY_F10 || ((Key.Mods & SF_MOD_CTRL) && Key.Char == 3))
        return false;

    switch (Key.Code)
    {
        case SF_KEY_UP:
            if (Index > 0)
                Chosen = Programs[Index - 1].Stats.Id;
            break;
        case SF_KEY_DOWN:
            if (Index + 1 < ProgramCount)
                Chosen = Programs[Index + 1].Stats.Id;
            break;
        case SF_KEY_PAGE_UP:
            Chosen = Programs[Index > 10 ? Index - 10 : 0].Stats.Id;
            break;
        case SF_KEY_PAGE_DOWN:
            if (ProgramCount)
                Chosen = Programs[Index + 10 < ProgramCount ? Index + 10 : ProgramCount - 1]
                             .Stats.Id;
            break;
        case SF_KEY_HOME:
            if (ProgramCount)
                Chosen = Programs[0].Stats.Id;
            break;
        case SF_KEY_END:
            if (ProgramCount)
                Chosen = Programs[ProgramCount - 1].Stats.Id;
            break;
        case SF_KEY_DELETE:
            if (S)
            {
                char Line[96];
                char* p = Append(Line, "End [");
                p = Number(p, S->Id);
                p = Append(p, "] ");
                p = Append(p, S->Name);
                Append(p, "? y - yes, any other key - no");
                Say(Line, WARNING);
                Asking = true;
            }
            break;
    }
    return true;
}

SfStatus SfMain(SfApp*, SfSystem* System)
{
    Sys   = System;
    Con   = System->Console;
    Admin = System->Admin;
    if (!Admin || !SF_HAS_FIELD(Admin, SfAdmin, GetProcessInfo))
    {
        Con->Print(Con, "taskmgr needs the admin right: run it as  sudo taskmgr\n");
        return SF_ACCESS_DENIED;
    }
    if (!SF_HAS_FIELD(Sys->Sync, SfSync, WaitAny))
        return SF_UNSUPPORTED;

    Con->SetMode(Con, SF_CONSOLE_RAW);
    Con->GetSize(Con, &Columns, &Rows);
    if (Columns > MAX_COLUMNS) Columns = MAX_COLUMNS;
    if (Rows > MAX_ROWS)       Rows    = MAX_ROWS;

    Sample();
    Sys->Process->GetId(Sys->Process, &Chosen);     // start on itself
    for (;;)
    {
        Draw();
        // Until a key, or the next sample a second after the last one;
        // then every key that is there.
        uint64_t T;
        Sys->Time->GetUptime(Sys->Time, &T);
        SfKey Key;
        for (uint64_t Wait = T - Now < 1000 ? 1000 - (T - Now) : 0; NextKey(&Key, Wait); Wait = 0)
            if (!OnKey(Key))
            {
                Con->SetMode(Con, SF_CONSOLE_LINE);
                return SF_SUCCESS;
            }
        Sys->Time->GetUptime(Sys->Time, &T);
        if (T - Now >= 1000)
            Sample();
    }
}
