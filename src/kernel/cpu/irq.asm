[BITS 64]

; Import symbols
extern irq_handler

; Export IRQ stubs
global irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7
global irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15

; Common IRQ stub for x86_64
irq_common_stub:
    ; Save all general purpose registers
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    
    ; Set up kernel data segments (if needed)
    ; mov ax, 0x10
    ; mov ds, ax
    ; mov es, ax
    
    ; Call the C IRQ handler
    ; RSP is already pointing to our stack frame
    mov rdi, rsp            ; Pass pointer to interrupt frame as first argument
    call irq_handler
    
    ; Restore all general purpose registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax
    
    ; Clean up error code and interrupt number
    add rsp, 16             ; Remove int_no and err_code (8 bytes each)
    
    ; Return from interrupt
    iretq

; IRQ handler macros for x86_64
%macro IRQ 2
    irq%1:
        cli
        push qword 0        ; Push dummy error code (8 bytes)
        push qword %2       ; Push interrupt number (8 bytes)
        jmp irq_common_stub
%endmacro

; Define all IRQ handlers
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