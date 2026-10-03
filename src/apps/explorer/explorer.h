#ifndef EXPLORER_H
#define EXPLORER_H

// explorer: a two-panel file manager with an editor. What its parts share.
//
//   main.cpp    the panels, their keys and the commands
//   screen.cpp  the cells of the screen and the dialogs
//   ops.cpp     copying, moving and deleting, folders and all
//   tools.cpp   volumes, finding files, bookmarks, comparing, quick view
//   editor.cpp  the editor: text and hex
//   util.cpp    text, paths, memory and the keys
//
// A path here is an absolute path of the whole disk ("/apps/hello"), as in
// the console; the files are reached through the root disk:/.

#include <sfos.h>

extern SfSystem*  Sys;
extern SfConsole* Con;
extern SfAdmin*   Admin;

const uint64_t PATH_SIZE = 512;

// --- colours ---------------------------------------------------------------

const uint8_t COL_FILE     = SF_CELL_COLOR(SF_COLOR_BRIGHT | SF_COLOR_CYAN, SF_COLOR_BLUE);
const uint8_t COL_FOLDER   = SF_CELL_COLOR(SF_COLOR_BRIGHT | SF_COLOR_WHITE, SF_COLOR_BLUE);
const uint8_t COL_PROGRAM  = SF_CELL_COLOR(SF_COLOR_BRIGHT | SF_COLOR_GREEN, SF_COLOR_BLUE);
const uint8_t COL_MARKED   = SF_CELL_COLOR(SF_COLOR_BRIGHT | SF_COLOR_YELLOW, SF_COLOR_BLUE);
const uint8_t COL_FRAME    = SF_CELL_COLOR(SF_COLOR_WHITE, SF_COLOR_BLUE);
const uint8_t COL_DIM      = SF_CELL_COLOR(SF_COLOR_CYAN, SF_COLOR_BLUE);
const uint8_t COL_CURSOR   = SF_CELL_COLOR(SF_COLOR_BLACK, SF_COLOR_CYAN);
const uint8_t COL_CURSOR_MARKED = SF_CELL_COLOR(SF_COLOR_BRIGHT | SF_COLOR_YELLOW, SF_COLOR_CYAN);
const uint8_t COL_LINE     = SF_CELL_COLOR(SF_COLOR_WHITE, SF_COLOR_BLACK);
const uint8_t COL_KEY      = SF_CELL_COLOR(SF_COLOR_BRIGHT | SF_COLOR_WHITE, SF_COLOR_BLACK);
const uint8_t COL_KEY_NAME = SF_CELL_COLOR(SF_COLOR_BLACK, SF_COLOR_CYAN);
const uint8_t COL_DIALOG   = SF_CELL_COLOR(SF_COLOR_BLACK, SF_COLOR_WHITE);
const uint8_t COL_CHOSEN   = SF_CELL_COLOR(SF_COLOR_BRIGHT | SF_COLOR_WHITE, SF_COLOR_CYAN);
const uint8_t COL_FIELD    = SF_CELL_COLOR(SF_COLOR_BLACK, SF_COLOR_CYAN);
const uint8_t COL_ERROR    = SF_CELL_COLOR(SF_COLOR_BRIGHT | SF_COLOR_WHITE, SF_COLOR_RED);

// --- util.cpp --------------------------------------------------------------

extern "C"
{
    void* memcpy(void* Dst, const void* Src, size_t Size);
    void* memmove(void* Dst, const void* Src, size_t Size);
    void* memset(void* Dst, int Value, size_t Size);
}

uint64_t Length(const char* S);
void  Copy(char* Dst, const char* Src, uint64_t Size);      // cut to Size, NUL included
char* Append(char* Out, const char* S);                     // where it ends
char* Number(char* Out, uint64_t Value, uint32_t Width = 0, char Pad = ' ');
char* HexNumber(char* Out, uint64_t Value, uint32_t Digits);
char* SizeText(char* Out, uint64_t Bytes);                  // at most 9 characters
char  Lower(char C);
bool  Same(const char* A, const char* B);
int   CompareNoCase(const char* A, const char* B);
bool  Match(const char* Mask, const char* Name);            // * and ?, any case
const char* Why(SfStatus Status);

void* Alloc(uint64_t Size);                                 // nullptr: no memory
void  Release(void* Buffer);

// Paths.
bool  Join(char* Out, const char* Folder, const char* Name);    // false: too long
void  Absolute(const char* Base, const char* Path, char* Out);
void  ParentOf(const char* Path, char* Out);
const char* NameOf(const char* Path);
bool  Inside(const char* Path, const char* Folder);         // Path is Folder or below it
void  DiskPath(const char* Path, char* Out);                // PATH_SIZE + 8 bytes
SfStatus Open(const char* Path, uint64_t Mode, SfFile** Out);
bool  IsFolder(SfFile* File);

// Keys: looked for without waiting too, so that Esc can stop a long job.
SfKey GetKey();                                             // waits for one
bool  Cancelled();                                          // Esc was pressed meanwhile
inline bool Ctrl(const SfKey& K, char Letter)
{
    return (K.Mods & SF_MOD_CTRL) && K.Char == Letter - 'a' + 1;
}
inline bool Types(const SfKey& K)                           // a character to type
{
    return (uint8_t)K.Char >= 32 && K.Char != 127 && !(K.Mods & (SF_MOD_CTRL | SF_MOD_ALT));
}

// --- screen.cpp ------------------------------------------------------------

extern uint32_t Columns, Rows;

void     InitScreen();
void     Put(uint32_t X, uint32_t Y, char C, uint8_t Color);
uint32_t Text(uint32_t X, uint32_t Y, const char* S, uint8_t Color);
void     TextIn(uint32_t X, uint32_t Y, const char* S, uint32_t Width, uint8_t Color);
void     Fill(uint32_t X, uint32_t Y, uint32_t Width, char C, uint8_t Color);
void     Box(uint32_t X, uint32_t Y, uint32_t Width, uint32_t Height, uint8_t Color);
void     KeyBar(const char* const* Names);                  // ten names, F1..F10
void     Show();                                            // the cells onto the screen
void     ShowWithCursor(uint32_t X, uint32_t Y);

// Dialogs: drawn over the screen behind them, which Repaint puts together
// (into the cells, without showing it) each time a dialog draws itself.
extern void (*Repaint)();
int  Buttons(const char* Title, const char* Line1, const char* Line2,
             const char* const* Labels, uint32_t Count, bool Error = false);
void Message(const char* Title, const char* Line, SfStatus Status = SF_SUCCESS);
bool Confirm(const char* Title, const char* Line1, const char* Line2 = nullptr);
bool Input(const char* Title, const char* Prompt, char* Buffer, uint64_t Size);
// The chosen item, -1 for Esc. A key the list has no use for ends it too
// when Other is given: *Other gets the key, the item under the cursor comes
// back (Other->Code stays 0 after Enter).
int  Menu(const char* Title, const char* const* Items, uint32_t Count, uint32_t Start,
          SfKey* Other = nullptr);
void Progress(const char* Title, const char* Line, int Percent, bool Now = false);

// --- main.cpp --------------------------------------------------------------

struct Entry
{
    char       Name[256];
    uint64_t   Size;
    SfDateTime Modified;
    bool       Folder;
    bool       Marked;
    bool       Counted;         // a folder whose Size was counted (Space)
};

struct Panel
{
    char     Path[PATH_SIZE];
    Entry*   Items;
    uint32_t Count, Capacity;
    uint32_t Cursor, Top;
    uint32_t Sort;              // SORT_*
    bool     Reverse;
    bool     Closed;            // the system does not let this folder be listed
};

extern Panel    Panels[2];
extern uint32_t Active;

// Show folder Path in P, the cursor on Focus (a name) when it is there.
void GoTo(Panel* P, const char* Path, const char* Focus = nullptr);
void Reload(Panel* P);

// --- ops.cpp ---------------------------------------------------------------

void BeginJob(const char* Title);
bool CopyItem(const char* From, const char* To);    // false: the job was stopped
bool MoveItem(const char* From, const char* To);
bool DeleteItem(const char* Path);
bool FolderSize(const char* Path, uint64_t* Bytes, uint64_t* Files);

// --- tools.cpp -------------------------------------------------------------

extern char     Bookmarks[32][PATH_SIZE];
extern uint32_t BookmarkCount;

void LoadConfig();
void SaveConfig();
void Volumes();
void FindFiles();
void BookmarkMenu();
void CompareFolders();
void RunProgram(const char* Path);
void DrawQuickView(uint32_t X, uint32_t Y, uint32_t Width, uint32_t Height, const char* Folder,
                   const Entry* E);

// --- editor.cpp ------------------------------------------------------------

bool LooksBinary(const uint8_t* Data, uint64_t Size);
void Edit(const char* Path, bool ReadOnly);

#endif // EXPLORER_H
