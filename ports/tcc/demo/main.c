// demo: a project of several files for tcc in SurfaceOS, a window of sfui.
//
//     tcc /demo          builds /demo/demo.bin
//     /demo/demo.bin     runs it; Enter or Esc closes it

#include <sfos.h>
#include <sfui.h>
#include "include/window.h"

SfStatus SfMain(SfApp* App, SfSystem* Sys)
{
    SfUi* Ui = SfUiOpen(Sys);
    SfAddWindow(Ui, "demo", 30, 7);
    SfAddLabel(Ui, 2, 1, 0, "hello from main.c");
    ShowWindow(Ui, "and from gui/window.c");
    SfUiRun(Ui);
    SfUiClose(Ui);
    return SF_SUCCESS;
}
