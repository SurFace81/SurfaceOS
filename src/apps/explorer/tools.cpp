// explorer: what goes beyond copying and editing - the volumes, finding
// files, bookmarks, comparing the two folders, the quick view, running a
// program - and what is kept from one run to the next.

#include "explorer.h"

static Panel* Here() { return &Panels[Active]; }

// --- kept between runs -----------------------------------------------------

// data:/explorer.cfg, a line each: left=<folder>, right=<folder>,
// mark=<folder> for every bookmark.
static const char* const CONFIG = "data:/explorer.cfg";

char     Bookmarks[32][PATH_SIZE];
uint32_t BookmarkCount;

static bool StartsWith(const char* Line, const char* Key, const char** Rest)
{
    while (*Key)
        if (*Line++ != *Key++)
            return false;
    *Rest = Line;
    return true;
}

void LoadConfig()
{
    static char Buffer[32 * 1024];
    SfFile* File;
    if (SF_ERROR(Sys->Files->Open(Sys->Files, CONFIG, SF_FILE_READ, &File)))
        return;
    uint64_t Size = sizeof(Buffer) - 1;
    if (SF_ERROR(File->Read(File, Buffer, &Size)))
        Size = 0;
    File->Close(File);
    Buffer[Size] = '\0';

    for (char* p = Buffer; *p;)
    {
        char* Line = p;
        while (*p && *p != '\n')
            p++;
        if (*p)
            *p++ = '\0';
        const char* Rest;
        if (StartsWith(Line, "left=", &Rest))
            Copy(Panels[0].Path, Rest, PATH_SIZE);
        else if (StartsWith(Line, "right=", &Rest))
            Copy(Panels[1].Path, Rest, PATH_SIZE);
        else if (StartsWith(Line, "mark=", &Rest) && BookmarkCount < 32)
            Copy(Bookmarks[BookmarkCount++], Rest, PATH_SIZE);
    }
}

void SaveConfig()
{
    static char Buffer[34 * (PATH_SIZE + 8)];
    char* p = Append(Buffer, "left=");
    p = Append(p, Panels[0].Path);
    p = Append(p, "\nright=");
    p = Append(p, Panels[1].Path);
    p = Append(p, "\n");
    for (uint32_t i = 0; i < BookmarkCount; i++)
    {
        p = Append(p, "mark=");
        p = Append(p, Bookmarks[i]);
        p = Append(p, "\n");
    }
    SfFile* File;
    if (SF_ERROR(Sys->Files->Open(Sys->Files, CONFIG,
                                  SF_FILE_WRITE | SF_FILE_CREATE | SF_FILE_TRUNCATE, &File)))
        return;
    uint64_t Size = (uint64_t)(p - Buffer);
    File->Write(File, Buffer, &Size);
    File->Close(File);
}

// --- bookmarks -------------------------------------------------------------

void BookmarkMenu()
{
    for (;;)
    {
        const char* Items[32];
        for (uint32_t i = 0; i < BookmarkCount; i++)
            Items[i] = Bookmarks[i];
        SfKey Other;
        int i = SfMenu(Ui, "Bookmarks - Ins: add this folder, Del: remove", Items,
                       BookmarkCount, 0, &Other);
        if (i < 0)
            return;
        if (Other.Code == 0)
        {
            GoTo(Here(), Bookmarks[i]);
            return;
        }
        if (Other.Code == SF_KEY_INSERT && BookmarkCount < 32)
        {
            bool Have = false;
            for (uint32_t k = 0; k < BookmarkCount; k++)
                Have |= Same(Bookmarks[k], Here()->Path);
            if (!Have)
                Copy(Bookmarks[BookmarkCount++], Here()->Path, PATH_SIZE);
        }
        else if (Other.Code == SF_KEY_DELETE && (uint32_t)i < BookmarkCount)
        {
            for (uint32_t k = (uint32_t)i; k + 1 < BookmarkCount; k++)
                Copy(Bookmarks[k], Bookmarks[k + 1], PATH_SIZE);
            BookmarkCount--;
        }
        SaveConfig();
    }
}

// --- volumes ---------------------------------------------------------------

// The disks and their partitions as the kernel reports them ("lsblk"), and
// where each is mounted ("mount").
struct Volume
{
    char Name[32];
    char Info[32];              // its size, "57 GB"
    char Path[128];             // where it is mounted, empty when it is not
    bool Partition;
};

static Volume   Vols[32];
static uint32_t VolCount;

static void ReadVolumes()
{
    static char Report[16 * 1024];
    VolCount = 0;

    uint64_t Size = sizeof(Report);
    Report[0] = '\0';
    Admin->Report(Admin, "lsblk", Report, &Size);
    for (char* p = Report; *p && VolCount < 32;)
    {
        char* Line = p;
        while (*p && *p != '\n')
            p++;
        if (*p)
            *p++ = '\0';
        while (*Line == ' ' || *Line == '\r')
            Line++;
        Volume* V = &Vols[VolCount];
        memset(V, 0, sizeof(Volume));
        // A partition comes after a branch of the tree: two box characters.
        if ((uint8_t)Line[0] >= 0x80)
        {
            V->Partition = true;
            while ((uint8_t)*Line >= 0x80 || *Line == ' ')
                Line++;
        }
        uint32_t n = 0;
        for (; *Line && *Line != ' ' && *Line != '\r' && n + 1 < sizeof(V->Name); Line++)
            V->Name[n++] = *Line;
        if (!n)
            continue;
        const char* Rest;
        for (; *Line; Line++)
            if (StartsWith(Line, "size=", &Rest))
            {
                n = 0;
                for (; *Rest && *Rest != '\r' && !(Rest[0] == ' ' && Rest[1] == ' ') &&
                       n + 1 < sizeof(V->Info); Rest++)
                    V->Info[n++] = *Rest;
                break;
            }
        if (V->Info[0])                 // anything else is not a device row
            VolCount++;
    }

    // "  <device> on <folder>"
    Size = sizeof(Report);
    Report[0] = '\0';
    Admin->Report(Admin, "mount", Report, &Size);
    for (char* p = Report; *p;)
    {
        char* Line = p;
        while (*p && *p != '\n')
            p++;
        if (*p)
            *p++ = '\0';
        while (*Line == ' ' || *Line == '\r')
            Line++;
        char* Device = Line;
        while (*Line && *Line != ' ')
            Line++;
        const char* Folder;
        if (!StartsWith(Line, " on ", &Folder))
            continue;
        *Line = '\0';
        for (uint32_t i = 0; i < VolCount; i++)
            if (Same(Vols[i].Name, Device))
            {
                Copy(Vols[i].Path, Folder, sizeof(Vols[i].Path));
                for (char* c = Vols[i].Path; *c; c++)
                    if (*c == '\r')
                        *c = '\0';
            }
    }
}

// Where volume Name is to be found: its own folder, or for a disk that of
// its first mounted partition.
static const char* VolumePath(const char* Name)
{
    for (uint32_t i = 0; i < VolCount; i++)
        if (Same(Vols[i].Name, Name) && Vols[i].Path[0])
            return Vols[i].Path;
    const char* Rest;
    for (uint32_t i = 0; i < VolCount; i++)
        if (Vols[i].Partition && Vols[i].Path[0] && StartsWith(Vols[i].Name, Name, &Rest))
            return Vols[i].Path;
    return nullptr;
}

void Volumes()
{
    uint32_t At = 0;
    for (;;)
    {
        ReadVolumes();
        static char Labels[32][192];
        const char* Items[32];
        for (uint32_t i = 0; i < VolCount; i++)
        {
            const Volume* V = &Vols[i];
            char* p = Append(Labels[i], V->Partition ? "  " : "");
            p = Append(p, V->Name);
            while (p < Labels[i] + 16)
                *p++ = ' ';
            p = Append(p, V->Info);
            while (p < Labels[i] + 28)
                *p++ = ' ';
            Append(p, V->Path[0] ? V->Path : V->Partition ? "not mounted" : "");
            Items[i] = Labels[i];
        }
        SfKey Other;
        int i = SfMenu(Ui, "Volumes - Enter: go there (mounting it), Del: unmount", Items,
                       VolCount, At, &Other);
        if (i < 0)
            return;
        At = (uint32_t)i;
        char Name[32];
        Copy(Name, Vols[i].Name, sizeof(Name));

        if (Other.Code == 0)
        {
            if (!VolumePath(Name))
            {
                SfStatus Status = Admin->Mount(Admin, Name);
                if (SF_ERROR(Status) && Status != SF_ALREADY_EXISTS)
                {
                    Message("Cannot mount it", Name, Status);
                    continue;
                }
                ReadVolumes();
            }
            const char* Path = VolumePath(Name);
            if (!Path)
            {
                Message("Volumes", "Nothing of it can be mounted");
                continue;
            }
            GoTo(Here(), Path);
            return;
        }
        if (Other.Code == SF_KEY_DELETE || Other.Code == SF_KEY_F8)
        {
            // Nobody may stand on what goes away.
            for (uint32_t k = 0; k < 2; k++)
                if (Inside(Panels[k].Path, "/mount") && Panels[k].Path[6])
                    GoTo(&Panels[k], "/");
            SfStatus Status = Admin->Unmount(Admin, Name);
            if (SF_ERROR(Status))
                Message("Cannot unmount it", Name, Status);
        }
    }
}

// --- finding files ---------------------------------------------------------

static const uint32_t MAX_FOUND = 1000;
static char*    Found[MAX_FOUND];
static uint32_t FoundCount;
static char     Mask[128] = "*";
static char     Wanted[128];

// Does the file hold Wanted, in any case? Read in pieces, the end of each
// kept for a find that lies across two.
static bool Holds(const char* Path)
{
    static uint8_t Buffer[65536 + 128];
    SfFile* File;
    if (SF_ERROR(Open(Path, SF_FILE_READ, &File)))
        return false;
    uint64_t n = Length(Wanted), Kept = 0;
    bool Result = false;
    while (!Result)
    {
        uint64_t Got = 65536;
        if (SF_ERROR(File->Read(File, Buffer + Kept, &Got)) || Got == 0)
            break;
        uint64_t Total = Kept + Got;
        for (uint64_t i = 0; i + n <= Total && !Result; i++)
        {
            uint64_t k = 0;
            while (k < n && Lower((char)Buffer[i + k]) == Lower(Wanted[k]))
                k++;
            Result = k == n;
        }
        Kept = Total < n - 1 ? Total : n - 1;
        memmove(Buffer, Buffer + Total - Kept, Kept);
    }
    File->Close(File);
    return Result;
}

// False: stopped, by Esc or by the list being full.
static bool Walk(const char* Folder)
{
    SfFile* Dir;
    if (SF_ERROR(Open(Folder, SF_FILE_READ, &Dir)))
        return true;
    Progress("Looking for files", Folder, -1);
    bool Go = true;
    SfDirEntry E;
    while (Go && Dir->ReadDir(Dir, &E) == SF_SUCCESS)
    {
        char Path[PATH_SIZE];
        if (!Join(Path, Folder, E.Name))
            continue;
        bool IsDir = E.Flags & SF_DIR_ENTRY_FOLDER;
        if (Match(Mask, E.Name) && (!Wanted[0] || (!IsDir && Holds(Path))))
        {
            char* Kept = (char*)Alloc(Length(Path) + 1);
            if (Kept)
            {
                Append(Kept, Path);
                Found[FoundCount++] = Kept;
            }
            Go = Kept && FoundCount < MAX_FOUND;
        }
        if (Go && IsDir)
            Go = Walk(Path);
        if (SfUiEscape(Ui))
            Go = false;
    }
    Dir->Close(Dir);
    return Go;
}

void FindFiles()
{
    if (!SfInput(Ui, "Find files", "Names like (* and ? stand for anything):", Mask, sizeof(Mask)) ||
        !SfInput(Ui, "Find files", "Holding the text (empty: any file):", Wanted, sizeof(Wanted)))
        return;
    if (!Mask[0])
        Copy(Mask, "*", sizeof(Mask));

    FoundCount = 0;
    Progress("Looking for files", Here()->Path, -1, true);
    Walk(Here()->Path);

    char Title[64];
    char* p = Append(Title, "Found: ");
    p = Number(p, FoundCount);
    Append(p, " - Enter goes there");
    int i = SfMenu(Ui, Title, Found, FoundCount, 0, nullptr);
    if (i >= 0)
    {
        char Folder[PATH_SIZE], Name[256];
        ParentOf(Found[i], Folder);
        Copy(Name, NameOf(Found[i]), sizeof(Name));
        GoTo(Here(), Folder, Name);
    }
    while (FoundCount)
        Release(Found[--FoundCount]);
}

// --- comparing -------------------------------------------------------------

static uint64_t Stamp(const SfDateTime* T)
{
    return (uint64_t)T->Year << 40 | (uint64_t)T->Month << 32 | (uint64_t)T->Day << 24 |
           (uint64_t)T->Hour << 16 | (uint64_t)T->Minute << 8 | T->Second;
}

// Mark in A the files B does not have, has in another size, or has older.
static uint32_t MarkDifferent(Panel* A, const Panel* B)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < A->Count; i++)
    {
        Entry* E = &A->Items[i];
        E->Marked = false;
        if (E->Folder)
            continue;
        const Entry* Twin = nullptr;
        for (uint32_t k = 0; k < B->Count && !Twin; k++)
            if (!B->Items[k].Folder && CompareNoCase(B->Items[k].Name, E->Name) == 0)
                Twin = &B->Items[k];
        E->Marked = !Twin || Twin->Size != E->Size ||
                    Stamp(&E->Modified) > Stamp(&Twin->Modified);
        n += E->Marked;
    }
    return n;
}

void CompareFolders()
{
    uint32_t n = MarkDifferent(&Panels[0], &Panels[1]);
    n += MarkDifferent(&Panels[1], &Panels[0]);
    if (!n)
        Message("Compare", "The files of the two folders are the same");
}

// --- running a program -----------------------------------------------------

// The screen goes back to a scrolling console for it, and it gets the keys;
// when it ends, a key brings the panels back.
void RunProgram(const char* Path)
{
    char Full[PATH_SIZE + 8];
    DiskPath(Path, Full);
    Con->SetMode(Con, SF_CONSOLE_LINE);
    uint64_t Handle = 0;
    SfStatus Status = Sys->Process->Start(Sys->Process, Full, 0, nullptr, nullptr,
                                          SF_START_GIVE_INPUT, &Handle);
    if (!SF_ERROR(Status))
    {
        Sys->Process->Wait(Sys->Process, Handle, nullptr);
        Con->Print(Con, "\n[it ended - a key goes back to explorer]");
        GetKey();
    }
    Con->SetMode(Con, SF_CONSOLE_RAW);
    if (SF_ERROR(Status))
        Message("Cannot run it", Path, Status);
}

// --- quick view ------------------------------------------------------------

// The start of the file under the cursor, as text or as hex, in the other
// panel. Read again only when the cursor is on another file.
void DrawQuickView(uint32_t X, uint32_t Y, uint32_t Width, uint32_t Height, const char* Folder,
                   const Entry* E)
{
    static char     Shown[PATH_SIZE];
    static uint64_t ShownSize;
    static uint8_t  Bytes[8192];
    static uint64_t Count;

    char Path[PATH_SIZE];
    if (!E || !Join(Path, Folder, E->Name))
        return;
    if (E->Folder)
    {
        SfText(Ui, X + 1, Y + 1, "A folder. Space counts what it holds.", COL_FILE);
        return;
    }
    if (!Same(Path, Shown) || ShownSize != E->Size)
    {
        Copy(Shown, Path, sizeof(Shown));
        ShownSize = E->Size;
        Count = 0;
        SfFile* File;
        if (!SF_ERROR(Open(Path, SF_FILE_READ, &File)))
        {
            Count = sizeof(Bytes);
            if (SF_ERROR(File->Read(File, Bytes, &Count)))
                Count = 0;
            File->Close(File);
        }
    }

    if (LooksBinary(Bytes, Count))
    {
        // offset, bytes, characters: as many bytes a row as fit.
        uint32_t Per = Width > 12 ? (Width - 9) / 4 : 1;
        Per = Per >= 16 ? 16 : Per >= 8 ? 8 : Per >= 4 ? 4 : 1;
        for (uint32_t r = 0; r < Height && (uint64_t)r * Per < Count; r++)
        {
            char Digits[12];
            HexNumber(Digits, (uint64_t)r * Per, 6);
            SfText(Ui, X, Y + r, Digits, COL_DIM);
            for (uint32_t i = 0; i < Per && (uint64_t)r * Per + i < Count; i++)
            {
                uint8_t B = Bytes[r * Per + i];
                HexNumber(Digits, B, 2);
                SfText(Ui, X + 8 + i * 3, Y + r, Digits, COL_FILE);
                SfPut(Ui, X + 8 + Per * 3 + 1 + i, Y + r, B >= 32 && B != 127 ? (char)B : '.',
                    COL_FOLDER);
            }
        }
        return;
    }
    uint32_t Row = 0, Col = 0;
    for (uint64_t i = 0; i < Count && Row < Height; i++)
    {
        uint8_t B = Bytes[i];
        if (B == '\n')
            Row++, Col = 0;
        else if (B == '\t')
            Col = (Col / 4 + 1) * 4;
        else if (B != '\r')
        {
            if (Col < Width)
                SfPut(Ui, X + Col, Y + Row, (char)B, COL_FOLDER);
            Col++;
        }
    }
}
