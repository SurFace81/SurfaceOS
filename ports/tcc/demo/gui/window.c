// The rest of the window, from a file in a folder of the project.

#include "../include/window.h"

void ShowWindow(SfUi* Ui, const char* Title)
{
    SfAddLabel(Ui, 2, 2, 0, Title);
    SfSetResult(SfAddButton(Ui, 10, -1, "OK"), 1);    // Enter: SfUiRun ends
}
