#ifndef SFOS_H
#define SFOS_H

// The SurfaceOS SDK. A program includes this one header and implements
//
//     SfStatus SfMain(SfApp* App, SfSystem* Sys);
//
// Everything it can do goes through the tables reachable from Sys; objects
// are protocols called with themselves as the first argument:
//
//     Sys->Console->Print(Sys->Console, "hello\n");
//
// No libc and no call numbers: the kernel fills the tables in.

#include "sfos/status.h"
#include "sfos/table.h"
#include "sfos/app.h"
#include "sfos/console.h"
#include "sfos/file.h"
#include "sfos/files.h"
#include "sfos/system.h"

#endif // SFOS_H
