; The first instructions of every other CPU (smp.cpp). A startup IPI starts
; a CPU in real mode at the start of this page (BOOT_HEADER.ApTrampoline-
; Address, 0x8000); from here it goes to protected mode, turns on paging
; with the kernel's page tables and long mode, and calls the kernel's
; ap_main(Cpu*) on the stack the boot CPU prepared.
;
; Built as a flat binary for address 0x8000 and copied there. The boot CPU
; fills the parameter block at PARAMS before each start; the page is
; identity-mapped in the kernel's tables meanwhile, since the jump to long
; mode goes on at these low addresses.

bits 16
org 0x8000

%define PARAMS 0x8F00                   ; smp.cpp: TRAMPOLINE_PARAMS

start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    lgdt [gdt_ptr]
    mov eax, cr0
    or eax, 1                           ; PE
    mov cr0, eax
    jmp 0x08:protected

bits 32
protected:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    mov eax, cr4
    or eax, 1 << 5                      ; PAE
    mov cr4, eax
    mov eax, [PARAMS + 0]               ; the kernel's PML4 (below 4 GiB)
    mov cr3, eax

    mov ecx, 0xC0000080                 ; EFER
    rdmsr
    or eax, (1 << 8) | (1 << 11)        ; LME, and NXE: the kernel's pages use NX
    wrmsr

    mov eax, cr0
    or eax, (1 << 31) | (1 << 16)       ; PG, WP
    mov cr0, eax
    jmp 0x18:long_mode

bits 64
long_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov rsp, [PARAMS + 8]               ; its kernel stack
    mov rdi, [PARAMS + 16]              ; its Cpu
    mov rax, [PARAMS + 24]              ; ap_main
    call rax
.hang:                                  ; ap_main does not return
    cli
    hlt
    jmp .hang

align 8
gdt:
    dq 0
    dq 0x00CF9A000000FFFF               ; 0x08 32-bit code
    dq 0x00CF92000000FFFF               ; 0x10 data
    dq 0x00AF9A000000FFFF               ; 0x18 64-bit code
gdt_end:
gdt_ptr:
    dw gdt_end - gdt - 1
    dd gdt

times (PARAMS - 0x8000) - ($ - $$) db 0
params:
    dq 0                                ; +0  CR3
    dq 0                                ; +8  stack top
    dq 0                                ; +16 Cpu*
    dq 0                                ; +24 entry
