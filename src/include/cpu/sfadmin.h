#ifndef SFADMIN_H
#define SFADMIN_H

// What a program with the admin right can do through the SurfaceOS SDK
// (SfAdmin, sfos/admin.h): the running programs, ending one, mounting and
// unmounting volumes, restarting and powering off.

namespace sfadmin
{
    // Register the SFCALL_ADMIN_* handlers.
    void init();
}

#endif // SFADMIN_H
