; Ring3 entry/exit for process execution.
;
; process_enter_user(entry, user_stack, arg):
;   saves kernel context, builds an iretq frame for ring 3 and jumps
;   to the app. Returns here only after process_return_to_kernel().
;
; process_return_to_kernel():
;   restores kernel data segments and the saved kernel stack, then
;   returns into run() (used by SYS_EXIT and user-mode fault handling).

section .text
bits 64

%define USER_CS 0x23        ; GDT UserData... user code selector (0x20 | RPL 3)
%define USER_DS 0x2B        ; user data selector (0x28 | RPL 3)
%define KERN_DS 0x10

global process_enter_user
process_enter_user:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [proc_saved_rsp], rsp

    mov r12, rdi            ; entry point
    mov r13, rsi            ; user stack top
    mov r14, rdx            ; arg (program_info*)

    ; iretq frame: SS, RSP, RFLAGS, CS, RIP
    push qword USER_DS      ; SS
    push r13                ; RSP
    pushfq                  ; RFLAGS
    or qword [rsp], 0x200   ; make sure IF is set in user mode
    push qword USER_CS      ; CS
    push r12                ; RIP

    ; Switch data segments to user mode
    mov ax, USER_DS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; App ABI: rdi = arg, everything else zeroed
    mov rdi, r14
    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rsi, rsi
    xor rbp, rbp
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15

    iretq

global process_return_to_kernel
process_return_to_kernel:
    ; Exceptions clear IF: make sure interrupts are on when control
    ; returns to the kernel side of run()
    sti

    ; Restore kernel data segments
    mov ax, KERN_DS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov rsp, [proc_saved_rsp]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

section .bss
proc_saved_rsp: resq 1
