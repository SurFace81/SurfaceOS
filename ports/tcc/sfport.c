// tcc on SurfaceOS: the program's start and the system calls tcc makes
// (sfport.h).
//
// `tcc <folder> [options]` builds a project: every .c file in the folder
// and the folders in it, into <folder>/<name>.bin, with the folder as the
// current one - its paths are what tcc sees, error messages included.

#include "sfport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv);           // tcc.c

SfSystem* Sys;

// The command line, and which arguments the console opened as argN:.
static const SfApp* App;
static bool         ArgIsFile[64];
// Paths without a root are below it.
static SfFile*      Current;

int sf_has_root(const char* Path)
{
    for (const char* P = Path; *P && *P != '/'; P++)
        if (*P == ':')
            return P != Path;
    return 0;
}

SfFile* sf_open(const char* Path, uint64_t Mode)
{
    SfFile* File = NULL;
    if (sf_has_root(Path))
        return SF_ERROR(Sys->Files->Open(Sys->Files, Path, Mode, &File)) ? NULL : File;
    for (uint64_t i = 1; i < App->ArgCount && i < 64; i++)
        if (ArgIsFile[i] && !strcmp(Path, App->Args[i]))
        {
            char Root[16];
            snprintf(Root, sizeof(Root), "arg%d:", (int)i);
            return SF_ERROR(Sys->Files->Open(Sys->Files, Root, Mode, &File)) ? NULL : File;
        }
    if (!Current)
        return NULL;
    return SF_ERROR(Current->Open(Current, Path, Mode, &File)) ? NULL : File;
}

long sf_read(SfFile* File, void* Buffer, unsigned long Size)
{
    uint64_t Count = Size;
    return SF_ERROR(File->Read(File, Buffer, &Count)) ? -1 : (long)Count;
}

int sf_write(SfFile* File, const void* Buffer, unsigned long Size)
{
    uint64_t Count = Size;
    return SF_ERROR(File->Write(File, Buffer, &Count)) || Count != Size ? -1 : 0;
}

int sf_write_zeros(SfFile* File, unsigned long Count)
{
    static const char Zeros[512];
    for (; Count; )
    {
        unsigned long Part = Count < sizeof(Zeros) ? Count : sizeof(Zeros);
        if (sf_write(File, Zeros, Part))
            return -1;
        Count -= Part;
    }
    return 0;
}

unsigned long sf_size(SfFile* File)
{
    SfDirEntry Info;
    return SF_ERROR(File->GetInfo(File, &Info)) ? 0 : Info.Size;
}

void sf_printf(SfFile* File, const char* Format, ...)
{
    char Text[4096];
    va_list Args;
    va_start(Args, Format);
    int Length = vsnprintf(Text, sizeof(Text), Format, Args);
    va_end(Args);
    if (Length >= (int)sizeof(Text))
        Length = sizeof(Text) - 1;
    if (File)
        sf_write(File, Text, (unsigned long)Length);
    else
        Sys->Console->Print(Sys->Console, Text);
}

void sf_exit(int Status)
{
    Sys->Process->Exit(Sys->Process, (SfStatus)Status);
    for (;;)
        ;                               // Exit does not return
}

static void* Allocate(uint64_t Size)
{
    void* Block = NULL;
    Sys->Memory->Allocate(Sys->Memory, Size, &Block);
    return Block;
}

static char* Copy(const char* Text)
{
    char* Block = Allocate(strlen(Text) + 1);
    return Block ? strcpy(Block, Text) : NULL;
}

// The .c files of a project, paths below its folder.
static char** Sources;
static int    SourceCount;

static void AddSource(const char* Path)
{
    if (SF_ERROR(Sys->Memory->Reallocate(Sys->Memory, Sources,
                                         (SourceCount + 1) * sizeof(char*), (void**)&Sources)))
        return;
    Sources[SourceCount] = Copy(Path);
    if (Sources[SourceCount])
        SourceCount++;
}

// Every .c file below Folder; Prefix is Folder's path ("", "gui/").
static void FindSources(SfFile* Folder, const char* Prefix)
{
    SfDirEntry Entry;
    while (!SF_ERROR(Folder->ReadDir(Folder, &Entry)))
    {
        char   Path[512];
        size_t Length = strlen(Entry.Name);
        snprintf(Path, sizeof(Path), "%s%s", Prefix, Entry.Name);
        if (Entry.Flags & SF_DIR_ENTRY_FOLDER)
        {
            SfFile* Inner = NULL;
            if (SF_ERROR(Folder->Open(Folder, Entry.Name, SF_FILE_READ, &Inner)))
                continue;
            strcat(Path, "/");
            FindSources(Inner, Path);
            Inner->Close(Inner);
        }
        else if (Length > 2 && Entry.Name[Length - 2] == '.' &&
                 (Entry.Name[Length - 1] == 'c' || Entry.Name[Length - 1] == 'C'))
            AddSource(Path);
    }
}

static int ComparePaths(const void* A, const void* B)
{
    return strcmp(*(char* const*)A, *(char* const*)B);
}

// The project's name: the last part of its path ("/files/demo/" -> "demo").
static void ProjectName(const char* Path, char* Name, size_t Size)
{
    const char* Start = Path;
    size_t      Length;
    for (const char* P = Path; *P; P++)
        if ((*P == '/' || *P == ':') && P[1] && P[1] != '/')
            Start = P + 1;
    for (Length = 0; Start[Length] && Start[Length] != '/'; Length++)
        ;
    if (Length == 0 || Length >= Size || !strncmp(Start, ".", Length) ||
        !strncmp(Start, "..", Length))
    {
        snprintf(Name, Size, "program");
        return;
    }
    memcpy(Name, Start, Length);
    Name[Length] = '\0';
}

// tcc <folder> [options]: the folder becomes the current one, and tcc gets
// the options, -o <name>.bin unless they have an -o, and the sources.
// Returns the new argc, or -1 when the folder has no .c file.
static int BuildProject(int Folder, SfFile* Project, char*** Argv)
{
    char Name[64], Output[80];
    ProjectName(App->Args[Folder], Name, sizeof(Name));
    snprintf(Output, sizeof(Output), "%s.bin", Name);

    FindSources(Project, "");
    if (SourceCount == 0)
    {
        sf_printf(NULL, "tcc: no .c files in %s\n", App->Args[Folder]);
        return -1;
    }
    qsort(Sources, SourceCount, sizeof(char*), ComparePaths);

    char** Args = Allocate((App->ArgCount + 2 + SourceCount + 1) * sizeof(char*));
    if (!Args)
        return -1;
    int  Count = 0;
    bool HasOutput = false;
    for (uint64_t i = 0; i < App->ArgCount; i++)
        if ((int)i != Folder)
        {
            HasOutput |= !strcmp(App->Args[i], "-o");
            Args[Count++] = (char*)App->Args[i];
        }
    if (!HasOutput)
    {
        Args[Count++] = "-o";
        Args[Count++] = Copy(Output);
    }
    for (int i = 0; i < SourceCount; i++)
        Args[Count++] = Sources[i];

    if (Current)
        Current->Close(Current);
    Current = Project;
    *Argv = Args;
    return Count;
}

// tcc's main with the command line as C has it.
SfStatus SfMain(SfApp* TheApp, SfSystem* TheSys)
{
    Sys = TheSys;
    App = TheApp;
    Sys->Files->Open(Sys->Files, "data:/", SF_FILE_READ, &Current);
    int     Folder  = 0;            // the project's argument, 0: none
    SfFile* Project = NULL;
    for (uint64_t i = 1; i < App->ArgCount && i < 64; i++)
    {
        char       Root[16];
        SfFile*    File = NULL;
        SfDirEntry Info;
        snprintf(Root, sizeof(Root), "arg%d:", (int)i);
        ArgIsFile[i] = !SF_ERROR(Sys->Files->Open(Sys->Files, Root, SF_FILE_READ, &File));
        if (File && !Folder && !SF_ERROR(File->GetInfo(File, &Info)) &&
            (Info.Flags & SF_DIR_ENTRY_FOLDER))
        {
            Folder  = (int)i;
            Project = File;
        }
        else if (File)
            File->Close(File);
    }
    if (Folder)
    {
        char** Args  = NULL;
        int    Count = BuildProject(Folder, Project, &Args);
        return Count < 0 ? (SfStatus)1 : (SfStatus)main(Count, Args);
    }

    char** Argv = NULL;
    if (SF_ERROR(Sys->Memory->Allocate(Sys->Memory, (App->ArgCount + 1) * sizeof(char*),
                                       (void**)&Argv)))
        return SF_OUT_OF_RESOURCES;
    for (uint64_t i = 0; i < App->ArgCount; i++)
        Argv[i] = (char*)App->Args[i];
    return (SfStatus)main((int)App->ArgCount, Argv);
}
