#ifndef SFUI_INTERNAL_H
#define SFUI_INTERNAL_H

// sfui inside: what its files share.

#include <sfui.h>
#include <string.h>

// The kinds of elements. The ones from UI_LABEL on are one row high.
enum
{
    UI_FRAME = 1,
    UI_WINDOW,
    UI_LIST,
    UI_CUSTOM,
    UI_LABEL,
    UI_FIELD,
    UI_BUTTON,
    UI_CHECKBOX,
    UI_BAR,
    UI_KEYBAR,
};

struct SfElement
{
    SfUi*       Ui;
    SfElement*  Prev;
    SfElement*  Next;
    uint32_t    Kind;
    sint32_t    X, Y, W, H;             // as SfAdd* took them
    bool        Visible;

    char*       Text;                   // never null; a field's has Size bytes
    uint8_t     Ink, Paper, FocusInk, FocusPaper;
    uint32_t    Align;
    uint32_t    Lines;
    void*       Tag;
    int         Result;

    // A list.
    uint32_t    Count, Cursor, Top;
    const char* const* Items;           // also a key bar's names

    // A field.
    uint64_t    Size, Len, Cur, Left;
    bool        Fresh;                  // what it offers goes at the first character

    bool        Checked;                // a check box
    uint32_t    Value;                  // a bar
    SfElement*  WasFocus;               // a window: the focus before it came

    SfKeyHandler      OnKey;
    SfChooseHandler   OnChoose;
    SfPaintHandler    OnPaint;
    SfDrawItemHandler OnDrawItem;
};

struct SfUi
{
    SfSystem*   Sys;
    SfConsole*  Con;
    uint32_t    Columns, Rows;
    SfCell*     Cells;
    SfElement*  First;
    SfElement*  Last;
    SfElement*  Focus;

    SfKeyHandler   OnKey;
    SfTimerHandler OnTimer;
    uint64_t    TimerMs, NextTick;

    bool        Ending;                 // SfUiEnd was called: the innermost run ends
    int         Result;

    bool        CaretOn;
    uint32_t    CaretX, CaretY;
    uint64_t    ProgressShown;          // when a progress window was drawn last
};

typedef struct UiRect
{
    uint32_t X, Y, W, H;
} UiRect;

void*      UiAlloc(SfUi* Ui, uint64_t Size);     // zeroed; null: no memory
void       UiFree(SfUi* Ui, void* Buffer);
uint64_t   UiNow(SfUi* Ui);

SfElement* UiLayerOf(SfElement* E);
SfElement* UiTopWindow(SfUi* Ui);
void       UiPlace(SfElement* E, UiRect* R);    // where it is on the screen
bool       UiTakesFocus(SfElement* E);
void       UiOffer(SfElement* E);
void       UiMoveFocus(SfUi* Ui, int Step, uint32_t Kind);
void       UiScroll(SfElement* List, uint32_t Shown);
void       UiChoose(SfElement* E);

#endif // SFUI_INTERNAL_H
