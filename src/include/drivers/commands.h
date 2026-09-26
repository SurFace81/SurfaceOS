#ifndef COMMANDS_H
#define COMMANDS_H

#include "console.h"

namespace commands
{
    void init();

    // Run a program and wait for it: argv[0] is a path, or a bare name
    // looked up in /apps. Arguments that name files or folders become the
    // program's argN: roots (see open_arg in commands.cpp). false when
    // there is no such program.
    bool run_app(int argc, const char** argv);
} // namespace commands

#endif