// sfui: the screen, its elements, drawing them and handing out the keys.
//
// The elements are one list, in the order they were added. Each drawing
// starts from blank cells and draws them all, the later over the earlier,
// then shows the cells with one Draw - so nothing is left over from before.
// A window starts a layer; where an element lies, and whether it takes keys,
// comes from the last window before it.

#include "ui.h"

// --- memory ------------------------------------------------------------------

void* UiAlloc(SfUi* Ui, uint64_t Size)
{
    void* Buffer = NULL;
    if (SF_ERROR(Ui->Sys->Memory->Allocate(Ui->Sys->Memory, Size, &Buffer)))
        return NULL;
    memset(Buffer, 0, Size);
    return Buffer;
}

void UiFree(SfUi* Ui, void* Buffer)
{
    if (Buffer)
        Ui->Sys->Memory->Free(Ui->Sys->Memory, Buffer);
}

uint64_t UiNow(SfUi* Ui)
{
    uint64_t Ms = 0;
    Ui->Sys->Time->GetUptime(Ui->Sys->Time, &Ms);
    return Ms;
}

// --- the screen --------------------------------------------------------------

SfUi* SfUiOpen(SfSystem* Sys)
{
    if (!SF_HAS_FIELD(Sys->Sync, SfSync, WaitAny))
        return NULL;
    void* Buffer = NULL;
    if (SF_ERROR(Sys->Memory->Allocate(Sys->Memory, sizeof(SfUi), &Buffer)))
        return NULL;
    SfUi* Ui = (SfUi*)Buffer;
    memset(Ui, 0, sizeof(SfUi));
    Ui->Sys = Sys;
    Ui->Con = Sys->Console;
    Ui->Con->GetSize(Ui->Con, &Ui->Columns, &Ui->Rows);
    Ui->Cells = (SfCell*)UiAlloc(Ui, (uint64_t)Ui->Columns * Ui->Rows * sizeof(SfCell));
    if (!Ui->Cells)
    {
        UiFree(Ui, Ui);
        return NULL;
    }
    Ui->Con->SetMode(Ui->Con, SF_CONSOLE_RAW);
    return Ui;
}

void SfUiClose(SfUi* Ui)
{
    if (Ui->First)
        SfRemove(Ui->First);
    Ui->Con->SetMode(Ui->Con, SF_CONSOLE_LINE);
    UiFree(Ui, Ui->Cells);
    UiFree(Ui, Ui);
}

void SfUiSize(SfUi* Ui, uint32_t* Columns, uint32_t* Rows)
{
    *Columns = Ui->Columns;
    *Rows = Ui->Rows;
}

void SfUiOnKey(SfUi* Ui, SfKeyHandler Handler)
{
    Ui->OnKey = Handler;
}

void SfUiOnTimer(SfUi* Ui, uint64_t Ms, SfTimerHandler Handler)
{
    Ui->TimerMs = Handler ? Ms : 0;
    Ui->OnTimer = Handler;
    Ui->NextTick = UiNow(Ui) + Ms;
}

// --- drawing primitives --------------------------------------------------------

void SfPut(SfUi* Ui, uint32_t X, uint32_t Y, char C, uint8_t Color)
{
    if (X < Ui->Columns && Y < Ui->Rows)
    {
        Ui->Cells[Y * Ui->Columns + X].Char  = C;
        Ui->Cells[Y * Ui->Columns + X].Color = Color;
    }
}

uint32_t SfText(SfUi* Ui, uint32_t X, uint32_t Y, const char* Text, uint8_t Color)
{
    for (; *Text; Text++, X++)
        SfPut(Ui, X, Y, *Text, Color);
    return X;
}

void SfTextIn(SfUi* Ui, uint32_t X, uint32_t Y, const char* Text, uint32_t Width,
              uint8_t Color)
{
    for (uint32_t i = 0; i < Width; i++)
    {
        SfPut(Ui, X + i, Y, *Text ? *Text : ' ', Color);
        if (*Text)
            Text++;
    }
}

void SfFill(SfUi* Ui, uint32_t X, uint32_t Y, uint32_t Width, char C, uint8_t Color)
{
    for (uint32_t i = 0; i < Width; i++)
        SfPut(Ui, X + i, Y, C, Color);
}

void SfCaret(SfUi* Ui, uint32_t X, uint32_t Y)
{
    Ui->CaretOn = true;
    Ui->CaretX = X;
    Ui->CaretY = Y;
}

// --- layers and places -------------------------------------------------------

// The window whose layer E is in; null for the bottom one.
SfElement* UiLayerOf(SfElement* E)
{
    for (SfElement* P = E->Prev; P; P = P->Prev)
        if (P->Kind == UI_WINDOW)
            return P;
    return NULL;
}

// The window of the top layer; null when there is none.
SfElement* UiTopWindow(SfUi* Ui)
{
    for (SfElement* E = Ui->Last; E; E = E->Prev)
        if (E->Kind == UI_WINDOW)
            return E;
    return NULL;
}

// A coordinate in an area Size cells long: negative ones from its end.
static uint32_t Coordinate(sint32_t V, uint32_t Size)
{
    if (V >= 0)
        return (uint32_t)V < Size ? (uint32_t)V : Size;
    return (uint32_t)-V < Size ? Size - (uint32_t)-V : 0;
}

// A length from At in an area Size cells long: 0 and negative ones to its
// end; never past it.
static uint32_t Extent(sint32_t V, uint32_t At, uint32_t Size)
{
    uint32_t Room = Size - At;
    if (V > 0)
        return (uint32_t)V < Room ? (uint32_t)V : Room;
    return (uint32_t)-V < Room ? Room - (uint32_t)-V : 0;
}

// A window's place on the screen: in the middle, at most two cells less
// than the screen unless it asks for the whole of it.
static void WindowPlace(SfElement* W, UiRect* R)
{
    SfUi* Ui = W->Ui;
    R->W = W->W > 0 ? ((uint32_t)W->W + 2 <= Ui->Columns ? (uint32_t)W->W : Ui->Columns - 2)
                    : Extent(W->W, 0, Ui->Columns);
    R->H = W->H > 0 ? ((uint32_t)W->H + 2 <= Ui->Rows ? (uint32_t)W->H : Ui->Rows - 2)
                    : Extent(W->H, 0, Ui->Rows);
    R->X = (Ui->Columns - R->W) / 2;
    R->Y = (Ui->Rows - R->H) / 2;
}

void UiPlace(SfElement* E, UiRect* R)
{
    SfUi* Ui = E->Ui;
    if (E->Kind == UI_WINDOW)
        return WindowPlace(E, R);
    if (E->Kind == UI_KEYBAR)
    {
        R->X = 0, R->Y = Ui->Rows - 1, R->W = Ui->Columns, R->H = 1;
        return;
    }

    UiRect Area = { 0, 0, Ui->Columns, Ui->Rows };
    SfElement* W = UiLayerOf(E);
    if (W)
    {
        WindowPlace(W, &Area);
        if (W->Lines != SF_LINES_NONE && Area.W >= 2 && Area.H >= 2)
            Area.X++, Area.Y++, Area.W -= 2, Area.H -= 2;
    }
    uint32_t X = Coordinate(E->X, Area.W), Y = Coordinate(E->Y, Area.H);
    R->X = Area.X + X;
    R->Y = Area.Y + Y;
    R->W = Extent(E->W, X, Area.W);
    R->H = E->Kind >= UI_LABEL ? (Y < Area.H ? 1 : 0) : Extent(E->H, Y, Area.H);
    if (E->Kind == UI_BUTTON || E->Kind == UI_CHECKBOX)
    {
        uint32_t Wanted = (uint32_t)strlen(E->Text) + 4;
        R->W = Wanted < Area.W - X ? Wanted : Area.W - X;
    }
}

// --- focus ---------------------------------------------------------------------

bool UiTakesFocus(SfElement* E)
{
    if (!E->Visible)
        return false;
    switch (E->Kind)
    {
        case UI_LIST:
        case UI_FIELD:
        case UI_BUTTON:
        case UI_CHECKBOX:   return true;
        case UI_CUSTOM:     return E->OnKey != NULL;
        default:            return false;
    }
}

// An element just added, or just given a key handler: the focus, when
// nothing of its layer has it yet.
void UiOffer(SfElement* E)
{
    SfUi* Ui = E->Ui;
    if (!UiTakesFocus(E) || UiLayerOf(E) != UiTopWindow(Ui))
        return;
    if (!Ui->Focus || UiLayerOf(Ui->Focus) != UiLayerOf(E))
        Ui->Focus = E;
}

void SfFocus(SfElement* E)
{
    if (E && UiTakesFocus(E))
        E->Ui->Focus = E;
}

SfElement* SfFocused(SfUi* Ui)
{
    return Ui->Focus;
}

// The next (Step 1) or the one before (Step -1) of the top layer that takes
// the focus; Kind 0 for any kind.
void UiMoveFocus(SfUi* Ui, int Step, uint32_t Kind)
{
    SfElement* Top = UiTopWindow(Ui);
    SfElement* From = Ui->Focus;
    if (!From)
        From = Step > 0 ? Ui->Last : Ui->First;
    SfElement* E = From;
    for (;;)
    {
        E = Step > 0 ? (E->Next ? E->Next : Ui->First) : (E->Prev ? E->Prev : Ui->Last);
        if (UiTakesFocus(E) && UiLayerOf(E) == Top && (!Kind || E->Kind == Kind))
        {
            Ui->Focus = E;
            return;
        }
        if (E == From)
            return;
    }
}

// --- drawing -------------------------------------------------------------------

static uint8_t ColorOf(SfElement* E, bool Focused)
{
    return Focused ? SF_CELL_COLOR(E->FocusInk, E->FocusPaper) : SF_CELL_COLOR(E->Ink, E->Paper);
}

// Text in Width cells as E's alignment puts it.
static void Aligned(SfUi* Ui, SfElement* E, uint32_t X, uint32_t Y, uint32_t Width,
                    const char* Text, uint8_t Color)
{
    uint32_t n = (uint32_t)strlen(Text);
    if (n > Width)
    {
        if (E->Align & SF_ALIGN_TAIL)
            Text += n - Width;
        n = Width;
    }
    uint32_t Pad = 0;
    switch (E->Align & 3)
    {
        case SF_ALIGN_CENTER:   Pad = (Width - n) / 2; break;
        case SF_ALIGN_RIGHT:    Pad = Width - n; break;
    }
    SfFill(Ui, X, Y, Pad, ' ', Color);
    SfTextIn(Ui, X + Pad, Y, Text, Width - Pad, Color);
}

// The shadow of a window: two cells at its right, one row under it, the
// cells there darkened as they are.
static void Shadow(SfUi* Ui, UiRect* R)
{
    uint8_t Dark = SF_CELL_COLOR(SF_COLOR_BRIGHT | SF_COLOR_BLACK, SF_COLOR_BLACK);
    for (uint32_t y = R->Y + 1; y <= R->Y + R->H; y++)
        for (uint32_t x = (y == R->Y + R->H ? R->X + 2 : R->X + R->W); x < R->X + R->W + 2; x++)
            if (x < Ui->Columns && y < Ui->Rows)
                Ui->Cells[y * Ui->Columns + x].Color = Dark;
}

static void DrawFrame(SfUi* Ui, SfElement* E, UiRect* R)
{
    uint8_t Color = ColorOf(E, false);
    for (uint32_t y = 0; y < R->H; y++)
        SfFill(Ui, R->X, R->Y + y, R->W, ' ', Color);
    if (E->Lines == SF_LINES_NONE || R->W < 2 || R->H < 2)
        return;

    bool Two = E->Lines == SF_LINES_DOUBLE;
    uint32_t Right = R->X + R->W - 1, Bottom = R->Y + R->H - 1;
    SfFill(Ui, R->X + 1, R->Y, R->W - 2, Two ? SF_BOX2_H : SF_BOX_H, Color);
    SfFill(Ui, R->X + 1, Bottom, R->W - 2, Two ? SF_BOX2_H : SF_BOX_H, Color);
    for (uint32_t y = R->Y + 1; y < Bottom; y++)
    {
        SfPut(Ui, R->X, y, Two ? SF_BOX2_V : SF_BOX_V, Color);
        SfPut(Ui, Right, y, Two ? SF_BOX2_V : SF_BOX_V, Color);
    }
    SfPut(Ui, R->X, R->Y, Two ? SF_BOX2_TOP_LEFT : SF_BOX_TOP_LEFT, Color);
    SfPut(Ui, Right, R->Y, Two ? SF_BOX2_TOP_RIGHT : SF_BOX_TOP_RIGHT, Color);
    SfPut(Ui, R->X, Bottom, Two ? SF_BOX2_BOTTOM_LEFT : SF_BOX_BOTTOM_LEFT, Color);
    SfPut(Ui, Right, Bottom, Two ? SF_BOX2_BOTTOM_RIGHT : SF_BOX_BOTTOM_RIGHT, Color);

    // The title, a space at each side of it, in the top line.
    if (!E->Text[0] || R->W < 6)
        return;
    uint32_t Room = R->W - 6, n = (uint32_t)strlen(E->Text);
    const char* Title = E->Text;
    if (n > Room)
    {
        if (E->Align & SF_ALIGN_TAIL)
            Title += n - Room;
        n = Room;
    }
    uint32_t At = R->X + 2;
    if ((E->Align & 3) == SF_ALIGN_CENTER)
        At = R->X + (R->W - n - 2) / 2;
    else if ((E->Align & 3) == SF_ALIGN_RIGHT)
        At = Right - n - 3;
    uint8_t TitleColor = ColorOf(E, true);
    SfPut(Ui, At, R->Y, ' ', TitleColor);
    SfTextIn(Ui, At + 1, R->Y, Title, n, TitleColor);
    SfPut(Ui, At + 1 + n, R->Y, ' ', TitleColor);
}

// The cursor in sight: Top moves as little as it has to.
void UiScroll(SfElement* L, uint32_t Shown)
{
    if (L->Cursor >= L->Count)
        L->Cursor = L->Count ? L->Count - 1 : 0;
    if (!Shown)
        return;
    if (L->Cursor < L->Top)
        L->Top = L->Cursor;
    if (L->Cursor >= L->Top + Shown)
        L->Top = L->Cursor - Shown + 1;
    if (L->Top + Shown > L->Count)
        L->Top = L->Count > Shown ? L->Count - Shown : 0;
}

static void DrawList(SfUi* Ui, SfElement* E, UiRect* R)
{
    bool Mine = Ui->Focus == E;
    uint8_t Color = ColorOf(E, false);
    for (uint32_t y = 0; y < R->H; y++)
        SfFill(Ui, R->X, R->Y + y, R->W, ' ', Color);
    UiScroll(E, R->H);
    for (uint32_t r = 0; r < R->H && E->Top + r < E->Count; r++)
    {
        uint32_t i = E->Top + r;
        bool Cursor = Mine && i == E->Cursor;
        if (E->OnDrawItem)
            E->OnDrawItem(Ui, E, i, R->X, R->Y + r, R->W, Cursor);
        else if (E->Items)
        {
            uint8_t c = ColorOf(E, Cursor);
            SfPut(Ui, R->X, R->Y + r, ' ', c);
            if (R->W > 1)
                Aligned(Ui, E, R->X + 1, R->Y + r, R->W - 1, E->Items[i], c);
        }
    }
    if (E->Top > 0)
        SfPut(Ui, R->X + R->W, R->Y, SF_ARROW_UP, Color);
    if (E->Top + R->H < E->Count)
        SfPut(Ui, R->X + R->W, R->Y + R->H - 1, SF_ARROW_DOWN, Color);
}

static void DrawField(SfUi* Ui, SfElement* E, UiRect* R)
{
    if (!R->W)
        return;
    if (E->Cur < E->Left)
        E->Left = E->Cur;
    if (E->Cur >= E->Left + R->W)
        E->Left = E->Cur - R->W + 1;
    bool Mine = Ui->Focus == E;
    SfTextIn(Ui, R->X, R->Y, E->Text + E->Left, R->W, ColorOf(E, Mine && E->Fresh));
    if (Mine)
        SfCaret(Ui, R->X + (uint32_t)(E->Cur - E->Left), R->Y);
}

static void DrawBar(SfUi* Ui, SfElement* E, UiRect* R)
{
    uint32_t Full = (E->Value * R->W + 50) / 100;
    for (uint32_t i = 0; i < R->W; i++)
        SfPut(Ui, R->X + i, R->Y, i < Full ? SF_BLOCK_FULL : SF_SHADE_LIGHT, ColorOf(E, i >= Full));
}

static void DrawKeyBar(SfUi* Ui, SfElement* E, UiRect* R)
{
    uint32_t Cell = R->W / 10;
    SfFill(Ui, R->X, R->Y, R->W, ' ', ColorOf(E, false));
    for (uint32_t i = 0; i < 10 && Cell > 2 && E->Items; i++)
    {
        char Digits[3] = { i == 9 ? '1' : ' ', i == 9 ? '0' : (char)('1' + i), 0 };
        SfText(Ui, R->X + i * Cell, R->Y, Digits, ColorOf(E, true));
        SfTextIn(Ui, R->X + i * Cell + 2, R->Y, E->Items[i], Cell - 2, ColorOf(E, false));
    }
}

static void DrawOne(SfUi* Ui, SfElement* E)
{
    UiRect R;
    UiPlace(E, &R);
    if (!R.W || !R.H)
        return;
    bool Mine = Ui->Focus == E;
    char Line[8];
    switch (E->Kind)
    {
        case UI_WINDOW:
            Shadow(Ui, &R);
            DrawFrame(Ui, E, &R);
            break;
        case UI_FRAME:
            DrawFrame(Ui, E, &R);
            break;
        case UI_LABEL:
            Aligned(Ui, E, R.X, R.Y, R.W, E->Text, ColorOf(E, false));
            break;
        case UI_LIST:
            DrawList(Ui, E, &R);
            break;
        case UI_FIELD:
            DrawField(Ui, E, &R);
            break;
        case UI_BUTTON:
        {
            uint8_t c = ColorOf(E, Mine);
            uint32_t X = SfText(Ui, R.X, R.Y, "[ ", c);
            X = SfText(Ui, X, R.Y, E->Text, c);
            SfText(Ui, X, R.Y, " ]", c);
            break;
        }
        case UI_CHECKBOX:
        {
            uint8_t c = ColorOf(E, Mine);
            Line[0] = '[', Line[1] = E->Checked ? 'x' : ' ', Line[2] = ']', Line[3] = ' ';
            Line[4] = 0;
            SfText(Ui, SfText(Ui, R.X, R.Y, Line, c), R.Y, E->Text, c);
            break;
        }
        case UI_BAR:
            DrawBar(Ui, E, &R);
            break;
        case UI_KEYBAR:
            DrawKeyBar(Ui, E, &R);
            break;
        case UI_CUSTOM:
            if (E->OnPaint)
                E->OnPaint(Ui, E, R.X, R.Y, R.W, R.H);
            break;
    }
}

void SfUiShow(SfUi* Ui)
{
    SfCell Blank = { ' ', SF_CELL_COLOR(SF_COLOR_WHITE, SF_COLOR_BLACK) };
    for (uint64_t i = 0; i < (uint64_t)Ui->Columns * Ui->Rows; i++)
        Ui->Cells[i] = Blank;
    Ui->CaretOn = false;
    for (SfElement* E = Ui->First; E; E = E->Next)
        if (E->Visible)
            DrawOne(Ui, E);
    Ui->Con->Draw(Ui->Con, 0, 0, Ui->Columns, Ui->Rows, Ui->Cells);
    Ui->Con->SetCursor(Ui->Con, Ui->CaretOn ? Ui->CaretX : 0, Ui->CaretOn ? Ui->CaretY : 0,
                       Ui->CaretOn);
}

// --- keys ----------------------------------------------------------------------

static char Lower(char C)
{
    return C >= 'A' && C <= 'Z' ? (char)(C - 'A' + 'a') : C;
}

static bool Typed(SfKey K)
{
    return (uint8_t)K.Char >= 32 && K.Char != 127 && !(K.Mods & (SF_MOD_CTRL | SF_MOD_ALT));
}

static bool IsEnter(SfKey K)
{
    return K.Code == SF_KEY_ENTER || K.Code == SF_KEY_KP_ENTER;
}

void UiChoose(SfElement* E)
{
    SfUi* Ui = E->Ui;
    int Result = E->Result;
    if (E->OnChoose)
        E->OnChoose(Ui, E);
    if (Result)
        SfUiEnd(Ui, Result);
}

static bool ListKey(SfElement* E, SfKey K)
{
    UiRect R;
    UiPlace(E, &R);
    uint32_t Page = R.H > 1 ? R.H - 1 : 1;
    uint32_t Last = E->Count ? E->Count - 1 : 0;
    switch (K.Code)
    {
        case SF_KEY_UP:         if (E->Cursor > 0) E->Cursor--; return true;
        case SF_KEY_DOWN:       if (E->Cursor < Last) E->Cursor++; return true;
        case SF_KEY_PAGE_UP:    E->Cursor = E->Cursor > Page ? E->Cursor - Page : 0; return true;
        case SF_KEY_PAGE_DOWN:
            E->Cursor = E->Cursor + Page < Last ? E->Cursor + Page : Last;
            return true;
        case SF_KEY_HOME:       E->Cursor = 0; return true;
        case SF_KEY_END:        E->Cursor = Last; return true;
    }
    if (IsEnter(K))
    {
        UiChoose(E);
        return true;
    }
    return false;
}

static bool FieldKey(SfElement* E, SfKey K)
{
    if (IsEnter(K))
    {
        UiChoose(E);
        return true;
    }
    bool Used = true;
    if (Typed(K))
    {
        if (E->Fresh)
            E->Len = E->Cur = 0;
        if (E->Len + 1 < E->Size)
        {
            memmove(E->Text + E->Cur + 1, E->Text + E->Cur, E->Len - E->Cur);
            E->Text[E->Cur++] = K.Char;
            E->Len++;
        }
    }
    else if (K.Code == SF_KEY_BACKSPACE && E->Cur > 0)
    {
        memmove(E->Text + E->Cur - 1, E->Text + E->Cur, E->Len - E->Cur);
        E->Cur--, E->Len--;
    }
    else if (K.Code == SF_KEY_DELETE && E->Cur < E->Len)
    {
        memmove(E->Text + E->Cur, E->Text + E->Cur + 1, E->Len - E->Cur - 1);
        E->Len--;
    }
    else if (K.Code == SF_KEY_LEFT && E->Cur > 0)
        E->Cur--;
    else if (K.Code == SF_KEY_RIGHT && E->Cur < E->Len)
        E->Cur++;
    else if (K.Code == SF_KEY_HOME)
        E->Cur = 0;
    else if (K.Code == SF_KEY_END)
        E->Cur = E->Len;
    else if (K.Code != SF_KEY_BACKSPACE && K.Code != SF_KEY_DELETE &&
             K.Code != SF_KEY_LEFT && K.Code != SF_KEY_RIGHT)
        Used = false;
    E->Text[E->Len] = '\0';
    if (Used)
        E->Fresh = false;
    return Used;
}

// What the element does with a key itself.
static bool OwnKey(SfElement* E, SfKey K)
{
    switch (E->Kind)
    {
        case UI_LIST:
            return ListKey(E, K);
        case UI_FIELD:
            return FieldKey(E, K);
        case UI_BUTTON:
            if (IsEnter(K) || K.Code == SF_KEY_SPACE)
                UiChoose(E);
            else if (K.Code == SF_KEY_LEFT || K.Code == SF_KEY_RIGHT)
                UiMoveFocus(E->Ui, K.Code == SF_KEY_LEFT ? -1 : 1, UI_BUTTON);
            else
                return false;
            return true;
        case UI_CHECKBOX:
            if (K.Code != SF_KEY_SPACE)
                return false;
            E->Checked = !E->Checked;
            UiChoose(E);
            return true;
    }
    return false;
}

static void HandKey(SfUi* Ui, SfKey K)
{
    SfElement* F = Ui->Focus;
    if (F && F->OnKey && F->OnKey(Ui, F, K))
        return;
    if (F && OwnKey(F, K))
        return;
    if (K.Code == SF_KEY_TAB && !(K.Mods & (SF_MOD_CTRL | SF_MOD_ALT)))
    {
        UiMoveFocus(Ui, (K.Mods & SF_MOD_SHIFT) ? -1 : 1, 0);
        return;
    }

    SfElement* Top = UiTopWindow(Ui);
    if (!Top)
    {
        if (Ui->OnKey)
            Ui->OnKey(Ui, NULL, K);
        return;
    }
    if (K.Code == SF_KEY_ESCAPE)
    {
        SfUiEnd(Ui, 0);
        return;
    }
    if (Typed(K))                       // the button that starts with it
        for (SfElement* E = Top->Next; E; E = E->Next)
            if (E->Kind == UI_BUTTON && E->Visible && Lower(E->Text[0]) == Lower(K.Char))
            {
                Ui->Focus = E;
                UiChoose(E);
                return;
            }
}

// A key that is there for the program, without waiting longer than
// TimeoutMs; false when there is none.
static bool NextKey(SfUi* Ui, SfKey* Key, uint64_t TimeoutMs)
{
    SfWaitItem Item = { SF_WAIT_KEY, 0, NULL };
    if (Ui->Sys->Sync->WaitAny(Ui->Sys->Sync, 1, &Item, TimeoutMs, NULL) != SF_SUCCESS)
        return false;
    if (Ui->Con->ReadKey(Ui->Con, Key) == SF_SUCCESS)
        return true;
    Ui->Sys->Time->Sleep(Ui->Sys->Time, 100);    // a program it started has the keys
    return false;
}

int SfUiRun(SfUi* Ui)
{
    for (;;)
    {
        SfUiShow(Ui);
        uint64_t Wait = SF_WAIT_FOREVER;
        if (Ui->TimerMs)
        {
            uint64_t Now = UiNow(Ui);
            Wait = Ui->NextTick > Now ? Ui->NextTick - Now : 0;
        }
        // The key, and every one that came meanwhile, before drawing again.
        SfKey K;
        for (; !Ui->Ending && NextKey(Ui, &K, Wait); Wait = 0)
            HandKey(Ui, K);
        if (Ui->TimerMs && !Ui->Ending && UiNow(Ui) >= Ui->NextTick)
        {
            Ui->NextTick = UiNow(Ui) + Ui->TimerMs;
            Ui->OnTimer(Ui);
        }
        if (Ui->Ending)
        {
            Ui->Ending = false;
            return Ui->Result;
        }
    }
}

void SfUiEnd(SfUi* Ui, int Result)
{
    Ui->Ending = true;
    Ui->Result = Result;
}

bool SfUiEscape(SfUi* Ui)
{
    SfKey K;
    while (NextKey(Ui, &K, 0))
        if (K.Code == SF_KEY_ESCAPE)
            return true;
    return false;
}
