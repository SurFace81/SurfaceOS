// explorer: a file manager of two panels, with an editor. Run it as
// `sudo explorer`: it works on the whole disk, which takes the admin right.
//
// A full-screen program (SF_CONSOLE_RAW), drawn as taskmgr is: the screen
// is put together in cells and shown with one Draw. Each panel lists a
// folder; Tab goes from one to the other, and what is copied or moved goes
// from the panel with the cursor to the other one. F1 lists the keys, F9
// the commands.

#include "explorer.h"

SfSystem*  Sys;
SfConsole* Con;
SfAdmin*   Admin;

Panel    Panels[2];
uint32_t Active;

enum { SORT_NAME, SORT_EXTENSION, SORT_TIME, SORT_SIZE };

static bool QuickView;              // the other panel shows the file under the cursor
static char Search[64];             // the start of a name, as typed

static Panel* Here()  { return &Panels[Active]; }
static Panel* There() { return &Panels[1 - Active]; }

static bool IsUp(const Entry* E)
{
    return E->Name[0] == '.' && E->Name[1] == '.' && !E->Name[2];
}

static Entry* UnderCursor(Panel* P)
{
    return P->Cursor < P->Count ? &P->Items[P->Cursor] : nullptr;
}

// --- reading a folder ------------------------------------------------------

static uint64_t Stamp(const SfDateTime* T)
{
    return (uint64_t)T->Year << 40 | (uint64_t)T->Month << 32 | (uint64_t)T->Day << 24 |
           (uint64_t)T->Hour << 16 | (uint64_t)T->Minute << 8 | T->Second;
}

static const char* Extension(const char* Name)
{
    const char* Ext = "";
    for (const char* c = Name + 1; Name[0] && *c; c++)
        if (*c == '.')
            Ext = c + 1;
    return Ext;
}

// ".." first, then the folders, then the files, each by the panel's order.
static bool Before(const Entry* A, const Entry* B, const Panel* P)
{
    if (IsUp(A) || IsUp(B))
        return IsUp(A) && !IsUp(B);
    if (A->Folder != B->Folder)
        return A->Folder;
    sint64_t d = 0;
    uint64_t a, b;
    switch (P->Sort)
    {
        case SORT_EXTENSION:
            d = CompareNoCase(Extension(A->Name), Extension(B->Name));
            break;
        case SORT_TIME:                 // the newest first
            a = Stamp(&A->Modified), b = Stamp(&B->Modified);
            d = a > b ? -1 : a < b ? 1 : 0;
            break;
        case SORT_SIZE:                 // the biggest first
            d = A->Size > B->Size ? -1 : A->Size < B->Size ? 1 : 0;
            break;
    }
    if (P->Reverse)
        d = -d;
    if (!d)
        d = CompareNoCase(A->Name, B->Name);
    return d < 0;
}

static void SortItems(Panel* P)
{
    static Entry T;
    for (uint32_t Gap = P->Count / 2; Gap > 0; Gap /= 2)
        for (uint32_t i = Gap; i < P->Count; i++)
        {
            T = P->Items[i];
            uint32_t j = i;
            for (; j >= Gap && Before(&T, &P->Items[j - Gap], P); j -= Gap)
                P->Items[j] = P->Items[j - Gap];
            P->Items[j] = T;
        }
}

static Entry* AddEntry(Panel* P)
{
    if (P->Count == P->Capacity)
    {
        uint32_t Wanted = P->Capacity ? P->Capacity * 2 : 128;
        Entry* Bigger = (Entry*)Alloc((uint64_t)Wanted * sizeof(Entry));
        if (!Bigger)
            return nullptr;
        memcpy(Bigger, P->Items, (uint64_t)P->Count * sizeof(Entry));
        Release(P->Items);
        P->Items = Bigger;
        P->Capacity = Wanted;
    }
    Entry* E = &P->Items[P->Count++];
    memset(E, 0, sizeof(Entry));
    return E;
}

static void Focus(Panel* P, const char* Name)
{
    for (uint32_t i = 0; Name && i < P->Count; i++)
        if (CompareNoCase(P->Items[i].Name, Name) == 0)
            P->Cursor = i;
    if (P->Cursor >= P->Count)
        P->Cursor = P->Count ? P->Count - 1 : 0;
}

// List P->Path; false when it does not open.
static bool Read(Panel* P)
{
    SfFile* Dir;
    if (SF_ERROR(Open(P->Path, SF_FILE_READ, &Dir)))
        return false;
    if (!IsFolder(Dir))
    {
        Dir->Close(Dir);
        return false;
    }
    P->Count = 0;
    if (P->Path[1])                     // not "/"
    {
        Entry* Up = AddEntry(P);
        if (Up)
        {
            Copy(Up->Name, "..", sizeof(Up->Name));
            Up->Folder = true;
        }
    }
    SfDirEntry D;
    SfStatus Status;
    while ((Status = Dir->ReadDir(Dir, &D)) == SF_SUCCESS)
    {
        Entry* E = AddEntry(P);
        if (!E)
            break;
        Copy(E->Name, D.Name, sizeof(E->Name));
        E->Size     = D.Size;
        E->Modified = D.Modified;
        E->Folder   = (D.Flags & SF_DIR_ENTRY_FOLDER) != 0;
    }
    Dir->Close(Dir);
    P->Closed = Status != SF_END_OF_FILE;       // /tmp: a program may not list its tmp:/
    SortItems(P);
    return true;
}

// The folders been to, the last one first.
static char     History[16][PATH_SIZE];
static uint32_t HistoryCount;

static void Remember(const char* Path)
{
    uint32_t At = 0;
    while (At < HistoryCount && !Same(History[At], Path))
        At++;
    if (At == 16)
        At = 15;                        // full: the oldest goes
    else if (At == HistoryCount)
        HistoryCount++;
    for (; At > 0; At--)
        Copy(History[At], History[At - 1], PATH_SIZE);
    Copy(History[0], Path, PATH_SIZE);
}

// A folder that is gone (deleted, its stick taken out) gives the nearest
// one above it that is still there.
void GoTo(Panel* P, const char* Path, const char* Name)
{
    char Wanted[PATH_SIZE], Kept[256] = "";
    Absolute("/", Path, Wanted);
    if (Name)
        Copy(Kept, Name, sizeof(Kept));         // Name may point into P->Items
    Copy(P->Path, Wanted, PATH_SIZE);
    while (!Read(P) && P->Path[1])
    {
        ParentOf(P->Path, Wanted);
        Copy(P->Path, Wanted, PATH_SIZE);
    }
    P->Top = 0;
    P->Cursor = 0;
    Focus(P, Kept);
    Remember(P->Path);
}

void Reload(Panel* P)
{
    char Name[256] = "";
    uint32_t Was = P->Cursor;
    if (UnderCursor(P))
        Copy(Name, UnderCursor(P)->Name, sizeof(Name));
    char Path[PATH_SIZE];
    Copy(Path, P->Path, PATH_SIZE);
    uint32_t Top = P->Top;
    GoTo(P, Path, nullptr);
    P->Top = Top;
    P->Cursor = Was;                    // its name gone: the same place
    Focus(P, Name);
}

static void ReloadBoth()
{
    Reload(&Panels[0]);
    Reload(&Panels[1]);
}

// --- drawing ---------------------------------------------------------------

static uint32_t MarkedCount(const Panel* P, uint64_t* Bytes = nullptr)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < P->Count; i++)
        if (P->Items[i].Marked)
        {
            n++;
            if (Bytes)
                *Bytes += P->Items[i].Size;
        }
    return n;
}

static void DrawPanel(uint32_t Index, uint32_t X, uint32_t W)
{
    Panel* P = &Panels[Index];
    uint32_t H = Rows - 2;
    bool Mine = Index == Active;

    for (uint32_t y = 0; y < H; y++)
        Fill(X, y, W, ' ', COL_FILE);
    Box(X, 0, W, H, COL_FRAME);

    // The folder in the top line; a long one shows its end.
    uint64_t Len = Length(P->Path);
    uint32_t Room = W - 6;
    const char* Title = Len > Room ? P->Path + (Len - Room) : P->Path;
    uint32_t n = (uint32_t)Length(Title);
    uint32_t At = X + (W - n - 2) / 2;
    Put(At, 0, ' ', Mine ? COL_CURSOR : COL_FRAME);
    At = Text(At + 1, 0, Title, Mine ? COL_CURSOR : COL_FRAME);
    Put(At, 0, ' ', Mine ? COL_CURSOR : COL_FRAME);

    if (QuickView && !Mine)
    {
        DrawQuickView(X + 1, 1, W - 2, H - 2, Here()->Path, UnderCursor(Here()));
        return;
    }

    // Name | Size | Date, the date without its time in a narrow panel.
    uint32_t Inner = W - 2, SizeW = 9, DateW = Inner >= 48 ? 14 : 8;
    uint32_t NameW = Inner - SizeW - DateW - 2;
    uint32_t SizeX = X + 1 + NameW + 1, DateX = SizeX + SizeW + 1;
    Text(X + 1 + (NameW - 4) / 2, 1, "Name", COL_MARKED);
    Text(SizeX + 2, 1, "Size", COL_MARKED);
    Text(DateX + (DateW - 4) / 2, 1, "Date", COL_MARKED);

    uint32_t Shown = H - 5;
    if (P->Cursor < P->Top)
        P->Top = P->Cursor;
    if (P->Cursor >= P->Top + Shown)
        P->Top = P->Cursor - Shown + 1;
    if (P->Top + Shown > P->Count)
        P->Top = P->Count > Shown ? P->Count - Shown : 0;

    bool Programs = Same(P->Path, "/apps");
    for (uint32_t r = 0; r < Shown; r++)
    {
        uint32_t Y = 2 + r, i = P->Top + r;
        Put(SizeX - 1, Y, SF_BOX_V, COL_FRAME);
        Put(DateX - 1, Y, SF_BOX_V, COL_FRAME);
        if (i >= P->Count)
            continue;
        const Entry* E = &P->Items[i];
        bool Cursor = Mine && i == P->Cursor;
        uint8_t Color = Cursor ? (E->Marked ? COL_CURSOR_MARKED : COL_CURSOR)
                      : E->Marked ? COL_MARKED
                      : E->Folder ? COL_FOLDER
                      : Programs ? COL_PROGRAM : COL_FILE;
        TextIn(X + 1, Y, E->Name, NameW, Color);
        if (Length(E->Name) > NameW)
            Put(X + NameW, Y, SF_TRIANGLE_RIGHT, Color);        // there is more of it

        char Part[32], Line[64];
        if (IsUp(E))
            Copy(Part, "UP", sizeof(Part));
        else if (E->Folder && !E->Counted)
            Copy(Part, "<DIR>", sizeof(Part));
        else
            SizeText(Part, E->Size);
        uint32_t Pad = SizeW - (uint32_t)Length(Part);
        memset(Line, ' ', Pad);
        Append(Line + Pad, Part);
        Put(SizeX - 1, Y, SF_BOX_V, Cursor ? Color : COL_FRAME);
        Text(SizeX, Y, Line, Color);

        char* p = Line;
        if (IsUp(E))
            *p = '\0';
        else
        {
            p = Number(p, E->Modified.Day, 2, '0');     *p++ = '.';
            p = Number(p, E->Modified.Month, 2, '0');   *p++ = '.';
            p = Number(p, E->Modified.Year % 100, 2, '0');
            if (DateW > 8)
            {
                *p++ = ' ';
                p = Number(p, E->Modified.Hour, 2, '0');    *p++ = ':';
                p = Number(p, E->Modified.Minute, 2, '0');
            }
        }
        Put(DateX - 1, Y, SF_BOX_V, Cursor ? Color : COL_FRAME);
        TextIn(DateX, Y, Line, DateW, Color);
    }
    Put(SizeX - 1, 1, SF_BOX_V, COL_FRAME);
    Put(DateX - 1, 1, SF_BOX_V, COL_FRAME);
    if (P->Closed)
        TextIn(X + 1, 4, "The system does not let this folder be listed.", NameW, COL_MARKED);

    // Under the list: what is marked, or the whole name under the cursor.
    Fill(X + 1, H - 3, Inner, SF_BOX_H, COL_FRAME);
    char Line[PATH_SIZE];
    uint64_t Bytes = 0;
    uint32_t Marked = MarkedCount(P, &Bytes);
    if (Marked)
    {
        char* p = Number(Line, Marked);
        p = Append(p, " marked, ");
        p = Number(p, Bytes);
        Append(p, " bytes");
        TextIn(X + 1, H - 2, Line, Inner, COL_MARKED);
    }
    else if (UnderCursor(P))
    {
        const Entry* E = UnderCursor(P);
        uint64_t L = Length(E->Name);
        TextIn(X + 1, H - 2, L > Inner ? E->Name + (L - Inner) : E->Name, Inner, COL_FILE);
    }
}

// The whole screen into the cells.
static void Compose()
{
    uint32_t Half = Columns / 2;
    DrawPanel(0, 0, Half);
    DrawPanel(1, Half, Columns - Half);

    // The line above the keys: the folder, or the name being typed.
    uint32_t Y = Rows - 2;
    Fill(0, Y, Columns, ' ', COL_LINE);
    if (Search[0])
    {
        uint32_t X = Text(0, Y, "Search: ", COL_KEY);
        Text(X, Y, Search, COL_KEY);
    }
    else
    {
        static const char* const Orders[] = { "name", "extension", "date", "size" };
        char Line[48];
        char* p = Append(Line, "by ");
        p = Append(p, Orders[Here()->Sort]);
        if (Here()->Reverse)
            p = Append(p, ", reversed");
        Append(p, "   F9 commands ");
        uint32_t n = (uint32_t)Length(Line);
        TextIn(0, Y, Here()->Path, Columns - n - 1, COL_LINE);
        Text(Columns - n, Y, Line, COL_LINE);
    }

    static const char* const Names[10] =
        { "Help", "Rename", "View", "Edit", "Copy", "Move", "Folder", "Delete", "Menu", "Quit" };
    KeyBar(Names);
}

static void Draw()
{
    Compose();
    Con->SetTitle(Con, Here()->Path);
    Show();
}

// --- commands --------------------------------------------------------------

// What a command works on: the marked ones, or else the one under the cursor.
static bool Chosen(const Panel* P, uint32_t i, bool AnyMarked)
{
    if (IsUp(&P->Items[i]))
        return false;
    return AnyMarked ? P->Items[i].Marked : i == P->Cursor;
}

static uint32_t ChosenCount(const Panel* P)
{
    bool Any = MarkedCount(P) != 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < P->Count; i++)
        n += Chosen(P, i, Any);
    return n;
}

static bool FolderAt(const char* Path)
{
    SfFile* File;
    if (SF_ERROR(Open(Path, SF_FILE_READ, &File)))
        return false;
    bool Result = IsFolder(File);
    File->Close(File);
    return Result;
}

// "<What> name" or "<What> 5 items", and its ending.
static void Describe(char* Out, const char* What, const Panel* P, const char* End)
{
    uint32_t n = ChosenCount(P);
    char* p = Append(Out, What);
    if (n == 1)
    {
        bool Any = MarkedCount(P) != 0;
        for (uint32_t i = 0; i < P->Count; i++)
            if (Chosen(P, i, Any))
            {
                *p++ = '"';
                p = Append(p, P->Items[i].Name);
                *p++ = '"';
            }
    }
    else
    {
        p = Number(p, n);
        p = Append(p, " items");
    }
    Append(p, End);
}

// F5 and F6. One item may be given a new name on the way: a destination
// that is not a folder is the name it gets.
static void CopyOrMove(bool Move)
{
    Panel* P = Here();
    uint32_t n = ChosenCount(P);
    if (!n)
        return;
    char Prompt[400], Typed[PATH_SIZE], Target[PATH_SIZE];
    Describe(Prompt, Move ? "Move " : "Copy ", P, " to:");
    Copy(Typed, There()->Path, sizeof(Typed));
    if (!Input(Move ? "Move" : "Copy", Prompt, Typed, sizeof(Typed)) || !Typed[0])
        return;
    Absolute(P->Path, Typed, Target);

    bool Into = FolderAt(Target);
    if (!Into && n > 1)
    {
        char Full[PATH_SIZE + 8];
        DiskPath(Target, Full);
        SfStatus Status = Sys->Files->CreateDirectory(Sys->Files, Full);
        if (SF_ERROR(Status))
            return Message("Cannot make the folder", Target, Status);
        Into = true;
    }

    BeginJob(Move ? "Moving" : "Copying");
    bool Any = MarkedCount(P) != 0;
    for (uint32_t i = 0; i < P->Count; i++)
    {
        if (!Chosen(P, i, Any))
            continue;
        char From[PATH_SIZE], To[PATH_SIZE];
        if (!Join(From, P->Path, P->Items[i].Name))
            continue;
        if (!Into)
            Copy(To, Target, sizeof(To));
        else if (!Join(To, Target, P->Items[i].Name))
            continue;
        if (!(Move ? MoveItem(From, To) : CopyItem(From, To)))
            break;
    }
    ReloadBoth();
}

static void Rename()
{
    Panel* P = Here();
    Entry* E = UnderCursor(P);
    if (!E || IsUp(E))
        return;
    char Typed[256], From[PATH_SIZE], To[PATH_SIZE];
    Copy(Typed, E->Name, sizeof(Typed));
    if (!Input("Rename", "The new name:", Typed, sizeof(Typed)) || !Typed[0] ||
        Same(Typed, E->Name))
        return;
    if (!Join(From, P->Path, E->Name))
        return;
    Absolute(P->Path, Typed, To);
    BeginJob("Renaming");
    MoveItem(From, To);
    ReloadBoth();
    Focus(P, NameOf(To));
}

static void DeleteChosen()
{
    Panel* P = Here();
    if (!ChosenCount(P))
        return;
    char Line[400];
    Describe(Line, "Delete ", P, "?");
    if (!Confirm("Delete", Line, "Folders go with everything in them."))
        return;
    BeginJob("Deleting");
    bool Any = MarkedCount(P) != 0;
    for (uint32_t i = 0; i < P->Count; i++)
    {
        char Path[PATH_SIZE];
        if (Chosen(P, i, Any) && Join(Path, P->Path, P->Items[i].Name) && !DeleteItem(Path))
            break;
    }
    ReloadBoth();
}

static void MakeFolder()
{
    char Typed[256] = "", Path[PATH_SIZE], Full[PATH_SIZE + 8];
    if (!Input("New folder", "Its name:", Typed, sizeof(Typed)) || !Typed[0])
        return;
    Absolute(Here()->Path, Typed, Path);
    DiskPath(Path, Full);
    SfStatus Status = Sys->Files->CreateDirectory(Sys->Files, Full);
    if (SF_ERROR(Status))
        return Message("Cannot make the folder", Path, Status);
    ReloadBoth();
    Focus(Here(), NameOf(Path));
}

static void NewFile()
{
    char Typed[256] = "", Path[PATH_SIZE];
    if (!Input("New file", "Its name:", Typed, sizeof(Typed)) || !Typed[0])
        return;
    Absolute(Here()->Path, Typed, Path);
    if (FolderAt(Path))
        return Message("New file", "A folder has that name");
    Edit(Path, false);
    ReloadBoth();
    Focus(Here(), NameOf(Path));
}

static void EditUnderCursor(bool View)
{
    Entry* E = UnderCursor(Here());
    char Path[PATH_SIZE];
    if (!E || E->Folder || !Join(Path, Here()->Path, E->Name))
        return;
    Edit(Path, View);
    ReloadBoth();
}

static void Enter()
{
    Panel* P = Here();
    Entry* E = UnderCursor(P);
    if (!E)
        return;
    char Path[PATH_SIZE];
    if (IsUp(E))
    {
        char Left[256];
        Copy(Left, NameOf(P->Path), sizeof(Left));
        ParentOf(P->Path, Path);
        GoTo(P, Path, Left);
    }
    else if (!Join(Path, P->Path, E->Name))
        Message("Open", "The path is too long");
    else if (E->Folder)
        GoTo(P, Path);
    else if (Same(P->Path, "/apps"))
        RunProgram(Path);
    else
    {
        Edit(Path, false);
        ReloadBoth();
    }
}

// Space and Ins: mark it or take the mark off, and step down. Space also
// counts what a folder holds.
static void Mark(bool Count)
{
    Panel* P = Here();
    Entry* E = UnderCursor(P);
    if (!E)
        return;
    if (!IsUp(E))
    {
        E->Marked = !E->Marked;
        if (Count && E->Folder && !E->Counted)
        {
            char Path[PATH_SIZE];
            uint64_t Bytes = 0, Files = 0;
            if (Join(Path, P->Path, E->Name) && FolderSize(Path, &Bytes, &Files))
            {
                E->Size = Bytes;
                E->Counted = true;
            }
        }
    }
    if (P->Cursor + 1 < P->Count)
        P->Cursor++;
}

static void MarkByMask(bool On)
{
    static char Mask[128] = "*";
    if (!Input(On ? "Mark" : "Unmark", "Names like (* and ? stand for anything):", Mask,
               sizeof(Mask)))
        return;
    Panel* P = Here();
    for (uint32_t i = 0; i < P->Count; i++)
        if (!IsUp(&P->Items[i]) && Match(Mask, P->Items[i].Name))
            P->Items[i].Marked = On;
}

static void SetSort(uint32_t Sort)
{
    Panel* P = Here();
    P->Reverse = P->Sort == Sort && !P->Reverse;    // the same again: backwards
    P->Sort = Sort;
    Reload(P);
}

static void HistoryMenu()
{
    const char* Items[16];
    for (uint32_t i = 0; i < HistoryCount; i++)
        Items[i] = History[i];
    int i = Menu("Folders been to", Items, HistoryCount, 0);
    if (i >= 0)
    {
        char Path[PATH_SIZE];
        Copy(Path, History[i], sizeof(Path));       // GoTo reorders the history
        GoTo(Here(), Path);
    }
}

static void Help()
{
    static const char* const Text[] =
    {
        "Tab           the other panel",
        "Enter         into a folder; edit a file; run a program in /apps",
        "Backspace     up to the folder above",
        "letters       jump to the name that starts so (Esc: forget it)",
        "",
        "F2            rename",
        "F3 / F4       view / edit;  Shift+F4: a new file",
        "F5 / F6       copy / move to the other panel, or to a path typed",
        "F7            a new folder",
        "F8, Del       delete",
        "",
        "Ins           mark, for the F keys to work on many",
        "Space         mark, and count what a folder holds",
        "+  -  *       mark by name, unmark by name, turn the marks over",
        "",
        "Ctrl+D        volumes: go to one, mount or unmount a stick",
        "Ctrl+F        find files by name and by what is in them",
        "Ctrl+Q        quick view: the other panel shows the file",
        "Ctrl+B        bookmarks",
        "Alt+Down      folders been to",
        "Ctrl+K        compare the folders: mark what differs",
        "Ctrl+U        swap the panels;  =  the same folder in the other",
        "Ctrl+R        read the folders again",
        "Ctrl+F3..F6   order by name, extension, date, size (again: reversed)",
        "F10           quit",
    };
    Menu("Keys", Text, sizeof(Text) / sizeof(Text[0]), 0);
}

static void Swap()
{
    static Panel T;
    T = Panels[0];
    Panels[0] = Panels[1];
    Panels[1] = T;
    Active = 1 - Active;
}

static void CommandMenu();

// --- keys ------------------------------------------------------------------

// The cursor onto the first name that starts with Search.
static bool SearchFor()
{
    Panel* P = Here();
    uint64_t n = Length(Search);
    for (uint32_t i = 0; i < P->Count; i++)
    {
        uint64_t k = 0;
        while (k < n && Lower(P->Items[i].Name[k]) == Lower(Search[k]))
            k++;
        if (k == n)
        {
            P->Cursor = i;
            return true;
        }
    }
    return false;
}

// One key; false when it is time to go.
static bool OnKey(const SfKey& K)
{
    Panel* P = Here();
    uint32_t Page = Rows > 8 ? Rows - 8 : 1;
    uint32_t Last = P->Count ? P->Count - 1 : 0;
    bool Shift = K.Mods & SF_MOD_SHIFT, Control = K.Mods & SF_MOD_CTRL;

    // Typing a name. The keys that mean something on their own do so only
    // at the start of one.
    if (Types(K))
    {
        uint64_t n = Length(Search);
        if (!n)
            switch (K.Char)
            {
                case ' ': Mark(true); return true;
                case '+': MarkByMask(true); return true;
                case '-': MarkByMask(false); return true;
                case '*':
                    for (uint32_t i = 0; i < P->Count; i++)
                        if (!IsUp(&P->Items[i]))
                            P->Items[i].Marked = !P->Items[i].Marked;
                    return true;
                case '=':
                    GoTo(There(), P->Path);
                    return true;
            }
        if (n + 1 < sizeof(Search))
        {
            Search[n] = K.Char;
            Search[n + 1] = '\0';
            if (!SearchFor())
                Search[n] = '\0';       // no such name: the character is not taken
        }
        return true;
    }
    if (K.Code == SF_KEY_BACKSPACE)
    {
        uint64_t n = Length(Search);
        if (n)
            Search[n - 1] = '\0';
        else if (P->Count && IsUp(&P->Items[0]))
        {
            P->Cursor = 0;
            Enter();
        }
        return true;
    }
    Search[0] = '\0';

    if (Ctrl(K, 'd'))       Volumes();
    else if (Ctrl(K, 'f'))  FindFiles();
    else if (Ctrl(K, 'q'))  QuickView = !QuickView;
    else if (Ctrl(K, 'b'))  BookmarkMenu();
    else if (Ctrl(K, 'k'))  CompareFolders();
    else if (Ctrl(K, 'u'))  Swap();
    else if (Ctrl(K, 'r'))  ReloadBoth();
    else switch (K.Code)
    {
        case SF_KEY_TAB:        Active = 1 - Active; break;
        case SF_KEY_UP:         if (P->Cursor > 0) P->Cursor--; break;
        case SF_KEY_DOWN:
            if ((K.Mods & SF_MOD_ALT))
                HistoryMenu();
            else if (P->Cursor < Last)
                P->Cursor++;
            break;
        case SF_KEY_PAGE_UP:    P->Cursor = P->Cursor > Page ? P->Cursor - Page : 0; break;
        case SF_KEY_PAGE_DOWN:  P->Cursor = P->Cursor + Page < Last ? P->Cursor + Page : Last; break;
        case SF_KEY_HOME:       P->Cursor = 0; break;
        case SF_KEY_END:        P->Cursor = Last; break;
        case SF_KEY_LEFT:       Active = 0; break;
        case SF_KEY_RIGHT:      Active = 1; break;
        case SF_KEY_ENTER:
        case SF_KEY_KP_ENTER:   Enter(); break;
        case SF_KEY_INSERT:     Mark(false); break;
        case SF_KEY_F1:         Help(); break;
        case SF_KEY_F2:         Rename(); break;
        case SF_KEY_F3:
            if (Control) SetSort(SORT_NAME);
            else         EditUnderCursor(true);
            break;
        case SF_KEY_F4:
            if (Control)    SetSort(SORT_EXTENSION);
            else if (Shift) NewFile();
            else            EditUnderCursor(false);
            break;
        case SF_KEY_F5:
            if (Control) SetSort(SORT_TIME);
            else         CopyOrMove(false);
            break;
        case SF_KEY_F6:
            if (Control) SetSort(SORT_SIZE);
            else         CopyOrMove(true);
            break;
        case SF_KEY_F7:         MakeFolder(); break;
        case SF_KEY_F8:
        case SF_KEY_DELETE:     DeleteChosen(); break;
        case SF_KEY_F9:         CommandMenu(); break;
        case SF_KEY_F10:        return false;
    }
    return true;
}

// F9: everything that has no F key of its own, by name.
static void CommandMenu()
{
    static const char* const Items[] =
    {
        "Volumes                      Ctrl+D",
        "Find files                   Ctrl+F",
        "Quick view on / off          Ctrl+Q",
        "Bookmarks                    Ctrl+B",
        "Folders been to              Alt+Down",
        "Compare the folders          Ctrl+K",
        "Swap the panels              Ctrl+U",
        "Same folder in the other     =",
        "Read the folders again       Ctrl+R",
        "Order by name                Ctrl+F3",
        "Order by extension           Ctrl+F4",
        "Order by date                Ctrl+F5",
        "Order by size                Ctrl+F6",
        "New file                     Shift+F4",
        "Mark by name                 +",
        "Unmark by name               -",
        "Turn the marks over          *",
        "Run as a program",
    };
    Entry* E = UnderCursor(Here());
    char Path[PATH_SIZE];
    switch (Menu("Commands", Items, sizeof(Items) / sizeof(Items[0]), 0))
    {
        case 0:  Volumes(); break;
        case 1:  FindFiles(); break;
        case 2:  QuickView = !QuickView; break;
        case 3:  BookmarkMenu(); break;
        case 4:  HistoryMenu(); break;
        case 5:  CompareFolders(); break;
        case 6:  Swap(); break;
        case 7:  GoTo(There(), Here()->Path); break;
        case 8:  ReloadBoth(); break;
        case 9:  SetSort(SORT_NAME); break;
        case 10: SetSort(SORT_EXTENSION); break;
        case 11: SetSort(SORT_TIME); break;
        case 12: SetSort(SORT_SIZE); break;
        case 13: NewFile(); break;
        case 14: MarkByMask(true); break;
        case 15: MarkByMask(false); break;
        case 16:
            for (uint32_t i = 0; i < Here()->Count; i++)
                if (!IsUp(&Here()->Items[i]))
                    Here()->Items[i].Marked = !Here()->Items[i].Marked;
            break;
        case 17:
            if (E && !E->Folder && Join(Path, Here()->Path, E->Name))
                RunProgram(Path);
            break;
    }
}

extern "C" SfStatus SfMain(SfApp*, SfSystem* System)
{
    Sys   = System;
    Con   = System->Console;
    Admin = System->Admin;
    if (!Admin)
    {
        Con->Print(Con, "explorer works on the whole disk, which takes the admin right: "
                        "run it as  sudo explorer\n");
        return SF_ACCESS_DENIED;
    }
    if (!StartKeys())
        return SF_OUT_OF_RESOURCES;

    Con->SetMode(Con, SF_CONSOLE_RAW);
    InitScreen();
    Repaint = Compose;

    // Where the panels were the last time.
    Copy(Panels[0].Path, "/", PATH_SIZE);
    Copy(Panels[1].Path, "/", PATH_SIZE);
    LoadConfig();
    for (uint32_t i = 0; i < 2; i++)
    {
        char Path[PATH_SIZE];
        Copy(Path, Panels[i].Path, sizeof(Path));
        GoTo(&Panels[i], Path);
    }

    for (;;)
    {
        Draw();
        if (!OnKey(GetKey()))
            break;
    }
    SaveConfig();
    Con->SetTitle(Con, "");
    Con->SetMode(Con, SF_CONSOLE_LINE);
    return SF_SUCCESS;                  // the key reader ends with the program
}
