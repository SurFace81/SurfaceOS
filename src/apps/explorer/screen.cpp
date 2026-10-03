// explorer: the cells of the screen and the dialogs.
//
// Everything is put together in Cells and drawn with one Draw call (Show).
// A dialog draws itself over the screen behind it, which it has Repaint put
// together first - so a dialog that follows another does not sit on it.

#include "explorer.h"

static const uint32_t MAX_COLUMNS = 256;
static const uint32_t MAX_ROWS    = 128;
static SfCell Cells[MAX_COLUMNS * MAX_ROWS];
uint32_t Columns, Rows;
void (*Repaint)();

void InitScreen()
{
    Con->GetSize(Con, &Columns, &Rows);
    if (Columns > MAX_COLUMNS) Columns = MAX_COLUMNS;
    if (Rows > MAX_ROWS)       Rows    = MAX_ROWS;
}

void Put(uint32_t X, uint32_t Y, char C, uint8_t Color)
{
    if (X < Columns && Y < Rows)
    {
        Cells[Y * Columns + X].Char  = C;
        Cells[Y * Columns + X].Color = Color;
    }
}

uint32_t Text(uint32_t X, uint32_t Y, const char* S, uint8_t Color)
{
    for (; *S; S++, X++)
        Put(X, Y, *S, Color);
    return X;
}

// S in exactly Width cells: cut, or padded with spaces.
void TextIn(uint32_t X, uint32_t Y, const char* S, uint32_t Width, uint8_t Color)
{
    for (uint32_t i = 0; i < Width; i++)
    {
        Put(X + i, Y, *S ? *S : ' ', Color);
        if (*S)
            S++;
    }
}

void Fill(uint32_t X, uint32_t Y, uint32_t Width, char C, uint8_t Color)
{
    for (uint32_t i = 0; i < Width; i++)
        Put(X + i, Y, C, Color);
}

// A double frame; what is inside stays.
void Box(uint32_t X, uint32_t Y, uint32_t Width, uint32_t Height, uint8_t Color)
{
    Put(X, Y, SF_BOX2_TOP_LEFT, Color);
    Fill(X + 1, Y, Width - 2, SF_BOX2_H, Color);
    Put(X + Width - 1, Y, SF_BOX2_TOP_RIGHT, Color);
    for (uint32_t y = Y + 1; y + 1 < Y + Height; y++)
    {
        Put(X, y, SF_BOX2_V, Color);
        Put(X + Width - 1, y, SF_BOX2_V, Color);
    }
    Put(X, Y + Height - 1, SF_BOX2_BOTTOM_LEFT, Color);
    Fill(X + 1, Y + Height - 1, Width - 2, SF_BOX2_H, Color);
    Put(X + Width - 1, Y + Height - 1, SF_BOX2_BOTTOM_RIGHT, Color);
}

// The bottom line: "1Help  2Save ..." over the whole width.
void KeyBar(const char* const* Names)
{
    uint32_t Y = Rows - 1, Cell = Columns / 10;
    Fill(0, Y, Columns, ' ', COL_KEY_NAME);
    for (uint32_t i = 0; i < 10; i++)
    {
        char Digits[4];
        Number(Digits, i + 1, 2);
        Text(i * Cell, Y, Digits, COL_KEY);
        TextIn(i * Cell + 2, Y, Names[i], Cell - 2, COL_KEY_NAME);
    }
}

void Show()
{
    Con->Draw(Con, 0, 0, Columns, Rows, Cells);
    Con->SetCursor(Con, 0, 0, 0);
}

void ShowWithCursor(uint32_t X, uint32_t Y)
{
    Con->Draw(Con, 0, 0, Columns, Rows, Cells);
    Con->SetCursor(Con, X, Y, 1);
}

// --- dialogs ---------------------------------------------------------------

// A window in the middle of the screen, its size cut to the screen.
static void Window(uint32_t* Width, uint32_t* Height, const char* Title, uint8_t Color,
                   uint32_t* X, uint32_t* Y)
{
    if (Repaint)
        Repaint();
    if (*Width > Columns - 2)
        *Width = Columns - 2;
    if (*Height > Rows - 2)
        *Height = Rows - 2;
    *X = (Columns - *Width) / 2;
    *Y = (Rows - *Height) / 2;
    for (uint32_t y = 0; y < *Height; y++)
        Fill(*X, *Y + y, *Width, ' ', Color);
    Box(*X, *Y, *Width, *Height, Color);

    uint32_t n = (uint32_t)Length(Title);
    if (n + 4 > *Width)
        n = *Width - 4;
    uint32_t At = *X + (*Width - n - 2) / 2;
    Put(At, *Y, ' ', Color);
    TextIn(At + 1, *Y, Title, n, Color);
    Put(At + 1 + n, *Y, ' ', Color);
}

// A line too long for Width shows its end: of a path, the name matters.
static const char* Tail(const char* S, uint32_t Width)
{
    uint64_t n = Length(S);
    return n > Width ? S + (n - Width) : S;
}

int Buttons(const char* Title, const char* Line1, const char* Line2,
            const char* const* Labels, uint32_t Count, bool Error)
{
    uint8_t Color = Error ? COL_ERROR : COL_DIALOG;
    uint32_t Row = 0;                   // the width of the row of buttons
    for (uint32_t i = 0; i < Count; i++)
        Row += (uint32_t)Length(Labels[i]) + 5;
    uint32_t Width = Row;
    if (Length(Line1) > Width)          Width = (uint32_t)Length(Line1);
    if (Line2 && Length(Line2) > Width) Width = (uint32_t)Length(Line2);
    if (Length(Title) > Width)          Width = (uint32_t)Length(Title);
    Width += 6;
    uint32_t Height = Line2 ? 7 : 6;

    uint32_t Chosen = 0;
    for (;;)
    {
        uint32_t X, Y, W = Width, H = Height;
        Window(&W, &H, Title, Color, &X, &Y);
        Text(X + 3, Y + 2, Tail(Line1, W - 6), Color);
        if (Line2)
            Text(X + 3, Y + 3, Tail(Line2, W - 6), Color);
        uint32_t At = X + (W > Row ? (W - Row) / 2 : 1);
        for (uint32_t i = 0; i < Count; i++)
        {
            uint8_t c = i == Chosen ? COL_CHOSEN : Color;
            At = Text(At, Y + H - 2, "[ ", c);
            At = Text(At, Y + H - 2, Labels[i], c);
            At = Text(At, Y + H - 2, " ]", c) + 1;
        }
        Show();

        SfKey K = GetKey();
        if (K.Code == SF_KEY_ESCAPE)
            return -1;
        if (K.Code == SF_KEY_ENTER || K.Code == SF_KEY_KP_ENTER)
            return (int)Chosen;
        if (K.Code == SF_KEY_LEFT)
            Chosen = (Chosen + Count - 1) % Count;
        if (K.Code == SF_KEY_RIGHT || K.Code == SF_KEY_TAB)
            Chosen = (Chosen + 1) % Count;
        for (uint32_t i = 0; i < Count && K.Char; i++)      // the first letter
            if (Lower(K.Char) == Lower(Labels[i][0]))
                return (int)i;
    }
}

void Message(const char* Title, const char* Line, SfStatus Status)
{
    static const char* const Ok[] = { "OK" };
    Buttons(Title, Line, SF_ERROR(Status) ? Why(Status) : nullptr, Ok, 1, SF_ERROR(Status));
}

bool Confirm(const char* Title, const char* Line1, const char* Line2)
{
    static const char* const YesNo[] = { "Yes", "No" };
    return Buttons(Title, Line1, Line2, YesNo, 2) == 0;
}

// One line to edit. What Buffer holds is offered; the first character typed
// replaces it, an arrow keeps it to be changed.
bool Input(const char* Title, const char* Prompt, char* Buffer, uint64_t Size)
{
    uint64_t Len = Length(Buffer), Cur = Len, Left = 0;
    bool Fresh = Len != 0;
    for (;;)
    {
        uint32_t X, Y, W = Columns > 84 ? 80 : Columns - 4, H = 5;
        Window(&W, &H, Title, COL_DIALOG, &X, &Y);
        Text(X + 2, Y + 1, Tail(Prompt, W - 4), COL_DIALOG);
        uint32_t Field = W - 4;
        if (Cur < Left)
            Left = Cur;
        if (Cur >= Left + Field)
            Left = Cur - Field + 1;
        TextIn(X + 2, Y + 2, Buffer + Left, Field, Fresh ? COL_CHOSEN : COL_FIELD);
        ShowWithCursor(X + 2 + (uint32_t)(Cur - Left), Y + 2);

        SfKey K = GetKey();
        if (K.Code == SF_KEY_ESCAPE)
            return false;
        if (K.Code == SF_KEY_ENTER || K.Code == SF_KEY_KP_ENTER)
            return true;
        if (Types(K))
        {
            if (Fresh)
                Len = Cur = 0;
            if (Len + 1 < Size)
            {
                memmove(Buffer + Cur + 1, Buffer + Cur, Len - Cur);
                Buffer[Cur++] = K.Char;
                Len++;
            }
        }
        else if (K.Code == SF_KEY_BACKSPACE && Cur > 0)
        {
            memmove(Buffer + Cur - 1, Buffer + Cur, Len - Cur);
            Cur--, Len--;
        }
        else if (K.Code == SF_KEY_DELETE && Cur < Len)
        {
            memmove(Buffer + Cur, Buffer + Cur + 1, Len - Cur - 1);
            Len--;
        }
        else if (K.Code == SF_KEY_LEFT && Cur > 0)
            Cur--;
        else if (K.Code == SF_KEY_RIGHT && Cur < Len)
            Cur++;
        else if (K.Code == SF_KEY_HOME)
            Cur = 0;
        else if (K.Code == SF_KEY_END)
            Cur = Len;
        Buffer[Len] = '\0';
        Fresh = false;
    }
}

int Menu(const char* Title, const char* const* Items, uint32_t Count, uint32_t Start,
         SfKey* Other)
{
    uint32_t Width = (uint32_t)Length(Title) + 2;
    for (uint32_t i = 0; i < Count; i++)
        if (Length(Items[i]) > Width)
            Width = (uint32_t)Length(Items[i]);
    Width += 4;
    if (Other)
        Other->Code = 0;

    uint32_t Cur = Start < Count ? Start : 0, Top = 0;
    for (;;)
    {
        uint32_t X, Y, W = Width, H = (Count ? Count : 1) + 2;
        Window(&W, &H, Title, COL_DIALOG, &X, &Y);
        uint32_t Shown = H - 2;
        if (Cur < Top)
            Top = Cur;
        if (Cur >= Top + Shown)
            Top = Cur - Shown + 1;
        if (!Count)
            Text(X + 2, Y + 1, "(nothing)", COL_DIALOG);
        for (uint32_t r = 0; r < Shown && Top + r < Count; r++)
        {
            uint8_t c = Top + r == Cur ? COL_CHOSEN : COL_DIALOG;
            Put(X + 1, Y + 1 + r, ' ', c);
            TextIn(X + 2, Y + 1 + r, Tail(Items[Top + r], W - 4), W - 3, c);
        }
        if (Top > 0)
            Put(X + W - 1, Y + 1, SF_ARROW_UP, COL_DIALOG);
        if (Top + Shown < Count)
            Put(X + W - 1, Y + H - 2, SF_ARROW_DOWN, COL_DIALOG);
        Show();

        SfKey K = GetKey();
        uint32_t Last = Count ? Count - 1 : 0;
        switch (K.Code)
        {
            case SF_KEY_ESCAPE:     return -1;
            case SF_KEY_F10:        return -1;
            case SF_KEY_ENTER:
            case SF_KEY_KP_ENTER:   return Count ? (int)Cur : -1;
            case SF_KEY_UP:         if (Cur > 0) Cur--; break;
            case SF_KEY_DOWN:       if (Cur < Last) Cur++; break;
            case SF_KEY_PAGE_UP:    Cur = Cur > Shown ? Cur - Shown : 0; break;
            case SF_KEY_PAGE_DOWN:  Cur = Cur + Shown < Last ? Cur + Shown : Last; break;
            case SF_KEY_HOME:       Cur = 0; break;
            case SF_KEY_END:        Cur = Last; break;
            default:
                if (Other)
                {
                    *Other = K;
                    return (int)Cur;
                }
        }
    }
}

// A job under way. Drawn at most ten times a second, unless Now. Percent
// below 0: no bar.
void Progress(const char* Title, const char* Line, int Percent, bool Now)
{
    static uint64_t Last;
    uint64_t Ms;
    Sys->Time->GetUptime(Sys->Time, &Ms);
    if (!Now && Ms - Last < 100)
        return;
    Last = Ms;

    uint32_t X, Y, W = 64, H = 6;
    Window(&W, &H, Title, COL_DIALOG, &X, &Y);
    Text(X + 2, Y + 1, Tail(Line, W - 4), COL_DIALOG);
    if (Percent >= 0)
    {
        uint32_t Bar = W - 4, Full = (uint32_t)Percent * Bar / 100;
        for (uint32_t i = 0; i < Bar; i++)
            Put(X + 2 + i, Y + 2, i < Full ? SF_BLOCK_FULL : SF_SHADE_LIGHT, COL_DIALOG);
    }
    Text(X + 2, Y + 4, "Esc stops it", COL_DIALOG);
    Show();
}
