#ifndef SFUI_H
#define SFUI_H

// sfui: full-screen programs of elements - labels, frames, lists, fields,
// buttons, check boxes - in the console's cells, driven by the keys.
//
//     SfUi* Ui = SfUiOpen(Sys);
//     SfElement* L = SfAddLabel(Ui, 2, 1, 0, "Hello");
//     SfSetColors(L, SF_COLOR_BRIGHT | SF_COLOR_WHITE, SF_COLOR_BLUE);
//     SfUiOnKey(Ui, OnKey);            // F10 there: SfUiEnd(Ui, 1)
//     SfUiRun(Ui);
//     SfUiClose(Ui);
//
// The screen is one list of elements, drawn in the order they were added:
// what comes later lies on top. A window (SfAddWindow) starts a layer: the
// elements added after it are its content, placed inside it, and only the
// top layer takes keys. SfRemove(Window) closes it - with all that came
// after it - and the layer below takes the keys again.
//
// Places: X and Y count from the top left of the element's area - the
// inside of its layer's window, or the screen; negative ones from the right
// and the bottom. A Width or Height of 0 reaches the area's edge, a negative
// one stops that many cells before it.
//
// Keys go first to the focused element's handler (SfOnKey), then to the
// element itself (a list moves its cursor), then Tab and Shift+Tab move the
// focus, then the program's handler (SfUiOnKey) - in the bottom layer; in a
// window Esc ends SfUiRun with 0 and a letter presses the button it starts.

#include <sfos.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SfUi SfUi;
typedef struct SfElement SfElement;

/// Called with a key the focused element did not take (SfOnKey) or that
/// nothing in the bottom layer took (SfUiOnKey; Element is null then).
///
/// True: the key is used up.
typedef bool (*SfKeyHandler)(SfUi* Ui, SfElement* Element, SfKey Key);

/// Called when an element is chosen: Enter on a list, field or button,
/// Space on a button or check box (after it turned over).
typedef void (*SfChooseHandler)(SfUi* Ui, SfElement* Element);

/// Draws a custom element (SfAddCustom) in its place on the screen, with
/// SfPut, SfText, SfTextIn, SfFill.
typedef void (*SfPaintHandler)(SfUi* Ui, SfElement* Element, uint32_t X, uint32_t Y,
                               uint32_t Width, uint32_t Height);

/// Draws row Index of a list (SfOnDrawItem) at (X, Y), Width cells.
///
/// Cursor: the list's cursor is on it and the list has the focus.
typedef void (*SfDrawItemHandler)(SfUi* Ui, SfElement* List, uint32_t Index, uint32_t X,
                                  uint32_t Y, uint32_t Width, bool Cursor);

/// Called every so many milliseconds while SfUiRun waits (SfUiOnTimer).
typedef void (*SfTimerHandler)(SfUi* Ui);

// --- the screen --------------------------------------------------------------

/// Takes the screen for elements: SF_CONSOLE_RAW, and cells of its size.
///
/// Null without memory, or on a system without WaitAny.
SfUi* SfUiOpen(SfSystem* Sys);

/// Gives the screen back (SF_CONSOLE_LINE) with every element.
void SfUiClose(SfUi* Ui);

/// *Columns and *Rows get the size of the screen.
void SfUiSize(SfUi* Ui, uint32_t* Columns, uint32_t* Rows);

/// Draws the elements and waits for keys, handing them out, until
/// SfUiEnd; its Result comes back.
///
/// It may be called again from a handler: a dialog waits for its answer so.
/// Esc in a window ends it with 0.
int SfUiRun(SfUi* Ui);

/// Ends the innermost SfUiRun with Result.
void SfUiEnd(SfUi* Ui, int Result);

/// Draws the elements now - for a job that runs without SfUiRun.
void SfUiShow(SfUi* Ui);

/// Esc was pressed since the last look. Other keys waiting are dropped.
///
/// For a job that runs without SfUiRun, to be stopped.
bool SfUiEscape(SfUi* Ui);

/// Handler for the keys nothing in the bottom layer took.
void SfUiOnKey(SfUi* Ui, SfKeyHandler Handler);

/// Handler called every Ms milliseconds while SfUiRun waits; 0 stops it.
void SfUiOnTimer(SfUi* Ui, uint64_t Ms, SfTimerHandler Handler);

// --- elements ----------------------------------------------------------------

/// A line of text, Width cells (0: to the edge).
SfElement* SfAddLabel(SfUi* Ui, sint32_t X, sint32_t Y, sint32_t Width, const char* Text);

/// A frame with Title in its top line; it fills its inside.
SfElement* SfAddFrame(SfUi* Ui, sint32_t X, sint32_t Y, sint32_t Width, sint32_t Height,
                      const char* Title);

/// A window in the middle of the screen, double-lined, with a shadow; it
/// starts a layer (see above). Width and Height as for elements, of the
/// screen, and at most two cells less than it.
///
/// Its colours are what the elements in it start with.
SfElement* SfAddWindow(SfUi* Ui, const char* Title, sint32_t Width, sint32_t Height);

/// A list of Count rows with a cursor; Up, Down, Page Up, Page Down, Home
/// and End move it. Arrows past its right edge show that there is more.
///
/// Its rows are SfSetItems' strings, or drawn by SfOnDrawItem.
SfElement* SfAddList(SfUi* Ui, sint32_t X, sint32_t Y, sint32_t Width, sint32_t Height);

/// A line to edit, Width cells; SfSetText gives it what to offer. The
/// first character typed replaces that, an arrow keeps it to be changed.
SfElement* SfAddField(SfUi* Ui, sint32_t X, sint32_t Y, sint32_t Width, uint64_t Size);

/// A button: "[ Text ]".
SfElement* SfAddButton(SfUi* Ui, sint32_t X, sint32_t Y, const char* Text);

/// A check box: "[x] Text", "[ ] Text"; Space turns it over.
SfElement* SfAddCheckBox(SfUi* Ui, sint32_t X, sint32_t Y, const char* Text, bool Checked);

/// A bar of Width cells, its value in per cent of it full.
SfElement* SfAddBar(SfUi* Ui, sint32_t X, sint32_t Y, sint32_t Width);

/// The bottom line of the screen: "1Help  2Save ...", ten names for
/// F1..F10 ("" for none), kept as they are, not copied; SfSetItems changes
/// them.
SfElement* SfAddKeyBar(SfUi* Ui, const char* const* Names);

/// An element the program draws itself (SfOnPaint). With a key handler
/// (SfOnKey) it takes the focus too.
SfElement* SfAddCustom(SfUi* Ui, sint32_t X, sint32_t Y, sint32_t Width, sint32_t Height);

/// Takes Element and every element added after it off the screen: closes
/// a window with what is in it.
void SfRemove(SfElement* Element);

// --- properties --------------------------------------------------------------

/// SfSetAlign: the text at the left.
#define SF_ALIGN_LEFT       0
/// SfSetAlign: the text in the middle.
#define SF_ALIGN_CENTER     1
/// SfSetAlign: the text at the right.
#define SF_ALIGN_RIGHT      2
/// SfSetAlign, added to one of the others: a text too long shows its end -
/// of a path, the name matters.
#define SF_ALIGN_TAIL       4

/// SfSetLines: none.
#define SF_LINES_NONE       0
/// SfSetLines: single lines.
#define SF_LINES_SINGLE     1
/// SfSetLines: double lines.
#define SF_LINES_DOUBLE     2

/// The text (copied): of a label, field, button, check box; the title of a
/// frame or window.
void SfSetText(SfElement* Element, const char* Text);

/// The text as it is now: what was typed into a field.
const char* SfGetText(SfElement* Element);

/// Ink and paper: SF_COLOR_*. For a bar its full part, for a key bar its
/// names.
void SfSetColors(SfElement* Element, uint8_t Ink, uint8_t Paper);

/// Ink and paper with the focus: a list's cursor, a field, a button, a check
/// box. For a frame or window its title, for a bar its empty part, for a key
/// bar its digits.
void SfSetFocusColors(SfElement* Element, uint8_t Ink, uint8_t Paper);

/// Where the text goes: SF_ALIGN_*.
void SfSetAlign(SfElement* Element, uint32_t Align);

/// The lines of a frame or window: SF_LINES_*.
void SfSetLines(SfElement* Element, uint32_t Lines);

/// Shown or not; a hidden element takes no keys.
void SfSetVisible(SfElement* Element, bool Visible);

/// A new place and size (as SfAdd* takes them).
void SfSetPlace(SfElement* Element, sint32_t X, sint32_t Y, sint32_t Width, sint32_t Height);

/// What the program wants kept with the element.
void SfSetTag(SfElement* Element, void* Tag);

/// What SfSetTag kept.
void* SfGetTag(SfElement* Element);

/// Ends the innermost SfUiRun with Result (not 0) when the element is
/// chosen - a dialog's buttons answer so.
void SfSetResult(SfElement* Element, int Result);

/// How many rows a list has.
void SfSetCount(SfElement* List, uint32_t Count);

/// A list's rows as strings, Count of them (kept as they are, not copied);
/// a key bar's ten names.
void SfSetItems(SfElement* List, const char* const* Items, uint32_t Count);

/// Puts a list's cursor on row Index.
void SfSetCursor(SfElement* List, uint32_t Index);

/// The row a list's cursor is on.
uint32_t SfGetCursor(SfElement* List);

/// Marks a check box, or takes its mark off.
void SfSetChecked(SfElement* CheckBox, bool Checked);

/// A check box is marked.
bool SfGetChecked(SfElement* CheckBox);

/// A bar's value, 0..100.
void SfSetValue(SfElement* Bar, uint32_t Percent);

// --- events ------------------------------------------------------------------

/// Gets the keys first while the element has the focus.
void SfOnKey(SfElement* Element, SfKeyHandler Handler);

/// Called when the element is chosen.
void SfOnChoose(SfElement* Element, SfChooseHandler Handler);

/// Draws a custom element.
void SfOnPaint(SfElement* Element, SfPaintHandler Handler);

/// Draws each row of a list.
void SfOnDrawItem(SfElement* List, SfDrawItemHandler Handler);

// --- focus -------------------------------------------------------------------

/// Gives the element the focus: its keys.
void SfFocus(SfElement* Element);

/// The element with the focus; null for none.
SfElement* SfFocused(SfUi* Ui);

// --- drawing: in SfOnPaint and SfOnDrawItem ----------------------------------

/// One cell at (X, Y) of the screen; Color as SF_CELL_COLOR. Outside the
/// screen: nothing.
void SfPut(SfUi* Ui, uint32_t X, uint32_t Y, char C, uint8_t Color);

/// Text at (X, Y); where it ends.
uint32_t SfText(SfUi* Ui, uint32_t X, uint32_t Y, const char* Text, uint8_t Color);

/// Text in exactly Width cells: cut, or padded with spaces.
void SfTextIn(SfUi* Ui, uint32_t X, uint32_t Y, const char* Text, uint32_t Width,
              uint8_t Color);

/// Width cells of C from (X, Y).
void SfFill(SfUi* Ui, uint32_t X, uint32_t Y, uint32_t Width, char C, uint8_t Color);

/// Shows the text cursor at (X, Y) on this drawing.
void SfCaret(SfUi* Ui, uint32_t X, uint32_t Y);

// --- dialogs: windows that wait for their answer -----------------------------

/// Title, one or two lines (Line2 may be null) and Count buttons. The
/// button chosen, -1 for Esc. Error: in the colours of an error.
int SfButtons(SfUi* Ui, const char* Title, const char* Line1, const char* Line2,
              const char* const* Labels, uint32_t Count, bool Error);

/// A message with [ OK ].
void SfMessage(SfUi* Ui, const char* Title, const char* Line1, const char* Line2, bool Error);

/// [ Yes ] and [ No ]: true for Yes.
bool SfConfirm(SfUi* Ui, const char* Title, const char* Line1, const char* Line2);

/// A line to type into Buffer (Size bytes); what it holds is offered.
/// False for Esc.
bool SfInput(SfUi* Ui, const char* Title, const char* Prompt, char* Buffer, uint64_t Size);

/// The item chosen from Count, the cursor first on Start; -1 for Esc. A
/// key the list has no use for ends it too when Other is given: *Other gets
/// it, the item under the cursor comes back (Other->Code is 0 after Enter).
int SfMenu(SfUi* Ui, const char* Title, const char* const* Items, uint32_t Count,
           uint32_t Start, SfKey* Other);

/// A window for a job that runs without SfUiRun: Title, a line and a bar,
/// "Esc stops it". SfRemove closes it.
SfElement* SfProgress(SfUi* Ui, const char* Title);

/// The line and the per cent (below 0: no bar) of a progress window, drawn
/// at most ten times a second unless Now.
void SfSetProgress(SfElement* Progress, const char* Line, int Percent, bool Now);

#ifdef __cplusplus
}
#endif

#endif // SFUI_H
