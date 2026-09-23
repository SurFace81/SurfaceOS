; Kernel entry point.
;
; The UEFI loader copies kernel.bin to physical 0x200000 and jumps to its
; first byte with RDI = physical BOOT_HEADER* (SysV ABI), on the firmware's
; own stack and page tables. The kernel is linked at KERNEL_VMA + phys
; (see linker.ld), so before any C++ code runs:
;
;   1. Low stub (.text.boot, runs at its physical address): build boot page
;      tables that map the first 1 GiB three times - identity (so the stub
;      survives its own CR3 load), direct map, kernel image - and jump to the
;      higher half. paging::init() replaces them with the final set.
;   2. Zero .bss. The kernel is a flat binary and .bss is NOLOAD, so the
;      loader never writes those bytes. Uninitialised statics would otherwise
;      start with firmware leftovers.
;   3. Switch to a kernel-owned stack and call kmain(boot_header_phys).

[bits 64]

extern kmain
extern __bss_start
extern __bss_end

KERNEL_STACK_SIZE equ 64 * 1024

; Keep in sync with include/cpu/paging.h.
KERNEL_VMA          equ 0xFFFFFFFF80000000
PAGE_TABLES_PHYS    equ 0x300000
PT_BOOT_OFFSET      equ 0x30000
DIRECT_MAP_PML4     equ 273
KERNEL_PML4         equ 511
KERNEL_PDPT         equ 510

BOOT_PML4           equ PAGE_TABLES_PHYS + PT_BOOT_OFFSET
BOOT_PDPT_LO        equ BOOT_PML4 + 0x1000     ; PML4[0] and PML4[273]
BOOT_PDPT_HI        equ BOOT_PML4 + 0x2000     ; PML4[511]
BOOT_PD             equ BOOT_PML4 + 0x3000     ; 512 x 2 MiB = phys 0..1 GiB

PTE_PW              equ 0x03                   ; present | write
PTE_PW_LARGE        equ 0x83                   ; present | write | 2 MiB

section .text.boot progbits alloc exec nowrite align=16
global _start

_start:
    cli
    cld                             ; rep stosq must count upwards
    mov r15, rdi                    ; BOOT_HEADER*, rep stosq clobbers rdi

    ; Zero the four boot tables.
    mov edi, BOOT_PML4
    mov ecx, 4 * 4096 / 8
    xor eax, eax
    rep stosq

    ; PML4 -> PDPTs. The direct map's first PDPT slot is index 0, the same
    ; as the identity map's, so both PML4 entries share BOOT_PDPT_LO.
    mov eax, BOOT_PDPT_LO | PTE_PW
    mov [BOOT_PML4], rax
    mov [BOOT_PML4 + DIRECT_MAP_PML4 * 8], rax
    mov eax, BOOT_PDPT_HI | PTE_PW
    mov [BOOT_PML4 + KERNEL_PML4 * 8], rax

    ; PDPTs -> the one PD.
    mov eax, BOOT_PD | PTE_PW
    mov [BOOT_PDPT_LO], rax
    mov [BOOT_PDPT_HI + KERNEL_PDPT * 8], rax

    ; PD: phys 0..1 GiB in 2 MiB pages.
    mov edi, BOOT_PD
    mov eax, PTE_PW_LARGE
    mov ecx, 512
.fill_pd:
    mov [rdi], rax
    add rax, 0x200000
    add rdi, 8
    dec ecx
    jnz .fill_pd

    mov eax, BOOT_PML4
    mov cr3, rax

    mov rax, higher_half
    jmp rax

section .text

higher_half:
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

    mov rdi, r15                    ; physical: kmain maps it itself
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
