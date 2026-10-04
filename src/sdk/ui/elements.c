// sfui: adding and removing elements, their properties and events.

#include "ui.h"

static char Empty[1];

// Puts a copy of Text into E (a field's into its own buffer).
static void Store(SfElement* E, const char* Text)
{
    if (!Text)
        Text = "";
    uint64_t n = strlen(Text);
    if (E->Kind == UI_FIELD)
    {
        if (n + 1 > E->Size)
            n = E->Size - 1;
        memcpy(E->Text, Text, n);
        E->Text[n] = '\0';
        E->Len = E->Cur = n;
        E->Left = 0;
        E->Fresh = n != 0;
        return;
    }
    char* Copy = (char*)UiAlloc(E->Ui, n + 1);
    if (!Copy)
        return;                         // no memory: the old text stays
    memcpy(Copy, Text, n);
    if (E->Text != Empty)
        UiFree(E->Ui, E->Text);
    E->Text = Copy;
}

// A new element at the end of the list, coloured as its layer is.
static SfElement* Add(SfUi* Ui, uint32_t Kind, sint32_t X, sint32_t Y, sint32_t W, sint32_t H)
{
    SfElement* E = (SfElement*)UiAlloc(Ui, sizeof(SfElement));
    if (!E)
        return NULL;
    E->Ui = Ui;
    E->Kind = Kind;
    E->X = X, E->Y = Y, E->W = W, E->H = H;
    E->Visible = true;
    E->Text = Empty;
    E->Lines = SF_LINES_SINGLE;

    SfElement* Window = UiTopWindow(Ui);
    E->Ink   = Window ? Window->Ink : SF_COLOR_WHITE;
    E->Paper = Window ? Window->Paper : SF_COLOR_BLACK;
    E->FocusInk   = SF_COLOR_BRIGHT | SF_COLOR_WHITE;
    E->FocusPaper = SF_COLOR_CYAN;

    E->Prev = Ui->Last;
    if (Ui->Last)
        Ui->Last->Next = E;
    else
        Ui->First = E;
    Ui->Last = E;
    return E;
}

SfElement* SfAddLabel(SfUi* Ui, sint32_t X, sint32_t Y, sint32_t Width, const char* Text)
{
    SfElement* E = Add(Ui, UI_LABEL, X, Y, Width, 1);
    if (E)
        Store(E, Text);
    return E;
}

SfElement* SfAddFrame(SfUi* Ui, sint32_t X, sint32_t Y, sint32_t Width, sint32_t Height,
                      const char* Title)
{
    SfElement* E = Add(Ui, UI_FRAME, X, Y, Width, Height);
    if (!E)
        return NULL;
    Store(E, Title);
    E->FocusInk = E->Ink, E->FocusPaper = E->Paper;
    return E;
}

SfElement* SfAddWindow(SfUi* Ui, const char* Title, sint32_t Width, sint32_t Height)
{
    SfElement* E = Add(Ui, UI_WINDOW, 0, 0, Width, Height);
    if (!E)
        return NULL;
    Store(E, Title);
    E->Ink = E->FocusInk = SF_COLOR_BLACK;
    E->Paper = E->FocusPaper = SF_COLOR_WHITE;
    E->Lines = SF_LINES_DOUBLE;
    E->Align = SF_ALIGN_CENTER;
    E->WasFocus = Ui->Focus;
    Ui->Focus = NULL;
    return E;
}

SfElement* SfAddList(SfUi* Ui, sint32_t X, sint32_t Y, sint32_t Width, sint32_t Height)
{
    SfElement* E = Add(Ui, UI_LIST, X, Y, Width, Height);
    if (E)
        UiOffer(E);
    return E;
}

SfElement* SfAddField(SfUi* Ui, sint32_t X, sint32_t Y, sint32_t Width, uint64_t Size)
{
    SfElement* E = Add(Ui, UI_FIELD, X, Y, Width, 1);
    if (!E)
        return NULL;
    E->Size = Size ? Size : 1;
    E->Text = (char*)UiAlloc(Ui, E->Size);
    if (!E->Text)
    {
        E->Text = Empty;
        SfRemove(E);
        return NULL;
    }
    E->Ink = SF_COLOR_BLACK, E->Paper = SF_COLOR_CYAN;
    UiOffer(E);
    return E;
}

SfElement* SfAddButton(SfUi* Ui, sint32_t X, sint32_t Y, const char* Text)
{
    SfElement* E = Add(Ui, UI_BUTTON, X, Y, 0, 1);
    if (!E)
        return NULL;
    Store(E, Text);
    UiOffer(E);
    return E;
}

SfElement* SfAddCheckBox(SfUi* Ui, sint32_t X, sint32_t Y, const char* Text, bool Checked)
{
    SfElement* E = Add(Ui, UI_CHECKBOX, X, Y, 0, 1);
    if (!E)
        return NULL;
    Store(E, Text);
    E->Checked = Checked;
    UiOffer(E);
    return E;
}

SfElement* SfAddBar(SfUi* Ui, sint32_t X, sint32_t Y, sint32_t Width)
{
    SfElement* E = Add(Ui, UI_BAR, X, Y, Width, 1);
    if (E)
        E->FocusInk = E->Ink, E->FocusPaper = E->Paper;
    return E;
}

SfElement* SfAddKeyBar(SfUi* Ui, const char* const* Names)
{
    SfElement* E = Add(Ui, UI_KEYBAR, 0, 0, 0, 1);
    if (!E)
        return NULL;
    E->Items = Names;
    E->Ink = SF_COLOR_BLACK, E->Paper = SF_COLOR_CYAN;
    E->FocusInk = SF_COLOR_BRIGHT | SF_COLOR_WHITE, E->FocusPaper = SF_COLOR_BLACK;
    return E;
}

SfElement* SfAddCustom(SfUi* Ui, sint32_t X, sint32_t Y, sint32_t Width, sint32_t Height)
{
    return Add(Ui, UI_CUSTOM, X, Y, Width, Height);
}

void SfRemove(SfElement* E)
{
    if (!E)
        return;
    SfUi* Ui = E->Ui;
    SfElement* Was = NULL;              // the focus before the first window going
    bool FocusGoes = false;
    for (SfElement* P = E; P; P = P->Next)
    {
        if (P->Kind == UI_WINDOW && !Was)
            Was = P->WasFocus ? P->WasFocus : P;
        FocusGoes |= P == Ui->Focus;
    }

    Ui->Last = E->Prev;
    if (E->Prev)
        E->Prev->Next = NULL;
    else
        Ui->First = NULL;
    while (E)
    {
        SfElement* Next = E->Next;
        if (E->Text != Empty)
            UiFree(Ui, E->Text);
        UiFree(Ui, E);
        E = Next;
    }

    // Was may have gone too, and a window had no focus before it.
    bool Kept = false;
    for (SfElement* P = Ui->First; P && Was && !Kept; P = P->Next)
        Kept = P == Was && Was->Kind != UI_WINDOW;
    if (Kept)
        Ui->Focus = Was;
    else if (Was || FocusGoes)
    {
        Ui->Focus = NULL;
        SfElement* Top = UiTopWindow(Ui);
        for (SfElement* P = Top ? Top->Next : Ui->First; P && !Ui->Focus; P = P->Next)
            if (UiTakesFocus(P))
                Ui->Focus = P;
    }
}

// --- properties ----------------------------------------------------------------

void SfSetText(SfElement* E, const char* Text)
{
    if (!E)
        return;
    Store(E, Text);
}

const char* SfGetText(SfElement* E)
{
    if (!E)
        return "";
    return E->Text;
}

void SfSetColors(SfElement* E, uint8_t Ink, uint8_t Paper)
{
    if (!E)
        return;
    bool Same = E->Kind == UI_FRAME && E->FocusInk == E->Ink && E->FocusPaper == E->Paper;
    E->Ink = Ink, E->Paper = Paper;
    if (Same)                           // a frame's title goes with it
        E->FocusInk = Ink, E->FocusPaper = Paper;
}

void SfSetFocusColors(SfElement* E, uint8_t Ink, uint8_t Paper)
{
    if (!E)
        return;
    E->FocusInk = Ink, E->FocusPaper = Paper;
}

void SfSetAlign(SfElement* E, uint32_t Align)
{
    if (!E)
        return;
    E->Align = Align;
}

void SfSetLines(SfElement* E, uint32_t Lines)
{
    if (!E)
        return;
    E->Lines = Lines;
}

void SfSetVisible(SfElement* E, bool Visible)
{
    if (!E)
        return;
    E->Visible = Visible;
    SfUi* Ui = E->Ui;
    if (!Visible && Ui->Focus == E)
    {
        UiMoveFocus(Ui, 1, 0);
        if (Ui->Focus == E)
            Ui->Focus = NULL;
    }
    if (Visible)
        UiOffer(E);
}

void SfSetPlace(SfElement* E, sint32_t X, sint32_t Y, sint32_t Width, sint32_t Height)
{
    if (!E)
        return;
    E->X = X, E->Y = Y, E->W = Width, E->H = Height;
}

void SfSetTag(SfElement* E, void* Tag)
{
    if (!E)
        return;
    E->Tag = Tag;
}

void* SfGetTag(SfElement* E)
{
    if (!E)
        return NULL;
    return E->Tag;
}

void SfSetResult(SfElement* E, int Result)
{
    if (!E)
        return;
    E->Result = Result;
}

void SfSetCount(SfElement* L, uint32_t Count)
{
    if (!L)
        return;
    L->Count = Count;
    UiScroll(L, 0);
}

void SfSetItems(SfElement* L, const char* const* Items, uint32_t Count)
{
    if (!L)
        return;
    L->Items = Items;
    SfSetCount(L, Count);
}

void SfSetCursor(SfElement* L, uint32_t Index)
{
    if (!L)
        return;
    L->Cursor = Index;
    UiScroll(L, 0);
}

uint32_t SfGetCursor(SfElement* L)
{
    if (!L)
        return 0;
    return L->Cursor;
}

void SfSetChecked(SfElement* C, bool Checked)
{
    if (!C)
        return;
    C->Checked = Checked;
}

bool SfGetChecked(SfElement* C)
{
    if (!C)
        return 0;
    return C->Checked;
}

void SfSetValue(SfElement* Bar, uint32_t Percent)
{
    if (!Bar)
        return;
    Bar->Value = Percent > 100 ? 100 : Percent;
}

// --- events --------------------------------------------------------------------

void SfOnKey(SfElement* E, SfKeyHandler Handler)
{
    if (!E)
        return;
    E->OnKey = Handler;
    UiOffer(E);
}

void SfOnChoose(SfElement* E, SfChooseHandler Handler)
{
    if (!E)
        return;
    E->OnChoose = Handler;
}

void SfOnPaint(SfElement* E, SfPaintHandler Handler)
{
    if (!E)
        return;
    E->OnPaint = Handler;
}

void SfOnDrawItem(SfElement* L, SfDrawItemHandler Handler)
{
    if (!L)
        return;
    L->OnDrawItem = Handler;
}
