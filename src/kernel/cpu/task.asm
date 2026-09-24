; Task switching (see task.h).
;
; task_switch(uint64_t* save_rsp, uint64_t new_rsp):
;   Pushes RFLAGS and the callee-saved registers on the current stack,
;   stores rsp in *save_rsp, then pops the same set from new_rsp and returns
;   into the other task. Interrupts are off from the pushfq on; the popfq
;   gives the resumed task its own IF back.
;
; task_kernel_start:
;   First return of a kernel task: rbx = argument, r12 = entry.
;
; task_user_start:
;   First return of a process task: rsp points at a cpu_context (user_regs
;   then the iret frame), popped exactly like the tail of syscall_entry.

section .text
bits 64

%define USER_DS 0x2B        ; user data selector (0x28 | RPL 3)

global task_switch
task_switch:
    pushfq
    cli
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp

    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    popfq
    ret

global task_kernel_start
task_kernel_start:
    mov rdi, rbx
    call r12
.hang:                      ; entry must not return
    cli
    hlt
    jmp .hang

global task_user_start
task_user_start:
    mov ax, USER_DS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

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
