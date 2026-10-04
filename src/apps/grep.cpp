// grep: the lines that match a pattern - in files, in whole folders, or in
// what comes through a pipe (`ls | grep bin`).
//
//   grep [-i] [-v] [-n] [-c] [-l] [-w] <pattern> [file or folder ...]
//
// A folder is searched with everything in it. With no file, grep reads
// lines from a pipe, or as they are typed (Ctrl+D ends). With several
// files, or a folder, each line found starts with its file's path.
//
// The pattern: a character stands for itself, and
//   .        any character
//   [abc]    one of them; [a-z] a range; [^abc] any but them
//   * + ?    the one before: any number of times, once or more, maybe once
//   ^ $      the start, the end of the line
//   \x       x itself (\. a dot)

#include <sfos.h>
#include <string.h>
#include <stdio.h>

static SfSystem*  Sys;
static SfConsole* Con;

static bool IgnoreCase, Invert, Numbers, CountOnly, NamesOnly, Words;
static bool ShowNames;              // several files: "path:" before the lines

// --- the pattern -----------------------------------------------------------

enum Repeat : uint8_t { ONCE, ANY, SOME, MAYBE };     // -, *, +, ?

struct Item
{
    uint8_t Set[32];                // the characters it takes, a bit each
    Repeat  Times;
};

static const uint32_t MAX_ITEMS = 128;
static Item     Items[MAX_ITEMS];
static uint32_t ItemCount;
static bool     AtStart, AtEnd;     // ^ and $

static void SetBit(Item* I, uint8_t C)
{
    I->Set[C / 8] |= (uint8_t)(1 << (C % 8));
}

// C into I's set - with -i in either case.
static void Add(Item* I, uint8_t C)
{
    SetBit(I, C);
    if (IgnoreCase && C >= 'a' && C <= 'z')
        SetBit(I, (uint8_t)(C - 'a' + 'A'));
    if (IgnoreCase && C >= 'A' && C <= 'Z')
        SetBit(I, (uint8_t)(C - 'A' + 'a'));
}

static bool Takes(const Item* I, char C)
{
    uint8_t U = (uint8_t)C;
    return I->Set[U / 8] & (1 << (U % 8));
}

// Pattern into Items: false (and why, into Why) when it is malformed.
static bool Compile(const char* P, const char** Why)
{
    ItemCount = 0;
    AtStart = *P == '^';
    if (AtStart)
        P++;
    while (*P)
    {
        if (*P == '$' && !P[1])
        {
            AtEnd = true;
            break;
        }
        if (ItemCount == MAX_ITEMS)
        {
            *Why = "the pattern is too long";
            return false;
        }
        Item* I = &Items[ItemCount++];
        memset(I, 0, sizeof(*I));
        if (*P == '.')
        {
            memset(I->Set, 0xFF, sizeof(I->Set));
            P++;
        }
        else if (*P == '[')
        {
            P++;
            bool Not = *P == '^';
            if (Not)
                P++;
            const char* First = P;
            while (*P && (*P != ']' || P == First))
            {
                uint8_t From = (uint8_t)*P;
                if (P[1] == '-' && P[2] && P[2] != ']')
                {
                    for (uint32_t C = From; C <= (uint8_t)P[2]; C++)
                        Add(I, (uint8_t)C);
                    P += 3;
                }
                else
                    Add(I, (uint8_t)*P++);
            }
            if (*P != ']')
            {
                *Why = "a [ without its ]";
                return false;
            }
            P++;
            if (Not)
                for (uint32_t k = 0; k < sizeof(I->Set); k++)
                    I->Set[k] = (uint8_t)~I->Set[k];
        }
        else
        {
            if (*P == '\\' && P[1])
                P++;
            else if (*P == '*' || *P == '+' || *P == '?')
            {
                *Why = "a * + or ? with nothing before it";
                return false;
            }
            Add(I, (uint8_t)*P++);
        }
        if (*P == '*' || *P == '+' || *P == '?')
            I->Times = *P == '*' ? ANY : *P == '+' ? SOME : MAYBE;
        if (I->Times != ONCE)
            P++;
    }
    return true;
}

static bool WordChar(char C)
{
    return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') ||
           C == '_';
}

// Do Items[i..] match Line from At on (Len its length)? With -w the match
// has to end where a word does.
static bool MatchHere(uint32_t i, const char* Line, uint64_t At, uint64_t Len)
{
    if (i == ItemCount)
    {
        if (AtEnd && At != Len)
            return false;
        return !Words || At == Len || !WordChar(Line[At]);
    }
    const Item* I = &Items[i];
    if (I->Times == ONCE)
        return At < Len && Takes(I, Line[At]) && MatchHere(i + 1, Line, At + 1, Len);

    // As many as it takes, then back off one at a time.
    uint64_t Max = I->Times == MAYBE ? 1 : Len - At;
    uint64_t n = 0;
    while (n < Max && At + n < Len && Takes(I, Line[At + n]))
        n++;
    uint64_t Min = I->Times == SOME ? 1 : 0;
    for (;; n--)
    {
        if (n >= Min && MatchHere(i + 1, Line, At + n, Len))
            return true;
        if (n == 0)
            return false;
    }
}

static bool Matches(const char* Line, uint64_t Len)
{
    for (uint64_t At = 0; At <= Len; At++)
    {
        if (!Words || At == 0 || !WordChar(Line[At - 1]))
            if (MatchHere(0, Line, At, Len))
                return true;
        if (AtStart)
            break;
    }
    return false;
}

// --- output ----------------------------------------------------------------

static void PrintNumber(uint64_t Value)
{
    char Text[24];
    snprintf(Text, sizeof(Text), "%llu", (unsigned long long)Value);
    Con->Print(Con, Text);
}

// Len characters of Text: a NUL among them shows as a space.
static void PrintPart(const char* Text, uint64_t Len)
{
    char Chunk[257];
    while (Len)
    {
        uint64_t n = Len < 256 ? Len : 256;
        for (uint64_t k = 0; k < n; k++)
            Chunk[k] = Text[k] ? Text[k] : ' ';
        Chunk[n] = '\0';
        Con->Print(Con, Chunk);
        Text += n;
        Len  -= n;
    }
}

// --- searching -------------------------------------------------------------

// One source of lines, searched: what was found in it so far.
struct Search
{
    const char* Path;               // as it is shown; null for the pipe
    uint64_t    Line;               // the number of the line now
    uint64_t    Found;
    bool        Binary;             // a NUL in it: not shown line by line
    bool        Done;               // -l: named already
};

// Line Len bytes long, the next of S.
static void Take(Search* S, const char* Line, uint64_t Len)
{
    S->Line++;
    if (S->Done || Matches(Line, Len) == Invert)
        return;
    S->Found++;
    if (NamesOnly)
    {
        Con->Print(Con, S->Path ? S->Path : "(the pipe)");
        Con->Print(Con, "\n");
        S->Done = true;
        return;
    }
    if (CountOnly || S->Binary)
        return;
    if (ShowNames && S->Path)
    {
        Con->Print(Con, S->Path);
        Con->Print(Con, ":");
    }
    if (Numbers)
    {
        PrintNumber(S->Line);
        Con->Print(Con, ":");
    }
    PrintPart(Line, Len);
    Con->Print(Con, "\n");
}

// What is said about S once it is all read.
static void Finish(Search* S)
{
    if (CountOnly)
    {
        if (ShowNames && S->Path)
        {
            Con->Print(Con, S->Path);
            Con->Print(Con, ":");
        }
        PrintNumber(S->Found);
        Con->Print(Con, "\n");
    }
    else if (S->Binary && S->Found && !NamesOnly)
    {
        Con->Print(Con, S->Path);
        Con->Print(Con, ": a binary file, it matches\n");
    }
}

static const uint64_t LINE_MAX = 4096;   // a longer line is taken in pieces
static char Chunk[64 * 1024];
static char LineBuf[LINE_MAX];

static void SearchFile(SfFile* File, const char* Path)
{
    Search S = {};
    S.Path = Path;
    uint64_t Len = 0;
    bool First = true;
    for (;;)
    {
        uint64_t Size = sizeof(Chunk);
        SfStatus Status = File->Read(File, Chunk, &Size);
        if (SF_ERROR(Status))
        {
            Con->Print(Con, "grep: ");
            Con->Print(Con, Path);
            Con->Print(Con, ": cannot read it\n");
            break;
        }
        if (!Size)
            break;
        for (uint64_t k = 0; First && k < Size && k < 4096; k++)
            S.Binary |= !Chunk[k];
        First = false;
        for (uint64_t k = 0; k < Size; k++)
        {
            char C = Chunk[k];
            if (C == '\n' || Len == LINE_MAX)
            {
                if (Len && LineBuf[Len - 1] == '\r')
                    Len--;
                Take(&S, LineBuf, Len);
                Len = 0;
                if (C == '\n')
                    continue;
            }
            LineBuf[Len++] = C;
        }
    }
    if (Len)
        Take(&S, LineBuf, Len);
    Finish(&S);
}

// Everything in folder Dir, Path its path as shown.
static void SearchFolder(SfFile* Dir, char* Path, uint64_t PathSize)
{
    uint64_t Base = strlen(Path);
    SfDirEntry Entry;
    while (Dir->ReadDir(Dir, &Entry) == SF_SUCCESS)
    {
        if (Base + 1 + strlen(Entry.Name) + 1 > PathSize)
            continue;                   // too deep to name
        snprintf(Path + Base, PathSize - Base, "%s%s", Base && Path[Base - 1] == '/' ? "" : "/",
                 Entry.Name);
        SfFile* Sub = nullptr;
        if (Dir->Open(Dir, Entry.Name, SF_FILE_READ, &Sub) != SF_SUCCESS)
        {
            Con->Print(Con, "grep: ");
            Con->Print(Con, Path);
            Con->Print(Con, ": cannot open it\n");
            continue;
        }
        if (Entry.Flags & SF_DIR_ENTRY_FOLDER)
            SearchFolder(Sub, Path, PathSize);
        else
            SearchFile(Sub, Path);
        Sub->Close(Sub);
    }
    Path[Base] = '\0';
}

// The lines of the pipe, or typed.
static void SearchInput()
{
    Search S = {};
    for (;;)
    {
        uint64_t Len = 0;
        if (SF_ERROR(Con->ReadLine(Con, LineBuf, sizeof(LineBuf), &Len)))
            break;
        Take(&S, LineBuf, Len);
    }
    Finish(&S);
}

// The options; run alone it is asked for, else something was wrong.
static SfStatus Usage(bool Asked = false)
{
    Con->Print(Con,
        "Usage: grep [-i] [-v] [-n] [-c] [-l] [-w] <pattern> [file or folder ...]\n"
        "  the lines that match, in the files, in everything in the folders, or\n"
        "  in what comes through | (or is typed, without a file)\n"
        "  -i  any case   -v  the lines that do not match   -n  their numbers\n"
        "  -c  how many   -l  only the names of the files   -w  whole words\n"
        "pattern: . any character, [abc] [a-z] [^abc] one of them / not,\n"
        "  * + ? the one before any times / once or more / maybe, ^ $ the start\n"
        "  and the end of the line, \\x x itself\n");
    return Asked ? SF_SUCCESS : SF_INVALID_PARAMETER;
}

SfStatus SfMain(SfApp* App, SfSystem* System)
{
    Sys = System;
    Con = System->Console;

    // Options first, then the pattern, then what to search.
    uint64_t i = 1;
    for (; i < App->ArgCount && App->Args[i][0] == '-' && App->Args[i][1]; i++)
        for (const char* O = App->Args[i] + 1; *O; O++)
            switch (*O)
            {
                case 'i': IgnoreCase = true; break;
                case 'v': Invert     = true; break;
                case 'n': Numbers    = true; break;
                case 'c': CountOnly  = true; break;
                case 'l': NamesOnly  = true; break;
                case 'w': Words      = true; break;
                default:  return Usage();
            }
    if (i >= App->ArgCount)
        return Usage(App->ArgCount == 1);

    const char* Why = nullptr;
    if (!Compile(App->Args[i], &Why))
    {
        Con->Print(Con, "grep: ");
        Con->Print(Con, Why);
        Con->Print(Con, "\n");
        return SF_INVALID_PARAMETER;
    }
    i++;

    if (i == App->ArgCount)
    {
        SearchInput();
        return SF_SUCCESS;
    }

    ShowNames = App->ArgCount - i > 1;
    for (; i < App->ArgCount; i++)
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
            Con->Print(Con, "grep: ");
            Con->Print(Con, App->Args[i]);
            Con->Print(Con, ": no such file or folder\n");
            continue;
        }
        static char Path[1024];
        snprintf(Path, sizeof(Path), "%s", App->Args[i]);
        if (Info.Flags & SF_DIR_ENTRY_FOLDER)
        {
            ShowNames = true;
            SearchFolder(File, Path, sizeof(Path));
        }
        else
            SearchFile(File, Path);
        File->Close(File);
    }
    return SF_SUCCESS;
}
