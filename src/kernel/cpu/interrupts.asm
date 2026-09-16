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

; Exception stubs.
;
; Only some vectors push an error code, so the no-error variant pushes a zero
; in its place. From SAVE_REGS onwards both macros see the identical layout:
;
;   rsp + 0        saved GPRs (15 qwords)
;   rsp + 15*8     error code
;   rsp + 16*8     RIP, CS, RFLAGS, RSP, SS
;
; handle_exception(vector, frame, error_code)

%macro EXCEPTION_BODY 1
    SAVE_REGS

    mov rdi, %1                ; vector
    mov rsi, rsp
    add rsi, 16*8              ; -> RIP
    mov rdx, [rsp + 15*8]      ; error code
    extern handle_exception
    call handle_exception

    RESTORE_REGS
    add rsp, 8                 ; drop the error code
    iretq
%endmacro

; Vector without a CPU-pushed error code
%macro EXCEPTION_HANDLER_NO_ERROR 1
global exception_handler_%1
exception_handler_%1:
    push qword 0               ; placeholder error code
    EXCEPTION_BODY %1
%endmacro

; Vector with a CPU-pushed error code
%macro EXCEPTION_HANDLER_WITH_ERROR 1
global exception_handler_%1
exception_handler_%1:
    EXCEPTION_BODY %1
%endmacro

; IRQ handler macro
%macro IRQ 2
global irq%1
irq%1:
    cli
    push qword 0        ; Push dummy error code (8 bytes)
    push qword %2       ; Push interrupt number (8 bytes)
    jmp irq_common_stub
%endmacro

irq_common_stub:
    SAVE_REGS
    ; Set up kernel data segments (if needed)
    ; mov ax, 0x10
    ; mov ds, ax
    ; mov es, ax

    ; RSP is already pointing to our stack frame
    mov rdi, rsp            ; Pass pointer to interrupt frame as first argument
    extern irq_handler
    call irq_handler
    
    RESTORE_REGS    
    add rsp, 16             ; Remove int_no and err_code (8 bytes each)
    iretq

; Define IDT handlers
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

; Define IRQ handlers
IRQ  0, 32   ; Timer
IRQ  1, 33   ; Keyboard
IRQ  2, 34   ; Cascade (never raised)
IRQ  3, 35   ; COM2
IRQ  4, 36   ; COM1
IRQ  5, 37   ; LPT2
IRQ  6, 38   ; Floppy
IRQ  7, 39   ; LPT1
IRQ  8, 40   ; RTC
IRQ  9, 41   ; Free
IRQ 10, 42   ; Free
IRQ 11, 43   ; Free
IRQ 12, 44   ; PS/2 Mouse
IRQ 13, 45   ; FPU
IRQ 14, 46   ; Primary ATA
IRQ 15, 47   ; Secondary ATA

; Syscall handler (int 0x80)
global syscall_entry
syscall_entry:
    SAVE_REGS

    mov rdi, rsp
    extern syscall_dispatch
    call syscall_dispatch

    RESTORE_REGS
    iretq
