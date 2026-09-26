#ifndef COMMANDS_H
#define COMMANDS_H

#include "console.h"

namespace commands
{
    void init();

    // Run a program and wait for it: argv[0] is a path, or a bare name
    // looked up in /apps. false when nothing could be loaded.
    bool run_app(int argc, const char** argv);
} // namespace commands

#endif