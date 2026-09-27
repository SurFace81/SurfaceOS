#ifndef REPORTS_H
#define REPORTS_H

// What the kernel has to say about the machine, as text (SfAdmin Report).

namespace reports
{
    // Print the report on `topic` - its name and arguments ("lspci",
    // "usbinfo 0", ...; sfos/admin.h lists them). false for an unknown one.
    bool report(const char* topic);
}

#endif // REPORTS_H
