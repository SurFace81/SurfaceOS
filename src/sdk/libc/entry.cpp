#include "../include/stdlib.h"
#include "../include/stdio.h"
#include "../include/abi/process.h"

// Apps may define `int main()` or `int main(int argc, char** argv)`: main is
// never name-mangled, and passing arguments a function ignores is harmless in
// the SysV calling convention.
extern int main(int argc, char** argv);

extern "C" void _start(program_info* info)
{
    heap_init((void*)info->heap_start, info->heap_size);
    exit(main((int)info->argc, info->argv));
}
