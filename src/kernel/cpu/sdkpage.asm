; The SDK code page (see sdkpage.h). These bytes are copied into every
; SurfaceOS program and run there in ring 3, never in the kernel. Nothing
; here may refer to an absolute address: only registers, the stack and
; relative jumps.
;
; Call numbers mirror abi/sfcall.h.

%define SFCALL_EXIT             0
%define SFCALL_CONSOLE_PRINT    1
%define SFCALL_CONSOLE_READLINE 2

section .rodata
bits 64

align 16
global sdk_code_start
global sdk_code_end
global sdk_start
global sdk_console_print
global sdk_console_readline

sdk_code_start:

; The process starts here: rdi = App, rsi = Sys, rdx = SfMain, rsp 16-byte
; aligned. SfMain's SfStatus becomes the exit status.
sdk_start:
    call rdx
    mov rdi, rax
    mov eax, SFCALL_EXIT
    syscall
    ud2                         ; SFCALL_EXIT does not return

; SfStatus Print(SfConsole* This, const char* Text)
sdk_console_print:
    mov rdi, rsi
    mov eax, SFCALL_CONSOLE_PRINT
    syscall
    ret

; SfStatus ReadLine(SfConsole* This, char* Buffer, uint64_t Size, uint64_t* Length)
sdk_console_readline:
    mov rdi, rsi
    mov rsi, rdx
    mov rdx, rcx
    mov eax, SFCALL_CONSOLE_READLINE
    syscall
    ret

sdk_code_end:
