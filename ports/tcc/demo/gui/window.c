// A frame around a title, from a file in a folder of the project.

#include "../include/window.h"
#include <stdio.h>
#include <string.h>

void ShowWindow(SfConsole* Con, const char* Title)
{
    char Line[80];
    int  Width = (int)strlen(Title) + 2;
    if (Width > 60)
        Width = 60;

    memset(Line, '-', Width);
    snprintf(Line + Width, sizeof(Line) - Width, "+\n");
    Con->Print(Con, "+");
    Con->Print(Con, Line);
    snprintf(Line, sizeof(Line), "| %.*s |\n", Width - 2, Title);
    Con->Print(Con, Line);
    Con->Print(Con, "+");
    memset(Line, '-', Width);
    snprintf(Line + Width, sizeof(Line) - Width, "+\n");
    Con->Print(Con, Line);
}
