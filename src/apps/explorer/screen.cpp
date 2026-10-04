// explorer: the screen, of sfui elements, and what its dialogs share.

#include "explorer.h"

SfUi*    Ui;
uint32_t Columns, Rows;

// A double frame; what is inside stays.
void Box(uint32_t X, uint32_t Y, uint32_t Width, uint32_t Height, uint8_t Color)
{
    SfPut(Ui, X, Y, SF_BOX2_TOP_LEFT, Color);
    SfFill(Ui, X + 1, Y, Width - 2, SF_BOX2_H, Color);
    SfPut(Ui, X + Width - 1, Y, SF_BOX2_TOP_RIGHT, Color);
    for (uint32_t y = Y + 1; y + 1 < Y + Height; y++)
    {
        SfPut(Ui, X, y, SF_BOX2_V, Color);
        SfPut(Ui, X + Width - 1, y, SF_BOX2_V, Color);
    }
    SfPut(Ui, X, Y + Height - 1, SF_BOX2_BOTTOM_LEFT, Color);
    SfFill(Ui, X + 1, Y + Height - 1, Width - 2, SF_BOX2_H, Color);
    SfPut(Ui, X + Width - 1, Y + Height - 1, SF_BOX2_BOTTOM_RIGHT, Color);
}

// What went wrong goes under the line, in the colours of an error.
void Message(const char* Title, const char* Line, SfStatus Status)
{
    SfMessage(Ui, Title, Line, SF_ERROR(Status) ? Why(Status) : nullptr, SF_ERROR(Status));
}

static SfElement* Job;              // the progress window, while a job runs

void Progress(const char* Title, const char* Line, int Percent, bool Now)
{
    if (!Job)
        Job = SfProgress(Ui, Title);
    else if (!Same(SfGetText(Job), Title))
        SfSetText(Job, Title);
    SfSetProgress(Job, Line, Percent, Now);
}

void EndProgress()
{
    SfRemove(Job);
    Job = nullptr;
}
