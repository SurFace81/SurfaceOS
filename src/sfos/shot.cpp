// shot: a screenshot (/sfos/SHOT.BIN). The kernel starts it when Print
// Screen is pressed, with the admin right, on the screen shown.
//
// It takes the picture at once, then writes it as a BMP file to
// /files/shots/shot_<date>_<time>.bmp a piece at a time, the title
// bar counting the megabytes, and says there how it went.

#include <sfos.h>
#include <stdio.h>

static SfSystem* Sys;
static SfAdmin*  Admin;

static const uint64_t PAGE  = 4096;
static const uint64_t PIECE = 256 * 1024;

static void Put16(uint8_t* P, uint32_t V)
{
    P[0] = (uint8_t)V;
    P[1] = (uint8_t)(V >> 8);
}

static void Put32(uint8_t* P, uint32_t V)
{
    Put16(P, V);
    Put16(P + 2, V >> 16);
}

static void* Pages(uint64_t Bytes)
{
    void* Address = nullptr;
    if (SF_ERROR(Sys->Memory->AllocatePages(Sys->Memory, (Bytes + PAGE - 1) / PAGE, &Address)))
        return nullptr;
    return Address;
}

// "screenshot ######.... 2.5/6.0 MB" on the title bar.
static void Progress(uint64_t Done, uint64_t Total)
{
    const uint32_t BAR = 10;
    char Bar[BAR + 1];
    uint32_t Full = (uint32_t)(Done * BAR / Total);
    for (uint32_t i = 0; i < BAR; i++)
        Bar[i] = (char)(i < Full ? 0xDB : 0xB0);    // a full block, a light shade
    Bar[BAR] = '\0';
    uint64_t D = Done * 10 / (1024 * 1024), T = Total * 10 / (1024 * 1024);
    char Text[64];
    snprintf(Text, sizeof(Text), "screenshot %s %llu.%llu/%llu.%llu MB", Bar,
             (unsigned long long)(D / 10), (unsigned long long)(D % 10),
             (unsigned long long)(T / 10), (unsigned long long)(T % 10));
    Admin->SetStatus(Admin, Text, 0);
}

static SfStatus Fail(const char* Why)
{
    char Text[64];
    snprintf(Text, sizeof(Text), "screenshot failed: %s", Why);
    Admin->SetStatus(Admin, Text, 4000);
    return SF_DEVICE_ERROR;
}

SfStatus SfMain(SfApp*, SfSystem* System)
{
    Sys   = System;
    Admin = System->Admin;
    if (!Admin)
        return SF_ACCESS_DENIED;

    // The picture first: what was on the screen when the key went down.
    uint64_t Size = 0;
    uint32_t Width = 0, Height = 0;
    Admin->CaptureScreen(Admin, nullptr, &Size, &Width, &Height);
    uint8_t* Picture = (uint8_t*)Pages(Size);
    if (!Picture)
        return Fail("no memory");
    if (SF_ERROR(Admin->CaptureScreen(Admin, Picture, &Size, &Width, &Height)))
        return Fail("no picture");

    // As a BMP: 24 bits a pixel, rows bottom up, each padded to 4 bytes.
    uint32_t Row  = (Width * 3 + 3) & ~3u;
    uint64_t File = 54 + (uint64_t)Row * Height;
    uint8_t* Bmp  = (uint8_t*)Pages(File);
    if (!Bmp)
        return Fail("no memory");
    for (uint32_t y = 0; y < Height; y++)
    {
        uint8_t* To = Bmp + 54 + (uint64_t)(Height - 1 - y) * Row;
        const uint8_t* From = Picture + (uint64_t)y * Width * 3;
        for (uint32_t i = 0; i < Row; i++)
            To[i] = i < Width * 3 ? From[i] : 0;
    }
    Bmp[0] = 'B';
    Bmp[1] = 'M';
    Put32(Bmp + 2, (uint32_t)File);
    Put32(Bmp + 10, 54);                // where the pixels start
    Put32(Bmp + 14, 40);                // BITMAPINFOHEADER
    Put32(Bmp + 18, Width);
    Put32(Bmp + 22, Height);
    Put16(Bmp + 26, 1);                 // planes
    Put16(Bmp + 28, 24);                // bits a pixel
    Put32(Bmp + 34, Row * Height);
    Put32(Bmp + 38, 2835);              // 72 dpi
    Put32(Bmp + 42, 2835);

    SfDateTime T = {};
    Sys->Time->GetTime(Sys->Time, &T);
    char Name[48], Path[96];
    snprintf(Name, sizeof(Name), "shot_%04u-%02u-%02u_%02u-%02u-%02u.bmp", T.Year, T.Month,
             T.Day, T.Hour, T.Minute, T.Second);
    snprintf(Path, sizeof(Path), "disk:/files/shots/%s", Name);

    Sys->Files->CreateDirectory(Sys->Files, "disk:/files/shots");
    SfFile* Out = nullptr;
    if (SF_ERROR(Sys->Files->Open(Sys->Files, Path,
                                  SF_FILE_WRITE | SF_FILE_CREATE | SF_FILE_TRUNCATE, &Out)))
        return Fail("cannot make the file");
    SfStatus Status = SF_SUCCESS;
    for (uint64_t Done = 0; Done < File && !SF_ERROR(Status);)
    {
        uint64_t n = File - Done < PIECE ? File - Done : PIECE;
        Status = Out->Write(Out, Bmp + Done, &n);
        Done += n;
        Progress(Done, File);
    }
    Out->Close(Out);
    if (SF_ERROR(Status))
        return Fail("cannot write it");

    char Text[64];
    snprintf(Text, sizeof(Text), "saved %s", Name);
    Admin->SetStatus(Admin, Text, 4000);
    return SF_SUCCESS;
}
