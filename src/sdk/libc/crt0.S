/* _start: the SysV ABI initial process stack.
 *
 * On entry rsp points at argc (16-byte aligned):
 *
 *     argc
 *     argv[0..argc-1], NULL
 *     envp[0..], NULL
 *     auxv ..., AT_NULL
 *     strings
 *
 * Nothing has run before this, so there is no frame to restore: clear rbp,
 * pull argc/argv/envp into the argument registers, realign and call
 * __libc_start(argc, argv, envp), which never returns.
 */
    .section .text._start, "ax"
    .globl _start
    .type _start, @function
_start:
    xor     %ebp, %ebp              /* outermost frame: NULL */
    pop     %rdi                    /* argc */
    mov     %rsp, %rsi              /* argv */
    lea     8(%rsi, %rdi, 8), %rdx  /* envp = argv + (argc+1)*8 */
    and     $-16, %rsp              /* align the stack for the call */
    call    __libc_start
    hlt                             /* unreachable */
    .size _start, .-_start
