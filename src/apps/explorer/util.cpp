// explorer: text, paths, memory and the keys.

#include "explorer.h"

// --- memory ----------------------------------------------------------------

// The compiler calls these for struct copies and zeroed arrays; the editor
// moves megabytes with them, hence rep movsb.
extern "C" void* memcpy(void* Dst, const void* Src, size_t Size)
{
    void* Result = Dst;
    __asm__ volatile("rep movsb" : "+D"(Dst), "+S"(Src), "+c"(Size) : : "memory");
    return Result;
}

extern "C" void* memmove(void* Dst, const void* Src, size_t Size)
{
    uint8_t* D = (uint8_t*)Dst;
    const uint8_t* S = (const uint8_t*)Src;
    if (D <= S || D >= S + Size)
        return memcpy(Dst, Src, Size);
    D += Size - 1;                      // overlapping, Dst above: from the end
    S += Size - 1;
    __asm__ volatile("std; rep movsb; cld" : "+D"(D), "+S"(S), "+c"(Size) : : "memory");
    return Dst;
}

extern "C" void* memset(void* Dst, int Value, size_t Size)
{
    void* Result = Dst;
    __asm__ volatile("rep stosb" : "+D"(Dst), "+c"(Size) : "a"(Value) : "memory");
    return Result;
}

void* Alloc(uint64_t Size)
{
    void* Buffer = nullptr;
    if (SF_ERROR(Sys->Memory->Allocate(Sys->Memory, Size ? Size : 1, &Buffer)))
        return nullptr;
    return Buffer;
}

void Release(void* Buffer)
{
    if (Buffer)
        Sys->Memory->Free(Sys->Memory, Buffer);
}

// --- text ------------------------------------------------------------------

uint64_t Length(const char* S)
{
    uint64_t n = 0;
    while (S[n])
        n++;
    return n;
}

void Copy(char* Dst, const char* Src, uint64_t Size)
{
    uint64_t i = 0;
    for (; Src[i] && i + 1 < Size; i++)
        Dst[i] = Src[i];
    Dst[i] = '\0';
}

char* Append(char* Out, const char* S)
{
    while (*S)
        *Out++ = *S++;
    *Out = '\0';
    return Out;
}

char* Number(char* Out, uint64_t Value, uint32_t Width, char Pad)
{
    char Digits[24];
    uint32_t n = 0;
    do
    {
        Digits[n++] = (char)('0' + Value % 10);
        Value /= 10;
    } while (Value);
    for (uint32_t i = n; i < Width; i++)
        *Out++ = Pad;
    while (n)
        *Out++ = Digits[--n];
    *Out = '\0';
    return Out;
}

char* HexNumber(char* Out, uint64_t Value, uint32_t Digits)
{
    for (uint32_t i = Digits; i > 0; i--)
        *Out++ = "0123456789ABCDEF"[(Value >> ((i - 1) * 4)) & 15];
    *Out = '\0';
    return Out;
}

// Bytes as they are up to 9999999, then in K, M or G.
char* SizeText(char* Out, uint64_t Bytes)
{
    if (Bytes < 10000000)
        return Number(Out, Bytes);
    if (Bytes < 10000000ULL * 1024)
        return Append(Number(Out, Bytes / 1024), " K");
    if (Bytes < 10000000ULL * 1024 * 1024)
        return Append(Number(Out, Bytes / (1024 * 1024)), " M");
    return Append(Number(Out, Bytes / (1024 * 1024 * 1024)), " G");
}

char Lower(char C)
{
    return C >= 'A' && C <= 'Z' ? (char)(C + 32) : C;
}

bool Same(const char* A, const char* B)
{
    while (*A && *A == *B)
        A++, B++;
    return *A == *B;
}

int CompareNoCase(const char* A, const char* B)
{
    while (*A && Lower(*A) == Lower(*B))
        A++, B++;
    return (int)(uint8_t)Lower(*A) - (int)(uint8_t)Lower(*B);
}

bool Match(const char* Mask, const char* Name)
{
    for (; *Mask; Mask++, Name++)
    {
        if (*Mask == '*')
        {
            while (*Mask == '*')
                Mask++;
            if (!*Mask)
                return true;
            for (; *Name; Name++)
                if (Match(Mask, Name))
                    return true;
            return false;
        }
        if (!*Name || (*Mask != '?' && Lower(*Mask) != Lower(*Name)))
            return false;
    }
    return !*Name;
}

const char* Why(SfStatus Status)
{
    switch (Status)
    {
        case SF_NOT_FOUND:          return "not found";
        case SF_ACCESS_DENIED:      return "not allowed";
        case SF_OUT_OF_RESOURCES:   return "no space or memory left";
        case SF_ALREADY_EXISTS:     return "it exists already";
        case SF_IN_USE:             return "in use";
        case SF_DEVICE_ERROR:       return "the device failed";
        case SF_INVALID_PARAMETER:  return "a bad name";
        case SF_UNSUPPORTED:        return "not supported";
        default:                    return "it failed";
    }
}

// --- paths -----------------------------------------------------------------

bool Join(char* Out, const char* Folder, const char* Name)
{
    uint64_t a = Length(Folder), b = Length(Name);
    if (a + b + 2 > PATH_SIZE)
    {
        Out[0] = '\0';
        return false;
    }
    char* p = Append(Out, Folder);
    if (a != 1)                         // "/" has its slash already
        *p++ = '/';
    Append(p, Name);
    return true;
}

// Path, taken from Base, as a clean absolute path: ".", ".." and double
// slashes resolved.
void Absolute(const char* Base, const char* Path, char* Out)
{
    char Buffer[PATH_SIZE * 2];
    uint64_t n = 0;
    if (Path[0] != '/')
    {
        for (const char* c = Base; *c && n < sizeof(Buffer) - 2; c++)
            Buffer[n++] = *c;
        Buffer[n++] = '/';
    }
    for (const char* c = Path; *c && n < sizeof(Buffer) - 1; c++)
        Buffer[n++] = *c;
    Buffer[n] = '\0';

    uint64_t o = 0;
    Out[o++] = '/';
    for (char* p = Buffer; *p;)
    {
        while (*p == '/')
            p++;
        char* Part = p;
        while (*p && *p != '/')
            p++;
        uint64_t Len = (uint64_t)(p - Part);
        if (Len == 0 || (Len == 1 && Part[0] == '.'))
            continue;
        if (Len == 2 && Part[0] == '.' && Part[1] == '.')
        {
            while (o > 1 && Out[o - 1] != '/')
                o--;
            if (o > 1)
                o--;
            continue;
        }
        if (o > 1 && o < PATH_SIZE - 1)
            Out[o++] = '/';
        for (uint64_t i = 0; i < Len && o < PATH_SIZE - 1; i++)
            Out[o++] = Part[i];
    }
    Out[o] = '\0';
}

void ParentOf(const char* Path, char* Out)
{
    Absolute(Path, "..", Out);
}

const char* NameOf(const char* Path)
{
    const char* Name = Path;
    for (const char* c = Path; *c; c++)
        if (*c == '/')
            Name = c + 1;
    return Name;
}

bool Inside(const char* Path, const char* Folder)
{
    uint64_t n = Length(Folder);
    if (n == 1)
        return true;                    // everything is inside "/"
    for (uint64_t i = 0; i < n; i++)
        if (Path[i] != Folder[i])
            return false;
    return Path[n] == '\0' || Path[n] == '/';
}

void DiskPath(const char* Path, char* Out)
{
    Copy(Out, "disk:", 8);
    Copy(Out + 5, Path, PATH_SIZE);
}

SfStatus Open(const char* Path, uint64_t Mode, SfFile** Out)
{
    char Full[PATH_SIZE + 8];
    DiskPath(Path, Full);
    return Sys->Files->Open(Sys->Files, Full, Mode, Out);
}

bool IsFolder(SfFile* File)
{
    SfDirEntry Info;
    return File->GetInfo(File, &Info) == SF_SUCCESS && (Info.Flags & SF_DIR_ENTRY_FOLDER);
}

// --- keys ------------------------------------------------------------------

SfKey GetKey()
{
    SfKey Key;
    while (SF_ERROR(Con->ReadKey(Con, &Key)))
        Sys->Time->Sleep(Sys->Time, 100);       // a program we started has the keys
    return Key;
}
