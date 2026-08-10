#include "../include/stdlib.h"
#include "../include/stdio.h"
#include "../include/abi/program.h"

extern int main(int argc, char** argv);

extern "C" void _start(program_info* info)
{
    heap_init((void*)info->heap_start, info->heap_size);

    char* argv[PROGRAM_MAX_ARGS];
    for (int i = 0; i < info->argc; i++)
        argv[i] = info->argv[i];

    int code = main(info->argc, argv);
    exit(code);
}