; The SDK code page (see sdkpage.h). These bytes are copied into every
; SurfaceOS program and run there in ring 3, never in the kernel. Nothing
; here may refer to an absolute address: only registers, the stack and
; relative jumps.
;
; Call numbers mirror abi/sfcall.h.

%define SFCALL_EXIT             0
%define SFCALL_CONSOLE_PRINT    1
%define SFCALL_CONSOLE_READLINE 2
%define SFCALL_FILES_OPEN       3
%define SFCALL_FILES_CREATE_UNIQUE 4
%define SFCALL_FILE_OPEN        5
%define SFCALL_FILE_CLOSE       6
%define SFCALL_FILE_READ        7
%define SFCALL_FILE_WRITE       8
%define SFCALL_FILE_GET_POSITION 9
%define SFCALL_FILE_SET_POSITION 10

; A protocol function whose arguments go to the kernel as they are, This
; included: the call's fourth argument moves from rcx to r10, which the
; `syscall` instruction leaves alone.
%macro PASS_THROUGH 2
global %1
%1:
    mov r10, rcx
    mov eax, %2
    syscall
    ret
%endmacro

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

PASS_THROUGH sdk_files_open,          SFCALL_FILES_OPEN
PASS_THROUGH sdk_files_create_unique, SFCALL_FILES_CREATE_UNIQUE
PASS_THROUGH sdk_file_open,           SFCALL_FILE_OPEN
PASS_THROUGH sdk_file_close,          SFCALL_FILE_CLOSE
PASS_THROUGH sdk_file_read,           SFCALL_FILE_READ
PASS_THROUGH sdk_file_write,          SFCALL_FILE_WRITE
PASS_THROUGH sdk_file_get_position,   SFCALL_FILE_GET_POSITION
PASS_THROUGH sdk_file_set_position,   SFCALL_FILE_SET_POSITION

sdk_code_end:
