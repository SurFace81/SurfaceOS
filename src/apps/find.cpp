// find: the files and folders in a folder - and in the folders in it - that
// pass every test given; their paths, one a line.
//
//   find <folder ...> [-name <pattern>] [-type f|d] [-size [+|-]<n>[k|m]]
//                     [-mtime [+|-]<days>] [-maxdepth <n>] [-empty]
//
//   -name     the name fits the pattern: * any characters, ? one, [abc]
//             one of them ([a-z] a range); any case, as the disk has it
//   -type     f a file, d a folder
//   -size     bytes (k KiB, m MiB): +n more, -n less, n exactly
//   -mtime    changed days ago: -n in the last n days, +n longer ago
//   -maxdepth no deeper than n folders down (0: the folder named only)
//   -empty    an empty file, or a folder with nothing in it
//
// The folder named is one of what it lists, at depth 0.

#include <sfos.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static SfSystem*  Sys;
static SfConsole* Con;

// The tests; unset ones pass everything.
static const char* Name;
static char        Type;                // 'f', 'd' or 0
static int         SizeCmp, AgeCmp;     // -1 less, 0 exactly, +1 more; 2 unset
static uint64_t    SizeBytes, AgeDays;
static uint64_t    MaxDepth = ~0ULL;
static bool        Empty;
static uint64_t    Today;               // days since 1970, from the clock

static char Lower(char C)
{
    return C >= 'A' && C <= 'Z' ? (char)(C - 'A' + 'a') : C;
}

// Does Text fit the wildcard pattern P?
static bool Fits(const char* P, const char* Text)
{
    for (; *P; P++, Text++)
    {
        if (*P == '*')
        {
            for (const char* T = Text;; T++)
            {
                if (Fits(P + 1, T))
                    return true;
                if (!*T)
                    return false;
            }
        }
        if (!*Text)
            return false;
        if (*P == '?')
            continue;
        if (*P == '[')
        {
            const char* Q = P + 1;
            bool In = false;
            for (; *Q && *Q != ']'; Q++)
            {
                if (Q[1] == '-' && Q[2] && Q[2] != ']')
                {
                    In |= Lower(*Text) >= Lower(*Q) && Lower(*Text) <= Lower(Q[2]);
                    Q += 2;
                }
                else
                    In |= Lower(*Text) == Lower(*Q);
            }
            if (*Q == ']')
            {
                if (!In)
                    return false;
                P = Q;
                continue;
            }
        }
        if (Lower(*P) != Lower(*Text))
            return false;
    }
    return !*Text;
}

// Days since 1 January 1970 of a date.
static uint64_t DaysOf(const SfDateTime* D)
{
    static const uint16_t Before[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    uint64_t Y = D->Year;
    uint64_t Days = (Y - 1970) * 365 + (Y - 1969) / 4 - (Y - 1901) / 100 + (Y - 1601) / 400;
    Days += Before[(D->Month - 1) % 12] + D->Day - 1;
    bool Leap = (Y % 4 == 0 && Y % 100 != 0) || Y % 400 == 0;
    if (Leap && D->Month > 2)
        Days++;
    return Days;
}

static bool Compare(int Cmp, uint64_t Value, uint64_t Wanted)
{
    return Cmp == 2 || (Cmp < 0 ? Value < Wanted : Cmp > 0 ? Value > Wanted : Value == Wanted);
}

// Does the entry pass? Dir: it is a folder, open; for -empty.
static bool Passes(const SfDirEntry* E, const char* EntryName, SfFile* Dir)
{
    bool Folder = E->Flags & SF_DIR_ENTRY_FOLDER;
    if (Name && !Fits(Name, EntryName))
        return false;
    if ((Type == 'f' && Folder) || (Type == 'd' && !Folder))
        return false;
    if (!Compare(SizeCmp, E->Size, SizeBytes))
        return false;
    uint64_t Changed = DaysOf(&E->Modified);
    if (AgeCmp != 2 && !Compare(AgeCmp, Today > Changed ? Today - Changed : 0, AgeDays))
        return false;
    if (Empty)
    {
        if (!Folder)
            return E->Size == 0;
        SfDirEntry Any;
        bool Has = Dir->ReadDir(Dir, &Any) == SF_SUCCESS;
        Dir->SetPosition(Dir, 0);
        return !Has;
    }
    return true;
}

static void Show(const char* Path)
{
    Con->Print(Con, Path);
    Con->Print(Con, "\n");
}

// Folder Dir (Path its path), Depth folders down: what is in it.
static void Walk(SfFile* Dir, char* Path, uint64_t PathSize, uint64_t Depth)
{
    if (Depth >= MaxDepth)
        return;
    uint64_t Base = strlen(Path);
    SfDirEntry Entry;
    while (Dir->ReadDir(Dir, &Entry) == SF_SUCCESS)
    {
        if (Base + 1 + strlen(Entry.Name) + 1 > PathSize)
            continue;                   // too deep to name
        snprintf(Path + Base, PathSize - Base, "%s%s", Base && Path[Base - 1] == '/' ? "" : "/",
                 Entry.Name);
        bool Folder = Entry.Flags & SF_DIR_ENTRY_FOLDER;
        SfFile* Sub = nullptr;
        if (Folder && Dir->Open(Dir, Entry.Name, SF_FILE_READ, &Sub) != SF_SUCCESS)
        {
            Con->Print(Con, "find: ");
            Con->Print(Con, Path);
            Con->Print(Con, ": cannot open it\n");
            continue;
        }
        if (Passes(&Entry, Entry.Name, Sub))
            Show(Path);
        if (Sub)
        {
            Walk(Sub, Path, PathSize, Depth + 1);
            Sub->Close(Sub);
        }
    }
    Path[Base] = '\0';
}

// "+n", "-n" or "n" with an optional unit letter: false when it is not one.
static bool ParseCount(const char* Text, int* Cmp, uint64_t* Value, bool Units)
{
    *Cmp = *Text == '+' ? 1 : *Text == '-' ? -1 : 0;
    if (*Cmp)
        Text++;
    char* End = nullptr;
    *Value = strtoull(Text, &End, 10);
    if (End == Text)
        return false;
    if (Units && (*End == 'k' || *End == 'K'))
        *Value *= 1024, End++;
    else if (Units && (*End == 'm' || *End == 'M'))
        *Value *= 1024 * 1024, End++;
    return !*End;
}

// The options; run alone it is asked for, else something was wrong.
static SfStatus Usage(bool Asked = false)
{
    Con->Print(Con,
        "Usage: find <folder ...> [tests]\n"
        "  the files and folders in the folders, and in the folders in them,\n"
        "  that pass every test:\n"
        "  -name <pattern>   the name fits: * any characters, ? one, [abc] one of them\n"
        "  -type f|d         a file / a folder\n"
        "  -size [+|-]n[k|m] bytes (KiB, MiB): more / less than n, or n\n"
        "  -mtime [+|-]days  changed: longer ago / in the last days, or that day\n"
        "  -maxdepth n       no deeper than n folders down\n"
        "  -empty            an empty file or folder\n");
    return Asked ? SF_SUCCESS : SF_INVALID_PARAMETER;
}

SfStatus SfMain(SfApp* App, SfSystem* System)
{
    Sys = System;
    Con = System->Console;
    SizeCmp = AgeCmp = 2;

    // The folders come first, then the tests.
    uint64_t Folders = 1;
    while (Folders < App->ArgCount && App->Args[Folders][0] != '-')
        Folders++;
    if (Folders == 1)
        return Usage(App->ArgCount == 1);
    for (uint64_t i = Folders; i < App->ArgCount; i++)
    {
        const char* T = App->Args[i];
        const char* V = i + 1 < App->ArgCount ? App->Args[i + 1] : nullptr;
        if (!strcmp(T, "-empty"))
        {
            Empty = true;
            continue;
        }
        if (!V)
            return Usage();
        i++;
        bool Ok = true;
        int Cmp;
        if (!strcmp(T, "-name"))
            Name = V;
        else if (!strcmp(T, "-type"))
        {
            Type = V[0];
            Ok = (Type == 'f' || Type == 'd') && !V[1];
        }
        else if (!strcmp(T, "-size"))
            Ok = ParseCount(V, &SizeCmp, &SizeBytes, true);
        else if (!strcmp(T, "-mtime"))
            Ok = ParseCount(V, &AgeCmp, &AgeDays, false);
        else if (!strcmp(T, "-maxdepth"))
            Ok = ParseCount(V, &Cmp, &MaxDepth, false) && Cmp == 0;
        else
            Ok = false;
        if (!Ok)
            return Usage();
    }
    SfDateTime Now;
    if (Sys->Time->GetTime(Sys->Time, &Now) == SF_SUCCESS)
        Today = DaysOf(&Now);

    for (uint64_t i = 1; i < Folders; i++)
    {
        char Root[24];
        snprintf(Root, sizeof(Root), "arg%llu:", (unsigned long long)i);
        SfFile* File = nullptr;
        SfDirEntry Info;
        if (Sys->Files->Open(Sys->Files, Root, SF_FILE_READ, &File) != SF_SUCCESS ||
            File->GetInfo(File, &Info) != SF_SUCCESS)
        {
            if (File)
                File->Close(File);
            Con->Print(Con, "find: ");
            Con->Print(Con, App->Args[i]);
            Con->Print(Con, ": no such file or folder\n");
            continue;
        }
        // The last part of the path is its name, for -name.
        const char* Own = App->Args[i];
        for (const char* c = App->Args[i]; *c; c++)
            if (*c == '/' && c[1])
                Own = c + 1;
        bool Folder = Info.Flags & SF_DIR_ENTRY_FOLDER;
        if (Passes(&Info, Own, Folder ? File : nullptr))
            Show(App->Args[i]);
        if (Folder)
        {
            static char Path[1024];
            snprintf(Path, sizeof(Path), "%s", App->Args[i]);
            Walk(File, Path, sizeof(Path), 0);
        }
        File->Close(File);
    }
    return SF_SUCCESS;
}
