section .text
bits 64

; Function to load IDT
global load_idt
load_idt:
    lidt [rdi]      ; rdi contains pointer to idtr structure
    ret

; Macro to save all registers
%macro SAVE_REGS 0
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
%endmacro

; Macro to restore all registers
%macro RESTORE_REGS 0
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
%endmacro

; Macro to create exception handler without error code
%macro EXCEPTION_HANDLER_NO_ERROR 1
global exception_handler_%1
exception_handler_%1:
    SAVE_REGS
    
    ; Pass exception number and stack frame pointer to C handler
    mov rdi, %1                ; Exception number
    mov rsi, rsp
    add rsi, 15*8              ; Pointer to exception stack frame (RIP, CS, RFLAGS, etc.)
    extern handle_exception
    call handle_exception
    
    RESTORE_REGS
    iretq
%endmacro

; Macro to create exception handler with error code
%macro EXCEPTION_HANDLER_WITH_ERROR 1
global exception_handler_%1
exception_handler_%1:
    ; Error code is already on stack, we need to remove it later
    SAVE_REGS
    
    ; Pass exception number and stack frame pointer to C handler
    mov rdi, %1                ; Exception number
    mov rsi, rsp
    add rsi, 15*8 + 8          ; Pointer to exception stack frame (skip error code)
    extern handle_exception
    call handle_exception
    
    RESTORE_REGS
    add rsp, 8                 ; Remove error code from stack
    iretq
%endmacro

; Generate all exception handlers
EXCEPTION_HANDLER_NO_ERROR   0    ; Divide Error - no error code
EXCEPTION_HANDLER_NO_ERROR   1    ; Debug - no error code
EXCEPTION_HANDLER_NO_ERROR   2    ; NMI - no error code
EXCEPTION_HANDLER_NO_ERROR   3    ; Breakpoint - no error code
EXCEPTION_HANDLER_NO_ERROR   4    ; Overflow - no error code
EXCEPTION_HANDLER_NO_ERROR   5    ; Bound Range Exceeded - no error code
EXCEPTION_HANDLER_NO_ERROR   6    ; Invalid Opcode - no error code
EXCEPTION_HANDLER_NO_ERROR   7    ; Device Not Available - no error code
EXCEPTION_HANDLER_WITH_ERROR 8    ; Double Fault - has error code (always 0)
EXCEPTION_HANDLER_NO_ERROR   9    ; Coprocessor Segment Overrun - no error code
EXCEPTION_HANDLER_WITH_ERROR 10   ; Invalid TSS - has error code
EXCEPTION_HANDLER_WITH_ERROR 11   ; Segment Not Present - has error code
EXCEPTION_HANDLER_WITH_ERROR 12   ; Stack Segment Fault - has error code
EXCEPTION_HANDLER_WITH_ERROR 13   ; General Protection Fault - has error code
EXCEPTION_HANDLER_WITH_ERROR 14   ; Page Fault - has error code
EXCEPTION_HANDLER_NO_ERROR   15   ; Reserved - no error code
EXCEPTION_HANDLER_NO_ERROR   16   ; x87 FPU Error - no error code
EXCEPTION_HANDLER_WITH_ERROR 17   ; Alignment Check - has error code
EXCEPTION_HANDLER_NO_ERROR   18   ; Machine Check - no error code
EXCEPTION_HANDLER_NO_ERROR   19   ; SIMD FPU Exception - no error code
EXCEPTION_HANDLER_NO_ERROR   20   ; Virtualization Exception - no error code
EXCEPTION_HANDLER_WITH_ERROR 21   ; Control Protection Exception - has error code
EXCEPTION_HANDLER_NO_ERROR   22   ; Reserved - no error code
EXCEPTION_HANDLER_NO_ERROR   23   ; Reserved - no error code
EXCEPTION_HANDLER_NO_ERROR   24   ; Reserved - no error code
EXCEPTION_HANDLER_NO_ERROR   25   ; Reserved - no error code
EXCEPTION_HANDLER_NO_ERROR   26   ; Reserved - no error code
EXCEPTION_HANDLER_NO_ERROR   27   ; Reserved - no error code
EXCEPTION_HANDLER_NO_ERROR   28   ; Hypervisor Injection Exception - no error code
EXCEPTION_HANDLER_WITH_ERROR 29   ; VMM Communication Exception - has error code
EXCEPTION_HANDLER_WITH_ERROR 30   ; Security Exception - has error code
EXCEPTION_HANDLER_NO_ERROR   31   ; Reserved - no error code