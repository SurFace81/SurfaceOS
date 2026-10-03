// explorer: the editor.
//
// The whole file sits in memory as one run of bytes (Data), and there are
// two ways to look at it and change it: as text, the lines numbered down
// the left, and as hex, each row of 16 bytes under its offset. F4 goes from
// one to the other; a file that does not look like text opens as hex.
//
// Every change is an insertion or a removal of bytes at a place, and is
// written down (Changes) with the bytes themselves, so it can be undone and
// done again. Changes of one group go together: typing a word, or the
// removal and insertion that replacing a byte is.

#include "explorer.h"

static const uint64_t NONE     = ~0ULL;
static const uint64_t MAX_FILE = 64ULL * 1024 * 1024;
static const uint32_t TAB      = 4;
static const uint32_t PER_ROW  = 16;        // bytes in a hex row

static const uint8_t COL_TEXT   = SF_CELL_COLOR(SF_COLOR_BRIGHT | SF_COLOR_WHITE, SF_COLOR_BLUE);
static const uint8_t COL_GUTTER = SF_CELL_COLOR(SF_COLOR_CYAN, SF_COLOR_BLUE);
static const uint8_t COL_HERE   = SF_CELL_COLOR(SF_COLOR_BRIGHT | SF_COLOR_YELLOW, SF_COLOR_BLUE);
static const uint8_t COL_SELECT = SF_CELL_COLOR(SF_COLOR_BLACK, SF_COLOR_CYAN);
static const uint8_t COL_STATUS = SF_CELL_COLOR(SF_COLOR_BLACK, SF_COLOR_CYAN);

// --- the bytes -------------------------------------------------------------

static uint8_t* Data;
static uint64_t Size, Capacity;

static char FilePath[PATH_SIZE];
static bool ReadOnly;
static bool HexMode;

static uint64_t Cur;                // the byte the cursor is at, 0..Size
static uint64_t Anchor = NONE;      // the other end of the selection

// What was cut or copied; it stays from one file to the next.
static uint8_t* Clip;
static uint64_t ClipSize;

static bool RawInsert(uint64_t Pos, const uint8_t* Bytes, uint64_t Count)
{
    if (Size + Count > Capacity)
    {
        uint64_t Wanted = (Size + Count) * 2 + 65536;
        uint8_t* Bigger = (uint8_t*)Alloc(Wanted);
        if (!Bigger)
            return false;
        memcpy(Bigger, Data, Size);
        Release(Data);
        Data = Bigger;
        Capacity = Wanted;
    }
    memmove(Data + Pos + Count, Data + Pos, Size - Pos);
    memcpy(Data + Pos, Bytes, Count);
    Size += Count;
    return true;
}

static void RawRemove(uint64_t Pos, uint64_t Count)
{
    memmove(Data + Pos, Data + Pos + Count, Size - Pos - Count);
    Size -= Count;
}

// --- changes: undo and redo ------------------------------------------------

struct Change
{
    uint64_t Pos, Count;
    uint8_t* Bytes;                 // what was inserted or removed
    uint32_t Group;
    bool     Insert;
};

static Change*  Changes;
static uint32_t ChangeCount, ChangeCapacity;
static uint32_t UndoTop;            // Changes below it are done, above undone
static uint32_t Group;
static bool     Grouping;           // the changes being made are one group
static bool     Sealed;             // the last change takes no more typing
static sint64_t SavedTop;           // UndoTop when the file was saved, -1: lost

static bool Modified()
{
    return SavedTop != (sint64_t)UndoTop;
}

static bool Record(bool Insert, uint64_t Pos, const uint8_t* Bytes, uint64_t Count)
{
    // What was undone is gone for good now.
    while (ChangeCount > UndoTop)
        Release(Changes[--ChangeCount].Bytes);
    if (SavedTop > (sint64_t)UndoTop)
        SavedTop = -1;

    // A character typed right after the last one joins it.
    if (Insert && Count == 1 && !Sealed && !Grouping && ChangeCount && Bytes[0] != '\n')
    {
        Change* Last = &Changes[ChangeCount - 1];
        if (Last->Insert && Last->Pos + Last->Count == Pos)
        {
            uint8_t* Longer = (uint8_t*)Alloc(Last->Count + 1);
            if (!Longer)
                return false;
            memcpy(Longer, Last->Bytes, Last->Count);
            Longer[Last->Count++] = Bytes[0];
            Release(Last->Bytes);
            Last->Bytes = Longer;
            return true;
        }
    }

    if (ChangeCount == ChangeCapacity)
    {
        uint32_t Wanted = ChangeCapacity ? ChangeCapacity * 2 : 256;
        Change* Bigger = (Change*)Alloc(Wanted * sizeof(Change));
        if (!Bigger)
            return false;
        memcpy(Bigger, Changes, ChangeCount * sizeof(Change));
        Release(Changes);
        Changes = Bigger;
        ChangeCapacity = Wanted;
    }
    uint8_t* Kept = (uint8_t*)Alloc(Count);
    if (!Kept)
        return false;
    memcpy(Kept, Bytes, Count);
    if (!Grouping)
        Group++;
    Changes[ChangeCount++] = { Pos, Count, Kept, Group, Insert };
    UndoTop = ChangeCount;
    Sealed = false;
    return true;
}

static bool Insert(uint64_t Pos, const uint8_t* Bytes, uint64_t Count)
{
    if (!Count || !RawInsert(Pos, Bytes, Count))
        return false;
    if (Record(true, Pos, Bytes, Count))
        return true;
    RawRemove(Pos, Count);              // no memory to remember it: not done
    return false;
}

static void Remove(uint64_t Pos, uint64_t Count)
{
    if (Count && Record(false, Pos, Data + Pos, Count))
        RawRemove(Pos, Count);
}

static void BeginGroup()
{
    Group++;
    Grouping = true;
}

static void EndGroup()
{
    Grouping = false;
    Sealed = true;
}

static void Undo()
{
    if (!UndoTop)
        return;
    uint32_t G = Changes[UndoTop - 1].Group;
    while (UndoTop && Changes[UndoTop - 1].Group == G)
    {
        Change* C = &Changes[--UndoTop];
        if (C->Insert)
        {
            RawRemove(C->Pos, C->Count);
            Cur = C->Pos;
        }
        else
        {
            RawInsert(C->Pos, C->Bytes, C->Count);
            Cur = C->Pos + C->Count;
        }
    }
    Sealed = true;
    Anchor = NONE;
}

static void Redo()
{
    if (UndoTop == ChangeCount)
        return;
    uint32_t G = Changes[UndoTop].Group;
    while (UndoTop < ChangeCount && Changes[UndoTop].Group == G)
    {
        Change* C = &Changes[UndoTop++];
        if (C->Insert)
        {
            RawInsert(C->Pos, C->Bytes, C->Count);
            Cur = C->Pos + C->Count;
        }
        else
        {
            RawRemove(C->Pos, C->Count);
            Cur = C->Pos;
        }
    }
    Sealed = true;
    Anchor = NONE;
}

// --- the selection and the clipboard ---------------------------------------

static bool Selected(uint64_t* From, uint64_t* To)
{
    if (Anchor == NONE || Anchor == Cur)
        return false;
    *From = Anchor < Cur ? Anchor : Cur;
    *To   = Anchor < Cur ? Cur : Anchor;
    if (*To > Size)
        *To = Size;
    return *From < *To;
}

static bool DeleteSelection()
{
    uint64_t From, To;
    if (!Selected(&From, &To))
        return false;
    Remove(From, To - From);
    Cur = From;
    Anchor = NONE;
    return true;
}

static void CopySelection()
{
    uint64_t From, To;
    if (!Selected(&From, &To))
        return;
    uint8_t* Kept = (uint8_t*)Alloc(To - From);
    if (!Kept)
        return;
    memcpy(Kept, Data + From, To - From);
    Release(Clip);
    Clip = Kept;
    ClipSize = To - From;
}

static void Paste()
{
    if (!ClipSize)
        return;
    BeginGroup();
    DeleteSelection();
    if (Insert(Cur, Clip, ClipSize))
        Cur += ClipSize;
    EndGroup();
}

// --- text: lines -----------------------------------------------------------

static uint64_t* Lines;             // where each line starts
static uint64_t  LineCount, LineCapacity;
static uint64_t  TopLine, LeftCol;
static uint64_t  Goal;              // the column Up and Down try to keep

static void FindLines()
{
    uint64_t Count = 1;
    for (uint64_t i = 0; i < Size; i++)
        Count += Data[i] == '\n';
    if (Count > LineCapacity)
    {
        Release(Lines);
        LineCapacity = Count * 2 + 1024;
        Lines = (uint64_t*)Alloc(LineCapacity * sizeof(uint64_t));
        if (!Lines)                 // no memory for lines: hex needs none
        {
            LineCapacity = LineCount = 0;
            HexMode = true;
            return;
        }
    }
    LineCount = 0;
    Lines[LineCount++] = 0;
    for (uint64_t i = 0; i < Size; i++)
        if (Data[i] == '\n')
            Lines[LineCount++] = i + 1;
}

static uint64_t LineOf(uint64_t Pos)
{
    uint64_t Low = 0, High = LineCount;     // Lines[Low] <= Pos < Lines[High]
    while (High - Low > 1)
    {
        uint64_t Mid = (Low + High) / 2;
        if (Lines[Mid] <= Pos)
            Low = Mid;
        else
            High = Mid;
    }
    return Low;
}

// Where the line's text ends: before its line break, CR LF or LF.
static uint64_t LineEnd(uint64_t Line)
{
    uint64_t End = Line + 1 < LineCount ? Lines[Line + 1] - 1 : Size;
    if (End > Lines[Line] && Data[End - 1] == '\r' && Line + 1 < LineCount)
        End--;
    return End;
}

// The column on screen of Pos in its line: a tab goes to the next stop.
static uint64_t ColumnOf(uint64_t Line, uint64_t Pos)
{
    uint64_t Col = 0;
    for (uint64_t i = Lines[Line]; i < Pos; i++)
        Col = Data[i] == '\t' ? (Col / TAB + 1) * TAB : Col + 1;
    return Col;
}

static uint64_t PosAtColumn(uint64_t Line, uint64_t Wanted)
{
    uint64_t Col = 0, End = LineEnd(Line), i = Lines[Line];
    for (; i < End && Col < Wanted; i++)
        Col = Data[i] == '\t' ? (Col / TAB + 1) * TAB : Col + 1;
    return i;
}

static bool WordChar(uint8_t C)
{
    return (C >= '0' && C <= '9') || (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') ||
           C == '_' || C >= 128;
}

// --- finding ---------------------------------------------------------------

static uint8_t  Pattern[128];
static uint64_t PatternSize;
static bool     PatternBytes;       // exact bytes; otherwise text, any case

static bool MatchAt(uint64_t Pos)
{
    for (uint64_t i = 0; i < PatternSize; i++)
        if (PatternBytes ? Data[Pos + i] != Pattern[i]
                         : Lower((char)Data[Pos + i]) != Lower((char)Pattern[i]))
            return false;
    return true;
}

// From Start on, then round from the top. The find is selected.
static void FindNext(uint64_t Start)
{
    if (!PatternSize || PatternSize > Size)
        return Message("Find", PatternSize ? "Not found" : "Nothing to look for");
    uint64_t Places = Size - PatternSize + 1;
    for (uint64_t n = 0; n < Places; n++)
    {
        uint64_t Pos = (Start + n) % Places;
        if (!MatchAt(Pos))
            continue;
        Anchor = HexMode ? Pos + PatternSize : Pos;
        Cur    = HexMode ? Pos : Pos + PatternSize;
        return;
    }
    Message("Find", "Not found");
}

static int HexDigit(char C)
{
    if (C >= '0' && C <= '9') return C - '0';
    if (C >= 'a' && C <= 'f') return C - 'a' + 10;
    if (C >= 'A' && C <= 'F') return C - 'A' + 10;
    return -1;
}

// --- hex -------------------------------------------------------------------

static uint64_t HexTop;             // the first row shown
static bool     Nibble;             // the cursor is on the low digit
static bool     AsciiSide;          // typing goes to the characters

// One byte in place of the one at the cursor, or a new one at the end.
static void PutByte(uint8_t Value)
{
    BeginGroup();
    if (Cur < Size)
        Remove(Cur, 1);
    Insert(Cur, &Value, 1);
    EndGroup();
}

// --- drawing ---------------------------------------------------------------

static void DrawStatus()
{
    Fill(0, 0, Columns, ' ', COL_STATUS);
    char Line[160];
    char* p = Line;
    if (HexMode)
    {
        p = Append(p, "Hex   offset ");
        p = HexNumber(p, Cur, 8);
        p = Append(p, AsciiSide ? "   characters" : "   bytes");
    }
    else
    {
        uint64_t L = LineOf(Cur);
        p = Append(p, "Text   line ");
        p = Number(p, L + 1);
        p = Append(p, " of ");
        p = Number(p, LineCount);
        p = Append(p, ", column ");
        p = Number(p, ColumnOf(L, Cur) + 1);
    }
    p = Append(p, "   ");
    p = Number(p, Size);
    p = Append(p, " bytes");
    if (ReadOnly)
        Append(p, "   view only");
    uint32_t n = (uint32_t)Length(Line);
    uint32_t At = Columns > n + 1 ? Columns - n - 1 : 0;
    Text(At, 0, Line, COL_STATUS);

    // The name, with a star while there are changes not saved.
    uint32_t Room = At > 4 ? At - 4 : 0;
    uint64_t Len = Length(FilePath);
    Put(1, 0, Modified() ? '*' : ' ', COL_STATUS);
    TextIn(2, 0, Len > Room ? FilePath + (Len - Room) : FilePath, Room, COL_STATUS);
}

// Where the cursor goes on the screen, as the last drawing left it.
static uint32_t CursorX, CursorY;

static void DrawText()
{
    uint32_t Height = Rows - 2;
    uint32_t Gutter = 5;                    // digits, and a space
    for (uint64_t n = LineCount; n >= 10000; n /= 10)
        Gutter++;
    uint32_t Width = Columns - Gutter;

    uint64_t Line = LineOf(Cur), Col = ColumnOf(Line, Cur);
    if (Line < TopLine)
        TopLine = Line;
    if (Line >= TopLine + Height)
        TopLine = Line - Height + 1;
    if (Col < LeftCol)
        LeftCol = Col;
    if (Col >= LeftCol + Width)
        LeftCol = Col - Width + 1;

    uint64_t From = 0, To = 0;
    Selected(&From, &To);
    for (uint32_t r = 0; r < Height; r++)
    {
        uint32_t Y = 1 + r;
        uint64_t L = TopLine + r;
        Fill(0, Y, Columns, ' ', COL_TEXT);
        if (L >= LineCount)
        {
            Fill(0, Y, Gutter, ' ', COL_GUTTER);
            continue;
        }
        char Digits[24];
        *Number(Digits, L + 1, Gutter - 1) = '\0';
        Text(0, Y, Digits, L == Line ? COL_HERE : COL_GUTTER);
        Put(Gutter - 1, Y, ' ', COL_GUTTER);

        uint64_t End = LineEnd(L), c = 0;
        for (uint64_t i = Lines[L]; i <= End && c < LeftCol + Width; i++)
        {
            // The line break is a cell too: it shows when it is selected.
            bool In = i >= From && i < To;
            uint8_t Color = In ? COL_SELECT : COL_TEXT;
            uint64_t Next = i < End && Data[i] == '\t' ? (c / TAB + 1) * TAB : c + 1;
            char Glyph = i < End && Data[i] != '\t' ? (char)Data[i] : ' ';
            for (; c < Next; c++)
                if (c >= LeftCol && c < LeftCol + Width && (i < End || In))
                    Put(Gutter + (uint32_t)(c - LeftCol), Y, Glyph, Color);
        }
    }
    DrawStatus();
    static const char* const Names[10] =
        { "Help", "Save", "Next", "Hex", "Go to", "", "Find", "", "", "Quit" };
    KeyBar(Names);
    CursorX = Gutter + (uint32_t)(Col - LeftCol);
    CursorY = 1 + (uint32_t)(Line - TopLine);
}

// offset, two groups of eight bytes, the characters.
static const uint32_t HEX_X   = 10;
static const uint32_t ASCII_X = HEX_X + PER_ROW * 3 + 3;

static uint32_t HexColumn(uint32_t i)
{
    return HEX_X + i * 3 + (i >= 8 ? 1 : 0);
}

static void DrawHex()
{
    uint32_t Height = Rows - 3;
    uint64_t Row = Cur / PER_ROW;
    if (Row < HexTop)
        HexTop = Row;
    if (Row >= HexTop + Height)
        HexTop = Row - Height + 1;

    Fill(0, 1, Columns, ' ', COL_GUTTER);
    Text(0, 1, "  Offset", COL_GUTTER);
    for (uint32_t i = 0; i < PER_ROW; i++)
    {
        char Digits[4];
        HexNumber(Digits, i, 2);
        Text(HexColumn(i), 1, Digits, i == Cur % PER_ROW ? COL_HERE : COL_GUTTER);
        Put(ASCII_X + i, 1, Digits[1], i == Cur % PER_ROW ? COL_HERE : COL_GUTTER);
    }

    uint64_t From = 0, To = 0;
    Selected(&From, &To);
    for (uint32_t r = 0; r < Height; r++)
    {
        uint32_t Y = 2 + r;
        uint64_t Start = (HexTop + r) * PER_ROW;
        Fill(0, Y, Columns, ' ', COL_TEXT);
        if (Start > Size)
            continue;
        char Digits[20];
        HexNumber(Digits, Start, 8);
        Text(0, Y, Digits, Start / PER_ROW == Row ? COL_HERE : COL_GUTTER);
        for (uint32_t i = 0; i < PER_ROW && Start + i < Size; i++)
        {
            uint64_t Pos = Start + i;
            uint8_t B = Data[Pos];
            // The byte under the cursor is marked on the side not typed on.
            bool In = Pos >= From && Pos < To;
            uint8_t Color = In ? COL_SELECT : COL_TEXT;
            HexNumber(Digits, B, 2);
            Text(HexColumn(i), Y, Digits, Pos == Cur && AsciiSide ? COL_SELECT : Color);
            if (In && i + 1 < PER_ROW && Pos + 1 < To)
                Put(HexColumn(i) + 2, Y, ' ', Color);
            Put(ASCII_X + i, Y, B >= 32 && B != 127 ? (char)B : '.',
                Pos == Cur && !AsciiSide ? COL_SELECT : Color);
        }
    }
    DrawStatus();
    static const char* const Names[10] =
        { "Help", "Save", "Next", "Text", "Go to", "", "Find", "", "", "Quit" };
    KeyBar(Names);
    uint32_t i = (uint32_t)(Cur % PER_ROW);
    CursorX = AsciiSide ? ASCII_X + i : HexColumn(i) + (Nibble ? 1 : 0);
    CursorY = 2 + (uint32_t)(Row - HexTop);
}

static void Compose()
{
    if (HexMode)
        DrawHex();
    else
        DrawText();
}

// --- the file --------------------------------------------------------------

bool LooksBinary(const uint8_t* Bytes, uint64_t Count)
{
    if (Count > 4096)
        Count = 4096;
    uint64_t Odd = 0;
    for (uint64_t i = 0; i < Count; i++)
    {
        uint8_t B = Bytes[i];
        if (B == 0)
            return true;
        if (B < 32 && B != '\t' && B != '\n' && B != '\r')
            Odd++;
    }
    return Odd * 10 > Count;
}

// The file into Data; a file that is not there yet is an empty new one.
static bool Load()
{
    Size = 0;
    Capacity = 65536;
    SfFile* File;
    SfStatus Status = Open(FilePath, SF_FILE_READ, &File);
    if (Status == SF_NOT_FOUND)
        return (Data = (uint8_t*)Alloc(Capacity)) != nullptr;
    if (SF_ERROR(Status))
    {
        Message("Cannot open the file", FilePath, Status);
        return false;
    }
    SfDirEntry Info;
    File->GetInfo(File, &Info);
    if (Info.Size > MAX_FILE)
    {
        File->Close(File);
        Message("Edit", "The file is too big to edit: over 64 MB");
        return false;
    }
    Capacity = Info.Size + 65536;
    Data = (uint8_t*)Alloc(Capacity);
    if (!Data)
    {
        File->Close(File);
        Message("Edit", "Not enough memory for the file");
        return false;
    }
    while (Size < Info.Size)
    {
        uint64_t n = Info.Size - Size;
        if (n > 1024 * 1024)
            n = 1024 * 1024;
        Status = File->Read(File, Data + Size, &n);
        if (SF_ERROR(Status) || n == 0)
            break;
        Size += n;
    }
    File->Close(File);
    return true;
}

static bool Save()
{
    SfFile* File;
    SfStatus Status = Open(FilePath, SF_FILE_WRITE | SF_FILE_CREATE | SF_FILE_TRUNCATE, &File);
    for (uint64_t Done = 0; !SF_ERROR(Status) && Done < Size;)
    {
        uint64_t n = Size - Done;
        if (n > 1024 * 1024)
            n = 1024 * 1024;
        uint64_t w = n;
        Status = File->Write(File, Data + Done, &w);
        if (!SF_ERROR(Status) && w != n)
            Status = SF_OUT_OF_RESOURCES;
        Done += n;
    }
    if (File)
        File->Close(File);
    if (SF_ERROR(Status))
    {
        Message("Cannot save the file", FilePath, Status);
        return false;
    }
    SavedTop = UndoTop;
    Sealed = true;
    return true;
}

// --- keys ------------------------------------------------------------------

static void Help()
{
    static const char* const Text[] =
    {
        "F2            save",
        "F4            text <-> hex",
        "F7, Ctrl+F    find; F3 the next one",
        "F5, Ctrl+G    go to a line (hex: an offset)",
        "Ctrl+Z        undo",
        "Ctrl+Y        do again",
        "Shift+arrows  select; Ctrl+A everything",
        "Ctrl+C / X    copy / cut",
        "Ctrl+V        paste",
        "Ctrl+arrows   by words; Ctrl+Home/End: the ends",
        "Esc, F10      leave",
        "",
        "Hex: Tab goes between the bytes and the",
        "characters; what is typed replaces the byte",
        "under the cursor. Ins puts a zero byte in,",
        "Del takes the byte out. Find looks for bytes",
        "(\"4D 5A\") on the byte side, for text on the",
        "character side.",
    };
    Menu("Editor keys", Text, sizeof(Text) / sizeof(Text[0]), 0);
}

static void AskFind()
{
    static char Wanted[128];
    bool Bytes = HexMode && !AsciiSide;
    if (!Input("Find", Bytes ? "Bytes in hex, as 4D 5A:" : "Text (any case):", Wanted,
               sizeof(Wanted)))
        return;
    PatternSize = 0;
    PatternBytes = Bytes;
    if (!Bytes)
    {
        PatternSize = Length(Wanted);
        memcpy(Pattern, Wanted, PatternSize);
    }
    else
        for (const char* c = Wanted; *c; c++)
        {
            if (*c == ' ')
                continue;
            int High = HexDigit(c[0]), Low = High < 0 ? -1 : HexDigit(c[1]);
            if (Low < 0)
                return Message("Find", "Bytes go as pairs of hex digits: 4D 5A 90");
            Pattern[PatternSize++] = (uint8_t)(High * 16 + Low);
            c++;
        }
    FindNext(Cur);
}

static void AskGoTo()
{
    char Wanted[32] = "";
    if (!Input("Go to", HexMode ? "Offset, in hex:" : "Line:", Wanted, sizeof(Wanted)))
        return;
    uint64_t Value = 0;
    for (const char* c = Wanted; *c; c++)
    {
        int Digit = HexDigit(*c);
        if (Digit < 0 || (!HexMode && Digit > 9))
            return;
        Value = Value * (HexMode ? 16 : 10) + (uint64_t)Digit;
    }
    if (HexMode)
        Cur = Value < Size ? Value : Size;
    else
    {
        if (Value < 1)         Value = 1;
        if (Value > LineCount) Value = LineCount;
        Cur = Lines[Value - 1];
    }
    Anchor = NONE;
}

// A key that moves the cursor; false when it is not one.
static bool MoveText(const SfKey& K)
{
    bool Jump = K.Mods & SF_MOD_CTRL;
    uint64_t Line = LineOf(Cur), Page = Rows - 3;
    bool Vertical = false;
    switch (K.Code)
    {
        case SF_KEY_LEFT:
            if (Jump)
            {
                while (Cur > 0 && !WordChar(Data[Cur - 1])) Cur--;
                while (Cur > 0 && WordChar(Data[Cur - 1]))  Cur--;
            }
            else if (Cur > Lines[Line])
                Cur--;
            else if (Line > 0)
                Cur = LineEnd(Line - 1);
            break;
        case SF_KEY_RIGHT:
            if (Jump)
            {
                while (Cur < Size && WordChar(Data[Cur]))  Cur++;
                while (Cur < Size && !WordChar(Data[Cur])) Cur++;
            }
            else if (Cur < LineEnd(Line))
                Cur++;
            else if (Line + 1 < LineCount)
                Cur = Lines[Line + 1];
            break;
        case SF_KEY_HOME:
            Cur = Jump ? 0 : Lines[Line];
            break;
        case SF_KEY_END:
            Cur = Jump ? Size : LineEnd(Line);
            break;
        case SF_KEY_UP:
            Line = Line > 0 ? Line - 1 : 0;
            Vertical = true;
            break;
        case SF_KEY_DOWN:
            Line = Line + 1 < LineCount ? Line + 1 : Line;
            Vertical = true;
            break;
        case SF_KEY_PAGE_UP:
            Line = Line > Page ? Line - Page : 0;
            Vertical = true;
            break;
        case SF_KEY_PAGE_DOWN:
            Line = Line + Page < LineCount ? Line + Page : LineCount - 1;
            Vertical = true;
            break;
        default:
            return false;
    }
    if (Vertical)
        Cur = PosAtColumn(Line, Goal);
    else
        Goal = ColumnOf(LineOf(Cur), Cur);
    return true;
}

static bool MoveHex(const SfKey& K)
{
    bool Jump = K.Mods & SF_MOD_CTRL;
    uint64_t Page = (uint64_t)(Rows - 3) * PER_ROW;
    switch (K.Code)
    {
        case SF_KEY_LEFT:       if (Cur > 0) Cur--; break;
        case SF_KEY_RIGHT:      if (Cur < Size) Cur++; break;
        case SF_KEY_UP:         if (Cur >= PER_ROW) Cur -= PER_ROW; break;
        case SF_KEY_DOWN:       Cur = Cur + PER_ROW <= Size ? Cur + PER_ROW : Cur; break;
        case SF_KEY_PAGE_UP:    Cur = Cur >= Page ? Cur - Page : Cur % PER_ROW; break;
        case SF_KEY_PAGE_DOWN:  Cur = Cur + Page <= Size ? Cur + Page : Size; break;
        case SF_KEY_HOME:       Cur = Jump ? 0 : Cur - Cur % PER_ROW; break;
        case SF_KEY_END:
            Cur = Jump ? Size : Cur - Cur % PER_ROW + PER_ROW - 1;
            if (Cur > Size)
                Cur = Size;
            break;
        default:
            return false;
    }
    Nibble = false;
    return true;
}

static void TypeText(const SfKey& K)
{
    if (K.Code == SF_KEY_ENTER || K.Code == SF_KEY_KP_ENTER)
    {
        // The new line starts under the first character of this one.
        uint8_t Bytes[64];
        uint64_t n = 0;
        Bytes[n++] = '\n';
        uint64_t Start = Lines[LineOf(Cur)];
        for (uint64_t i = Start; i < Cur && n < sizeof(Bytes) &&
                                 (Data[i] == ' ' || Data[i] == '\t'); i++)
            Bytes[n++] = Data[i];
        BeginGroup();
        DeleteSelection();
        if (Insert(Cur, Bytes, n))
            Cur += n;
        EndGroup();
    }
    else if (K.Code == SF_KEY_BACKSPACE)
    {
        if (!DeleteSelection() && Cur > 0)
        {
            uint64_t n = Cur >= 2 && Data[Cur - 1] == '\n' && Data[Cur - 2] == '\r' ? 2 : 1;
            Remove(Cur - n, n);
            Cur -= n;
        }
    }
    else if (K.Code == SF_KEY_DELETE)
    {
        if (!DeleteSelection() && Cur < Size)
            Remove(Cur, Cur + 1 < Size && Data[Cur] == '\r' && Data[Cur + 1] == '\n' ? 2 : 1);
    }
    else if (Types(K) || K.Code == SF_KEY_TAB)
    {
        uint8_t C = K.Code == SF_KEY_TAB ? '\t' : (uint8_t)K.Char;
        uint64_t From, To;
        bool Replacing = Selected(&From, &To);
        if (Replacing)
            BeginGroup();
        DeleteSelection();
        if (Insert(Cur, &C, 1))
            Cur++;
        if (Replacing)
            Grouping = false;           // not sealed: typing goes on in it
    }
    else
        return;
    Anchor = NONE;
    FindLines();
    Goal = ColumnOf(LineOf(Cur), Cur);
}

static void TypeHex(const SfKey& K)
{
    if (K.Code == SF_KEY_TAB)
    {
        AsciiSide = !AsciiSide;
        Nibble = false;
        return;
    }
    if (ReadOnly)
        return;
    if (K.Code == SF_KEY_DELETE)
    {
        if (!DeleteSelection() && Cur < Size)
            Remove(Cur, 1);
        Sealed = true;
    }
    else if (K.Code == SF_KEY_BACKSPACE)
    {
        if (Cur > 0)
            Cur--;
    }
    else if (K.Code == SF_KEY_INSERT)
    {
        uint8_t Zero = 0;
        Insert(Cur, &Zero, 1);
        Sealed = true;
    }
    else if (Types(K) && AsciiSide)
    {
        PutByte((uint8_t)K.Char);
        Cur++;
    }
    else if (Types(K) && HexDigit(K.Char) >= 0)
    {
        uint8_t Old = Cur < Size ? Data[Cur] : 0, D = (uint8_t)HexDigit(K.Char);
        PutByte(Nibble ? (uint8_t)((Old & 0xF0) | D) : (uint8_t)((Old & 0x0F) | (D << 4)));
        if (Nibble)
            Cur++;
        Nibble = !Nibble;
        Anchor = NONE;
        return;
    }
    else
        return;
    Nibble = false;
    Anchor = NONE;
}

// Leaving: changes not saved are asked about. False: stay.
static bool MayLeave()
{
    if (!Modified())
        return true;
    static const char* const Labels[] = { "Save", "Discard", "Cancel" };
    switch (Buttons("Leave the editor", "The file has changes that are not saved:", FilePath,
                    Labels, 3))
    {
        case 0:  return Save();
        case 1:  return true;
        default: return false;
    }
}

void Edit(const char* Path, bool View)
{
    Copy(FilePath, Path, sizeof(FilePath));
    ReadOnly = View;
    if (!Load())
        return;
    Con->SetTitle(Con, NameOf(FilePath));

    Cur = TopLine = LeftCol = Goal = HexTop = 0;
    Anchor = NONE;
    Nibble = AsciiSide = false;
    ChangeCount = UndoTop = 0;
    SavedTop = 0;
    Grouping = false;
    Sealed = true;
    PatternSize = 0;
    HexMode = LooksBinary(Data, Size);
    FindLines();

    void (*Behind)() = Repaint;
    Repaint = Compose;
    for (;;)
    {
        Compose();
        ShowWithCursor(CursorX, CursorY);
        SfKey K = GetKey();
        bool Shift = K.Mods & SF_MOD_SHIFT;

        if (K.Code == SF_KEY_ESCAPE || K.Code == SF_KEY_F10)
        {
            if (MayLeave())
                break;
        }
        else if (K.Code == SF_KEY_F1)
            Help();
        else if (K.Code == SF_KEY_F2)
        {
            if (!ReadOnly)
                Save();
        }
        else if (K.Code == SF_KEY_F3)
            FindNext(HexMode ? Cur + 1 : Cur);
        else if (K.Code == SF_KEY_F4)
        {
            HexMode = !HexMode;
            Nibble = false;
            if (!HexMode)
            {
                FindLines();
                if (!HexMode)
                    Goal = ColumnOf(LineOf(Cur), Cur);
            }
        }
        else if (K.Code == SF_KEY_F7 || Ctrl(K, 'f'))
            AskFind();
        else if (K.Code == SF_KEY_F5 || Ctrl(K, 'g'))
            AskGoTo();
        else if (Ctrl(K, 'a'))
        {
            Anchor = 0;
            Cur = Size;
        }
        else if (Ctrl(K, 'c'))
            CopySelection();
        else if (Ctrl(K, 'z') || Ctrl(K, 'y') || Ctrl(K, 'x') || Ctrl(K, 'v'))
        {
            if (ReadOnly)
                continue;
            if (Ctrl(K, 'z'))
                Undo();
            else if (Ctrl(K, 'y'))
                Redo();
            else if (Ctrl(K, 'v'))
                Paste();
            else
            {
                CopySelection();
                DeleteSelection();
                Sealed = true;
            }
            if (!HexMode)
                FindLines();
        }
        else
        {
            // Shift with a moving key stretches the selection from where
            // the cursor was; a moving key without it drops the selection.
            uint64_t Was = Cur;
            if (HexMode ? MoveHex(K) : MoveText(K))
            {
                if (!Shift)
                    Anchor = NONE;
                else if (Anchor == NONE)
                    Anchor = Was;
                Sealed = true;
            }
            else if (HexMode)
                TypeHex(K);
            else if (!ReadOnly)
                TypeText(K);
        }
        if (Cur > Size)
            Cur = Size;
        if (!HexMode && !LineCount)
            HexMode = true;
    }

    Repaint = Behind;
    while (ChangeCount)
        Release(Changes[--ChangeCount].Bytes);
    Release(Changes);
    Release(Lines);
    Release(Data);
    Changes = nullptr;
    Lines = nullptr;
    Data = nullptr;
    ChangeCapacity = 0;
    LineCapacity = LineCount = 0;
    Con->SetTitle(Con, "");
}
