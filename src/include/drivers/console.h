#ifndef CONSOLE_H
#define CONSOLE_H

#include "keyboard.h"
#include "screen.h"
#include "../stdlib/list.h"
#include "../mm/memory.h"
#include "../cpu/cpuid.h"
#include "../cpu/pci.h"
#include "usb/xhci.h"

namespace console {
    void init(void);
}

#endif