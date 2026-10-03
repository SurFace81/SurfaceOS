// tcc on SurfaceOS: the system as tcc sees it (sfport.c).
//
// tcc calls the SDK's tables through Sys directly; these helpers cover
// what takes more than one call.

#ifndef SFPORT_H
#define SFPORT_H

#include <sfos.h>

/// The tables SfMain got.
extern SfSystem* Sys;

/// Path starts with a root: "data:/x", "arg1:".
int sf_has_root(const char* Path);
/// Opens Path with SF_FILE_* Mode: a path with a root as it stands, a
/// command-line argument through the argN: root the console opened for it,
/// any other path below the current folder (data:/). NULL when it cannot.
SfFile* sf_open(const char* Path, uint64_t Mode);
/// Reads up to Size bytes at File's position: the count read, -1 on an
/// error.
long sf_read(SfFile* File, void* Buffer, unsigned long Size);
/// Writes Size bytes at File's position: 0, -1 when not all of them went.
int sf_write(SfFile* File, const void* Buffer, unsigned long Size);
/// Writes Count zero bytes at File's position, as sf_write.
int sf_write_zeros(SfFile* File, unsigned long Count);
/// File's size in bytes.
unsigned long sf_size(SfFile* File);
/// Formats as printf to File, or to the console when File is NULL.
void sf_printf(SfFile* File, const char* Format, ...);
/// Ends tcc with Status (0: success).
__attribute__((noreturn)) void sf_exit(int Status);

#endif // SFPORT_H
