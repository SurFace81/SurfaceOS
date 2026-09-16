; Kernel entry point.
;
; The UEFI loader jumps here with RDI = BOOT_HEADER* (SysV ABI) on the
; firmware's own stack. Two things must happen before any C++ code runs:
;
;   1. Zero .bss. The kernel is a flat binary (see linker.ld) and .bss is
;      NOLOAD, so the loader never writes those bytes. Uninitialised
;      statics would otherwise start with firmware leftovers.
;   2. Switch to a kernel-owned stack. The old code did
;      `movq $0x200000, %rsp` from inside kmain, which grew down into the
;      boot header at 0x100000 and ran after GCC had already emitted the
;      function prologue on the firmware stack.

[bits 64]

extern kmain
extern __bss_start
extern __bss_end

KERNEL_STACK_SIZE equ 64 * 1024

section .text
global _start

_start:
    cld                             ; rep stosq must count upwards
    mov r15, rdi                    ; BOOT_HEADER*, rep stosq clobbers rdi

    ; Zero [__bss_start, __bss_end); the linker aligns both to 8 bytes.
    mov rdi, __bss_start
    mov rcx, __bss_end
    sub rcx, rdi
    shr rcx, 3
    xor rax, rax
    rep stosq

    ; Kernel stack (lives in the .bss we just zeroed)
    lea rsp, [rel kernel_stack_top]
    xor rbp, rbp                    ; terminate stack-frame chains

    mov rdi, r15
    call kmain

.hang:
    cli
    hlt
    jmp .hang

section .bss
align 16
global kernel_stack_bottom
kernel_stack_bottom:
    resb KERNEL_STACK_SIZE
global kernel_stack_top
kernel_stack_top:
