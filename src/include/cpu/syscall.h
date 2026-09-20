#ifndef SYSCALL_H
#define SYSCALL_H

#include "../../sdk/include/abi/syscall.h"

extern "C" void syscall_entry();

namespace syscall
{
    // Build the dispatch tables. Called once from kmain.
    void init();
}

#endif
