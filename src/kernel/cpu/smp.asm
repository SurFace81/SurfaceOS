; The other CPUs' first code (ap_trampoline.asm), built as a flat binary
; for its low page; smp.cpp copies it there.

section .rodata
align 16

global ap_trampoline_start
global ap_trampoline_end

ap_trampoline_start:
    incbin "bin/kernel/cpu/ap_trampoline.bin"
ap_trampoline_end:
