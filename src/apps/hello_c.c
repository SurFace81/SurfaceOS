// hello_c: a program in C. The same file builds as C++ unchanged: the SDK
// is one for both, bool and NULL included.
#include <sfos.h>

SF_STATIC_ASSERT(sizeof(uint64_t) == 8, "uint64_t");

static void PrintNumber(SfConsole* Con, uint64_t Value)
{
    char Text[21];
    size_t i = sizeof(Text) - 1;
    Text[i] = 0;
    do
    {
        Text[--i] = (char)('0' + Value % 10);
        Value /= 10;
    } while (Value);
    Con->Print(Con, &Text[i]);
}

SfStatus SfMain(SfApp* App, SfSystem* Sys)
{
    SfConsole* Con = Sys->Console;
    Con->Print(Con, "Hello from ");
    Con->Print(Con, App->Name);
#ifdef __cplusplus
    Con->Print(Con, ", built as C++\n");
#else
    Con->Print(Con, ", built as C\n");
#endif

    bool HasAdmin = Sys->Admin != NULL;
    Con->Print(Con, HasAdmin ? "with the admin right\n" : "without the admin right\n");

    uint64_t Uptime = 0;
    if (Sys->Time->GetUptime(Sys->Time, &Uptime) == SF_SUCCESS)
    {
        Con->Print(Con, "up for ");
        PrintNumber(Con, Uptime);
        Con->Print(Con, " ms\n");
    }

    for (uint64_t i = 1; i < App->ArgCount; i++)
    {
        Con->Print(Con, "arg: ");
        Con->Print(Con, App->Args[i]);
        Con->Print(Con, "\n");
    }

    return SF_SUCCESS;
}
