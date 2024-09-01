#ifndef SCREEN_INIT_H
#define SCREEN_INIT_H

#include "../../kernel.h"
#include "../screen.h"

#ifdef __cplusplus
extern "C" {
#endif

void initScreen(SYSTEM_SCREEN* Screen, SURFOS_BOOT_HEADER* Header) {
    Screen->BufferAddress = Header->FrameBufferAddress;
    Screen->BufferSize = Header->FrameBufferSize;
    Screen->CursorPositionX = 0;
    Screen->CursorPositionY = 0;
    Screen->FontPtr = Header->StandartFontBuffer;
    Screen->Height = Header->ScreenHeight;
    Screen->Width = Header->ScreenWidth;
    Screen->NumberOfSymbols = Header->FontNumberOfSymbols;
    Screen->PixelsPerScanLine = Header->ScreenPixelsPerScanLine;
    Screen->SymbolSizeX = Header->FontSymbolSizeX;
    Screen->SymbolSizeY = Header->FontSymbolSizeY;
    Screen->TextColor = 0x9E9E9EA8;     // - Gray color, (0x0000FFFF - Green; 0x00FF00FF - Red; 0xFF0000FF - Blue)

    clear_screen();
}

#ifdef __cplusplus
}
#endif

#endif