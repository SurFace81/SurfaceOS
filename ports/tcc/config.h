// tcc for SurfaceOS: what tcc's configure script writes on other systems.

#define TCC_VERSION         "0.9.27"
#define TCC_TARGET_X86_64   1
// Static programs only: no dlopen, no shared libraries.
#define CONFIG_TCC_STATIC   1
// tcc's own files, {B} in the paths below: its data folder.
#define CONFIG_TCCDIR       "data:"
#define CONFIG_TCC_SYSINCLUDEPATHS  "{B}/include"
#define CONFIG_TCC_LIBPATHS         "{B}/lib"
#define CONFIG_TCC_CRTPREFIX        "{B}/lib"
