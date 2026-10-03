// hello: the smallest interactive SurfaceOS program.
//
// Asks for a name with Console->ReadLine and greets it with Console->Print.
// Ctrl+D on an empty line or Ctrl+C ends it with that status.

#include <sfos.h>

SfStatus SfMain(SfApp* App, SfSystem* Sys)
{
    SfConsole* Con = Sys->Console;

    Con->Print(Con, "Hello from ");
    Con->Print(Con, App->Name);
    Con->Print(Con, "! What is your name? ");

    char     Name[64];
    SfStatus Status = Con->ReadLine(Con, Name, sizeof(Name), nullptr);
    if (SF_ERROR(Status))
        return Status;

    Con->Print(Con, "Hello, ");
    Con->Print(Con, Name[0] ? Name : "stranger");
    Con->Print(Con, "!\n");
    return SF_SUCCESS;
}
