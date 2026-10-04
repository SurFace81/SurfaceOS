// sfui: dialogs - windows of elements that wait for their answer.

#include "ui.h"

static uint32_t Len(const char* S)
{
    return S ? (uint32_t)strlen(S) : 0;
}

int SfButtons(SfUi* Ui, const char* Title, const char* Line1, const char* Line2,
              const char* const* Labels, uint32_t Count, bool Error)
{
    uint32_t Row = 0;                   // the row of buttons, a space between them
    for (uint32_t i = 0; i < Count; i++)
        Row += Len(Labels[i]) + 4 + (i ? 1 : 0);
    uint32_t Width = Row;
    if (Len(Line1) > Width) Width = Len(Line1);
    if (Len(Line2) > Width) Width = Len(Line2);
    if (Len(Title) > Width) Width = Len(Title);
    Width += 6;

    SfElement* W = SfAddWindow(Ui, Title, (sint32_t)Width, Line2 ? 7 : 6);
    if (!W)
        return -1;
    if (Error)
        SfSetColors(W, SF_COLOR_BRIGHT | SF_COLOR_WHITE, SF_COLOR_RED);
    SfSetAlign(SfAddLabel(Ui, 2, 1, -2, Line1), SF_ALIGN_TAIL);
    if (Line2)
        SfSetAlign(SfAddLabel(Ui, 2, 2, -2, Line2), SF_ALIGN_TAIL);

    UiRect R;
    UiPlace(W, &R);
    uint32_t Inner = R.W - 2;
    sint32_t X = Inner > Row ? (sint32_t)((Inner - Row) / 2) : 0;
    for (uint32_t i = 0; i < Count; i++)
    {
        SfElement* B = SfAddButton(Ui, X, -1, Labels[i]);
        if (B)
            SfSetResult(B, (int)i + 1);
        X += (sint32_t)Len(Labels[i]) + 5;
    }
    int Result = SfUiRun(Ui);
    SfRemove(W);
    return Result - 1;
}

void SfMessage(SfUi* Ui, const char* Title, const char* Line1, const char* Line2, bool Error)
{
    static const char* const Ok[] = { "OK" };
    SfButtons(Ui, Title, Line1, Line2, Ok, 1, Error);
}

bool SfConfirm(SfUi* Ui, const char* Title, const char* Line1, const char* Line2)
{
    static const char* const YesNo[] = { "Yes", "No" };
    return SfButtons(Ui, Title, Line1, Line2, YesNo, 2, false) == 0;
}

bool SfInput(SfUi* Ui, const char* Title, const char* Prompt, char* Buffer, uint64_t Size)
{
    SfElement* W = SfAddWindow(Ui, Title, Ui->Columns > 84 ? 80 : (sint32_t)Ui->Columns - 4, 5);
    if (!W)
        return false;
    SfSetAlign(SfAddLabel(Ui, 1, 0, -1, Prompt), SF_ALIGN_TAIL);
    SfElement* F = SfAddField(Ui, 1, 1, -1, Size);
    bool Done = false;
    if (F)
    {
        SfSetText(F, Buffer);
        SfSetResult(F, 1);
        Done = SfUiRun(Ui) == 1;
        if (Done)
            memcpy(Buffer, F->Text, F->Len + 1);
    }
    SfRemove(W);
    return Done;
}

// A menu's list: Enter answers with the item, F10 is Esc, and with Other
// (its tag) any key the list has no use for answers too.
static void MenuChoose(SfUi* Ui, SfElement* List)
{
    SfUiEnd(Ui, List->Count ? (int)List->Cursor + 1 : 0);
}

static bool MenuKey(SfUi* Ui, SfElement* List, SfKey K)
{
    SfKey* Other = (SfKey*)List->Tag;
    switch (K.Code)
    {
        case SF_KEY_F10:
            SfUiEnd(Ui, 0);
            return true;
        case SF_KEY_UP: case SF_KEY_DOWN: case SF_KEY_PAGE_UP: case SF_KEY_PAGE_DOWN:
        case SF_KEY_HOME: case SF_KEY_END: case SF_KEY_ENTER: case SF_KEY_KP_ENTER:
        case SF_KEY_ESCAPE:
            return false;
    }
    if (!Other)
        return false;
    *Other = K;
    SfUiEnd(Ui, (int)List->Cursor + 1);
    return true;
}

int SfMenu(SfUi* Ui, const char* Title, const char* const* Items, uint32_t Count,
           uint32_t Start, SfKey* Other)
{
    uint32_t Width = Len(Title) + 2;
    for (uint32_t i = 0; i < Count; i++)
        if (Len(Items[i]) > Width)
            Width = Len(Items[i]);
    if (Other)
        Other->Code = 0;

    SfElement* W = SfAddWindow(Ui, Title, (sint32_t)Width + 4, (sint32_t)(Count ? Count : 1) + 2);
    if (!W)
        return -1;
    SfElement* L = SfAddList(Ui, 0, 0, 0, 0);
    if (!Count)
        SfAddLabel(Ui, 1, 0, 0, "(nothing)");
    int Result = 0;
    if (L)
    {
        SfSetAlign(L, SF_ALIGN_TAIL);
        SfSetItems(L, Items, Count);
        SfSetCursor(L, Start < Count ? Start : 0);
        SfSetTag(L, Other);
        SfOnChoose(L, MenuChoose);
        SfOnKey(L, MenuKey);
        Result = SfUiRun(Ui);
    }
    SfRemove(W);
    return Result - 1;
}

SfElement* SfProgress(SfUi* Ui, const char* Title)
{
    SfElement* W = SfAddWindow(Ui, Title, 64, 6);
    if (!W)
        return NULL;
    SfElement* Line = SfAddLabel(Ui, 1, 0, -1, "");
    SfElement* Bar = SfAddBar(Ui, 1, 1, -1);
    SfAddLabel(Ui, 1, 3, 0, "Esc stops it");
    if (Line)
        SfSetAlign(Line, SF_ALIGN_TAIL);
    if (Bar)
        SfSetVisible(Bar, false);
    Ui->ProgressShown = 0;
    return W;
}

void SfSetProgress(SfElement* W, const char* Line, int Percent, bool Now)
{
    if (!W)
        return;
    SfUi* Ui = W->Ui;
    uint64_t Ms = UiNow(Ui);
    if (!Now && Ms - Ui->ProgressShown < 100)
        return;
    Ui->ProgressShown = Ms;
    SfElement* Text = W->Next;          // as SfProgress added them
    SfElement* Bar = Text ? Text->Next : NULL;
    if (Text)
        SfSetText(Text, Line);
    if (Bar)
    {
        Bar->Visible = Percent >= 0;
        SfSetValue(Bar, Percent >= 0 ? (uint32_t)Percent : 0);
    }
    SfUiShow(Ui);
}
