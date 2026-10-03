// tcc on SurfaceOS: the program's start and the system calls tcc makes
// (sfport.h).

#include "sfport.h"
#include <stdio.h>
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

// tcc's main with the command line as C has it.
SfStatus SfMain(SfApp* TheApp, SfSystem* TheSys)
{
    Sys = TheSys;
    App = TheApp;
    Sys->Files->Open(Sys->Files, "data:/", SF_FILE_READ, &Current);
    for (uint64_t i = 1; i < App->ArgCount && i < 64; i++)
    {
        char    Root[16];
        SfFile* File = NULL;
        snprintf(Root, sizeof(Root), "arg%d:", (int)i);
        ArgIsFile[i] = !SF_ERROR(Sys->Files->Open(Sys->Files, Root, SF_FILE_READ, &File));
        if (File)
            File->Close(File);
    }

    char** Argv = NULL;
    if (SF_ERROR(Sys->Memory->Allocate(Sys->Memory, (App->ArgCount + 1) * sizeof(char*),
                                       (void**)&Argv)))
        return SF_OUT_OF_RESOURCES;
    for (uint64_t i = 0; i < App->ArgCount; i++)
        Argv[i] = (char*)App->Args[i];
    return (SfStatus)main((int)App->ArgCount, Argv);
}
