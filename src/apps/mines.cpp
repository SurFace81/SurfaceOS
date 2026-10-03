// mines: the start of a minesweeper.
//
// A full-screen program (SF_CONSOLE_RAW): a title line, the field in a frame
// in the middle, a panel beside it, a message line and the keys at the
// bottom. The whole screen is put together in Cells and drawn with one Draw
// call, as in taskmgr. Arrows move the cursor, Space opens the cell under
// it, f puts a flag on it or takes it off; q or Esc quits.

#include <sfos.h>

static SfSystem*  Sys;
static SfConsole* Con;

// --- random numbers --------------------------------------------------------

// xorshift64: fast and good enough for a game, never for secrets.
static uint64_t RandomState = 1;

// Seed from the clock: the uptime in ms and the time of day.
static void Seed()
{
    uint64_t Ms;
    SfDateTime T;
    Sys->Time->GetUptime(Sys->Time, &Ms);
    Sys->Time->GetTime(Sys->Time, &T);
    RandomState ^= Ms * 0x9E3779B97F4A7C15ull;
    RandomState ^= ((uint64_t)T.Hour << 16 | (uint64_t)T.Minute << 8 | T.Second)
                   * 0xBF58476D1CE4E5B9ull;
    if (!RandomState)
        RandomState = 1;            // xorshift never leaves 0
}

// A number in 0..Limit-1 (Limit > 0).
static uint32_t Random(uint32_t Limit)
{
    RandomState ^= RandomState << 13;
    RandomState ^= RandomState >> 7;
    RandomState ^= RandomState << 17;
    return (uint32_t)(RandomState % Limit);
}

// --- the field -------------------------------------------------------------

static const uint32_t WIDTH  = 9;
static const uint32_t HEIGHT = 9;
static const uint32_t MINES  = 10;

static bool     Open[WIDTH * HEIGHT];
static bool     Mine[WIDTH * HEIGHT];
static bool     Flag[WIDTH * HEIGHT];
static uint32_t FlagCount;
static uint32_t CursorX, CursorY;
static uint32_t OpenedCount;        // cells opened; all but the mines: won
static bool     Over;               // lost or won: the field takes no more moves
static bool     Lost;
static const char* Message = "";    // the line above the keys

// The time: from the first move to the end of the game.
static bool     Started;            // the first cell is opened, the clock runs
static uint64_t StartMs, StopMs;    // uptime then and when the game ended

static uint64_t Uptime()
{
    uint64_t Ms;
    Sys->Time->GetUptime(Sys->Time, &Ms);
    return Ms;
}

// Seconds of the game so far, at most 999 as in the classic one.
static uint64_t Seconds()
{
    if (!Started)
        return 0;
    uint64_t S = ((Over ? StopMs : Uptime()) - StartMs) / 1000;
    return S > 999 ? 999 : S;
}

static void PlaceMines(uint32_t SafeX, uint32_t SafeY)
{
    Seed();
    for (uint32_t i = 0; i < WIDTH * HEIGHT; i++)
        Mine[i] = false;

    for (uint32_t i = 0; i < MINES; i++)
    {
        uint32_t x, y;
        do
        {
            x = Random(WIDTH);
            y = Random(HEIGHT);
        } while (Mine[y * WIDTH + x] || (x == SafeX && y == SafeY));
        Mine[y * WIDTH + x] = true;
    }
}

// How many mines are around (x, y).
static uint32_t MinesAround(uint32_t x, uint32_t y)
{
    uint32_t Count = 0;
    for (uint32_t ny = y ? y - 1 : 0; ny <= y + 1 && ny < HEIGHT; ny++)
        for (uint32_t nx = x ? x - 1 : 0; nx <= x + 1 && nx < WIDTH; nx++)
            if ((nx != x || ny != y) && Mine[ny * WIDTH + nx])
                Count++;
    return Count;
}

// Open (x, y), and when no mine is around it, everything around it too -
// on and on, through a stack of the cells still to look around, not by
// recursion: each cell goes on it once, when it is opened, so WIDTH * HEIGHT
// is always enough.
static void OpenFrom(uint32_t x, uint32_t y)
{
    static uint32_t Stack[WIDTH * HEIGHT];
    uint32_t Top = 0;

    Open[y * WIDTH + x] = true;
    OpenedCount++;
    Stack[Top++] = y * WIDTH + x;

    while (Top)
    {
        uint32_t i = Stack[--Top];
        uint32_t cx = i % WIDTH, cy = i / WIDTH;
        if (MinesAround(cx, cy))
            continue;               // a number: the opening stops here

        // No mine around, so none of these is a mine either.
        for (uint32_t ny = cy ? cy - 1 : 0; ny <= cy + 1 && ny < HEIGHT; ny++)
            for (uint32_t nx = cx ? cx - 1 : 0; nx <= cx + 1 && nx < WIDTH; nx++)
            {
                uint32_t n = ny * WIDTH + nx;
                if (Open[n] || Flag[n])
                    continue;
                Open[n] = true;
                OpenedCount++;
                Stack[Top++] = n;
            }
    }
}

// f on the cell under the cursor: a flag on a closed cell, or off again.
static void ToggleFlag()
{
    uint32_t i = CursorY * WIDTH + CursorX;
    if (Over || Open[i])
        return;                     // an open cell needs no flag
    Flag[i] = !Flag[i];
    if (Flag[i])
        FlagCount++;
    else
        FlagCount--;
}

// Space on the cell under the cursor.
static void OpenCell()
{
    uint32_t i = CursorY * WIDTH + CursorX;
    if (Over || Open[i] || Flag[i])
        return;

    if (!Started)                   // the first move: no mine under it
    {
        PlaceMines(CursorX, CursorY);
        Started = true;
        StartMs = Uptime();
    }

    if (Mine[i])
    {
        Open[i] = true;             // the one that went off
        Over = Lost = true;
        StopMs = Uptime();
        Message = "Boom! You lost.";
        return;
    }

    OpenFrom(CursorX, CursorY);
    if (OpenedCount == WIDTH * HEIGHT - MINES)
    {
        Over = true;
        StopMs = Uptime();
        Message = "All mines found - you win!";
    }
}

// --- the screen ------------------------------------------------------------

static const uint32_t MAX_COLUMNS = 256;
static const uint32_t MAX_ROWS    = 128;
static SfCell   Cells[MAX_COLUMNS * MAX_ROWS];
static uint32_t Columns, Rows;

static const uint8_t NORMAL  = SF_CELL_COLOR(SF_COLOR_WHITE, SF_COLOR_BLACK);
static const uint8_t BRIGHT  = SF_CELL_COLOR(SF_COLOR_BRIGHT | SF_COLOR_WHITE, SF_COLOR_BLACK);
static const uint8_t FRAME   = SF_CELL_COLOR(SF_COLOR_WHITE, SF_COLOR_BLACK);
static const uint8_t HEADER  = SF_CELL_COLOR(SF_COLOR_BLACK, SF_COLOR_WHITE);

// The cells: dark grey tiles in grey lines, opened ones white. The font is
// one pixel thin, so every symbol stands in a strong contrast: dark ink on
// light paper, or white ink on dark. Colour 7 (SF_COLOR_WHITE) is grey on
// this console, bright black dark grey, bright white white.
static const uint8_t CLOSED_PAPER = SF_COLOR_BRIGHT | SF_COLOR_BLACK;
static const uint8_t OPENED_PAPER = SF_COLOR_BRIGHT | SF_COLOR_WHITE;
static const uint8_t CURSOR_PAPER = SF_COLOR_BRIGHT | SF_COLOR_YELLOW;
static const uint8_t FLAG_PAPER   = SF_COLOR_RED;                   // white F
static const uint8_t LIGHT_INK    = SF_COLOR_BRIGHT | SF_COLOR_WHITE;

// The mines, shown when the game is lost: like an opened cell, white, with
// the mine in red - red is the mines' colour, the flags' too.
static const char    MINE_CHAR    = (char)0x0F;                     // ☼ (code page 437)
static const uint8_t MINE_INK     = SF_COLOR_RED;

// The numbers 1..8 each on a paper of its own, as the flags are; the ink is
// white or black, whichever stands out more on it. Red is the flags', so 3
// is magenta.
static const uint8_t NUMBER_PAPER[9] =
{
    OPENED_PAPER,
    SF_COLOR_BLUE,                      // 1
    SF_COLOR_GREEN,                     // 2
    SF_COLOR_MAGENTA,                   // 3
    SF_COLOR_YELLOW,                    // 4 (brown)
    SF_COLOR_CYAN,                      // 5
    SF_COLOR_BRIGHT | SF_COLOR_BLUE,    // 6
    SF_COLOR_BLACK,                     // 7
    SF_COLOR_WHITE,                     // 8 (grey)
};
static const uint8_t NUMBER_INK[9] =
{
    SF_COLOR_BLACK,
    LIGHT_INK,                          // 1
    SF_COLOR_BLACK,                     // 2
    LIGHT_INK,                          // 3
    LIGHT_INK,                          // 4
    SF_COLOR_BLACK,                     // 5
    LIGHT_INK,                          // 6
    LIGHT_INK,                          // 7
    SF_COLOR_BLACK,                     // 8
};

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

// Value in decimal, right-aligned in Width (0: as long as it is) into Out;
// where it ends.
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

    Put(X + 1, Y, ' ', BRIGHT);
    uint32_t End = Text(X + 2, Y, Title, BRIGHT);
    Put(End, Y, ' ', BRIGHT);
}

// --- drawing ---------------------------------------------------------------

// The field is a grid of lines; a cell is 3 characters and the line beside
// it wide, 1 character and the line below it high: 4 x 2 characters, which
// with the 8 x 16 font is 32 x 32 pixels - square, with the symbol in the
// middle of the 3.
//
//   ┌───┬───┐
//   │ 1 │ ▒ │
//   ├───┼───┤
static const uint32_t CELL_W = 4;
static const uint32_t CELL_H = 2;
static const uint32_t FIELD_W = WIDTH * CELL_W + 1;     // in characters
static const uint32_t FIELD_H = HEIGHT * CELL_H + 1;

static const uint32_t PANEL_WIDTH = 20;

static void DrawHeader()
{
    Fill(0, 0, Columns, ' ', HEADER);
    Text(1, 0, "Mines", HEADER);
    // TODO: what goes on the right of the title line.
}

// The line character where the grid lines cross at (gx, gy).
static char Crossing(uint32_t gx, uint32_t gy)
{
    bool Top = gy == 0, Bottom = gy == FIELD_H - 1;
    bool Left = gx == 0, Right = gx == FIELD_W - 1;
    if (Top)    return Left ? SF_BOX_TOP_LEFT    : Right ? SF_BOX_TOP_RIGHT    : SF_BOX_T_DOWN;
    if (Bottom) return Left ? SF_BOX_BOTTOM_LEFT : Right ? SF_BOX_BOTTOM_RIGHT : SF_BOX_T_UP;
    return Left ? SF_BOX_T_RIGHT : Right ? SF_BOX_T_LEFT : SF_BOX_CROSS;
}

static void DrawField(uint32_t X, uint32_t Y)
{
    for (uint32_t gy = 0; gy < FIELD_H; gy++)
        for (uint32_t gx = 0; gx < FIELD_W; gx++)
        {
            bool LineH = gy % CELL_H == 0, LineV = gx % CELL_W == 0;
            if (LineH || LineV)
            {
                char C = LineH && LineV ? Crossing(gx, gy) : LineH ? SF_BOX_H : SF_BOX_V;
                Put(X + gx, Y + gy, C, FRAME);
                continue;
            }

            uint32_t x = gx / CELL_W, y = gy / CELL_H, i = y * WIDTH + x;
            bool Opened = Open[i];
            uint8_t Paper = Opened ? OPENED_PAPER : CLOSED_PAPER;
            uint8_t Ink = SF_COLOR_BLACK;
            char C = ' ';
            if (Flag[i])
            {
                C = 'F';
                Ink = LIGHT_INK;
                Paper = FLAG_PAPER;
                if (Lost && !Mine[i])
                    C = 'X';                           // lost: this flag was wrong
            }
            else if (Lost && Mine[i])
            {
                C = MINE_CHAR;                         // lost: every mine shown
                Ink = MINE_INK;
                Paper = OPENED_PAPER;
            }
            else if (Opened)
            {
                uint32_t Around = MinesAround(x, y);
                C = Around ? (char)('0' + Around) : ' ';
                Ink = NUMBER_INK[Around];
                Paper = NUMBER_PAPER[Around];
            }
            // The cursor shows as a yellow cell; dark ink stays, white would
            // vanish on yellow: a flag goes red, a number black.
            if (x == CursorX && y == CursorY)
            {
                if (Ink == LIGHT_INK)
                    Ink = Flag[i] ? SF_COLOR_RED : SF_COLOR_BLACK;
                Paper = CURSOR_PAPER;
            }
            bool Middle = gx % CELL_W == CELL_W / 2;
            Put(X + gx, Y + gy, Middle ? C : ' ', SF_CELL_COLOR(Ink, Paper));
        }
}

static void DrawPanel(uint32_t X, uint32_t Y)
{
    Frame(X, Y, PANEL_WIDTH, FIELD_H, "Game");
    Text(X + 2, Y + 2, "Mines:", NORMAL);
    Text(X + 2, Y + 3, "Flags:", NORMAL);
    Text(X + 2, Y + 4, "Time:", NORMAL);
    char Line[24];
    Number(Line, FlagCount < MINES ? MINES - FlagCount : 0, 3);
    Text(X + 8, Y + 2, Line, NORMAL);
    Number(Line, FlagCount, 3);
    Text(X + 8, Y + 3, Line, NORMAL);
    Number(Line, Seconds(), 3);
    Text(X + 8, Y + 4, Line, NORMAL);
}

static void DrawFooter()
{
    Fill(0, Rows - 2, Columns, ' ', NORMAL);
    Text(1, Rows - 2, Message, BRIGHT);

    uint32_t Y = Rows - 1;
    Fill(0, Y, Columns, ' ', NORMAL);
    char Arrows[] = { ' ', SF_ARROW_UP, SF_ARROW_DOWN, SF_ARROW_LEFT, SF_ARROW_RIGHT, 0 };
    uint32_t X = Text(0, Y, Arrows, BRIGHT);
    X = Text(X, Y, " move   ", NORMAL);
    X = Text(X, Y, "Space", BRIGHT);
    X = Text(X, Y, " open   ", NORMAL);
    X = Text(X, Y, "F", BRIGHT);
    X = Text(X, Y, " flag   ", NORMAL);
    X = Text(X, Y, "Q", BRIGHT);
    X = Text(X, Y, " quit   ", NORMAL);
    X = Text(X, Y, "R", BRIGHT);
    X = Text(X, Y, " restart   ", NORMAL);
}

static void Draw()
{
    for (uint32_t i = 0; i < Columns * Rows; i++)
    {
        Cells[i].Char  = ' ';
        Cells[i].Color = NORMAL;
    }

    // The field and the panel side by side in the middle of the screen.
    uint32_t Width  = FIELD_W + 1 + PANEL_WIDTH;
    uint32_t Height = FIELD_H;
    uint32_t X = Columns > Width ? (Columns - Width) / 2 : 0;
    uint32_t Y = Rows > Height + 3 ? 1 + (Rows - 3 - Height) / 2 : 1;

    DrawHeader();
    DrawField(X, Y);
    DrawPanel(X + FIELD_W + 1, Y);
    DrawFooter();
    Con->Draw(Con, 0, 0, Columns, Rows, Cells);
}

// --- keys ------------------------------------------------------------------

// ReadKey waits for a key, but the clock has to go on meanwhile: as in
// taskmgr, WaitAny waits for a key until the next second, and ReadKey
// takes it once it is there.
static bool NextKey(SfKey* Key, uint64_t TimeoutMs)
{
    SfWaitItem Item = { SF_WAIT_KEY, 0, nullptr };
    return Sys->Sync->WaitAny(Sys->Sync, 1, &Item, TimeoutMs, nullptr) == SF_SUCCESS &&
           Con->ReadKey(Con, Key) == SF_SUCCESS;
}

// One key; false when it is time to go.
static bool OnKey(const SfKey& Key)
{
    if (Key.Char == 'q' || Key.Char == 'Q' || Key.Code == SF_KEY_ESCAPE ||
        ((Key.Mods & SF_MOD_CTRL) && Key.Char == 3))
        return false;

    if (Key.Char == 'f' || Key.Char == 'F')
    {
        ToggleFlag();
        return true;
    }

    if (Key.Char == 'r' || Key.Char == 'R')
    {
        for (uint32_t i = 0; i < WIDTH * HEIGHT; i++)
            Open[i] = Mine[i] = Flag[i] = false;
        FlagCount = OpenedCount = CursorX = CursorY = 0;
        Over = Lost = Started = false;      // the next move places new mines
        Message = "";
        return true;
    }

    if (Over == true) return true;

    switch (Key.Code)
    {
        case SF_KEY_UP:    if (CursorY > 0)          CursorY--; break;
        case SF_KEY_DOWN:  if (CursorY + 1 < HEIGHT) CursorY++; break;
        case SF_KEY_LEFT:  if (CursorX > 0)          CursorX--; break;
        case SF_KEY_RIGHT: if (CursorX + 1 < WIDTH)  CursorX++; break;
        case SF_KEY_SPACE: OpenCell(); break;
    }
    return true;
}

SfStatus SfMain(SfApp*, SfSystem* System)
{
    Sys = System;
    Con = System->Console;
    if (!SF_HAS_FIELD(Sys->Sync, SfSync, WaitAny))
        return SF_UNSUPPORTED;

    Con->SetMode(Con, SF_CONSOLE_RAW);         // clears the screen, hides the cursor
    Con->GetSize(Con, &Columns, &Rows);
    if (Columns > MAX_COLUMNS) Columns = MAX_COLUMNS;
    if (Rows > MAX_ROWS)       Rows    = MAX_ROWS;

    for (;;)
    {
        Draw();
        // Until a key; while the clock runs, at most until its next second.
        uint64_t Timeout = SF_WAIT_FOREVER;
        if (Started && !Over)
            Timeout = 1000 - (Uptime() - StartMs) % 1000;
        SfKey Key;
        for (; NextKey(&Key, Timeout); Timeout = 0)
            if (!OnKey(Key))
            {
                Con->SetMode(Con, SF_CONSOLE_LINE);
                return SF_SUCCESS;
            }
    }
}
