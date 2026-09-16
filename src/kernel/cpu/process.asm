; Entry into and exit from a process session.
;
; process_enter_user(cpu_context* ctx):
;   Saves the console's callee-saved registers and stack, then loads a full
;   user CPU state (struct cpu_context, see process.h) and iretq's into it.
;   Returns only through process_return_to_kernel().
;
;   `ctx` must lie on a stack that stays valid while it is popped; the caller
;   places it at the top of the process kernel stack.
;
; process_return_to_kernel():
;   Abandons whatever trap frame is on the process kernel stack, restores the
;   console stack saved above and returns from process_enter_user. Used when
;   the last process of a session is gone or the session is killed.

section .text
bits 64

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

    ; Data segments first: the pops below overwrite rax.
    mov ax, USER_DS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; cpu_context = user_regs (15 qwords, r15 first) + iret frame
    mov rsp, rdi
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq

global process_return_to_kernel
process_return_to_kernel:
    ; Traps through interrupt gates clear IF; the console runs with it set.
    sti

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
