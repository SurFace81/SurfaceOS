[bits 16]
[org 0x7C00]

    jmp short start
    nop

OEMName             db "SURFOS  "
BytesPerSector      dw 0x0200           ; 512 bytes per sector
SectorsPerCluster   db 0x08             ; 8 sectors per cluster
ReservedSectors     dw 0x0020           ; 32 sectors
NumberOfFATs        db 0x02             ; 2 FAT tables
RootEntitiesCount   dw 0x0000
TotalSectors16      dw 0x0000           ; ???
MediaType           db 0xF8             ; Fixed disk
SectorsPerFAT       dw 0x0000
SectorsPerTrack     dw 0x003D           ; 61 sectors per track
NumberOfHeads       dw 0x0002           ; 2 heads
HiddenSectors       dd 0x00000000
TotalSectors32      dd 0x00FE3B1F       ; Must be non 0 for FAT32

; Only for FAT32
BigSectorsPerFAT    dd 0x00000778
ExtFlags            dw 0x0000
FSVersion           dw 0x0000
RootDirectoryStart  dd 0x00000002       ; first cluster of root directory
FSInfoSector        dw 0x0001
BackupBootSector    dw 0x0006

times 12 db 0x00    ; reserved "for future expansion"

DriveNumber         db 0x00             ; boot drive
ReservedByte        db 0x00
BootSignature       db 0x29
VolumeID            dd 0xFFFFFFFF
VolumeLabel         db "SURFACEOS  "
FSType              db "FAT32   "


start:
    ; create stack and set segment registers
    cli
    xor ax, ax
    mov es, ax
    mov ds, ax
    mov fs, ax
    mov gs, ax
    mov bp, 0x9FC00
    mov sp, bp
    sti

    mov ah, 0x00
    mov al, 0x03
    int 0x10

    mov ah, 0x01
    mov cx, 0x2607
    int 0x10

    mov bx, uefi_msg
    call print

    jmp $


; Print function
; IN: bx - message
print:
    pusha
.start:
    mov al, [bx] 
    cmp al, 0 
    je done
    mov ah, 0x0e
    int 0x10 ; 
    add bx, 1
    jmp .start
done:
    popa
    ret


; Data
uefi_msg       db "You must use UEFI, if you want to try SurfaceOS!", 0