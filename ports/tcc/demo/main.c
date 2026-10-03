// demo: a project of several files for tcc in SurfaceOS.
//
//     tcc /demo          builds /demo/demo.bin
//     /demo/demo.bin     runs it

#include <sfos.h>
#include "include/window.h"

SfStatus SfMain(SfApp* App, SfSystem* Sys)
{
    Sys->Console->Print(Sys->Console, "demo: hello from main.c\n");
    ShowWindow(Sys->Console, "and from gui/window.c");
    return SF_SUCCESS;
}
