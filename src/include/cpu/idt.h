#ifndef IDT_H
#define IDT_H

#include "types.h"
#include "../drivers/uart.h"

// IDT descriptor
struct interrupt_descriptor {
    UINT16 address_low;    // 
    UINT16 selector;       // code segment
    UINT8  ist;            // Interrupt Stack Table (0 for general)
    UINT8  flags;          // 
    UINT16 address_mid;    // 
    UINT32 address_high;   // 
    UINT32 reserved;       // (must be zero)
} __attribute__((packed));

// IDTR
struct idtr {
    UINT16 limit;     // Size of IDT in bytes minus 1
    UINT64 base;      // Base address of IDT
} __attribute__((packed));

#define IDT_ENTRIES 256
#define IDT_FLAG_INTERRUPT_GATE 0x8E  // Present=1, DPL=00, Type=1110 (interrupt gate)

// Exception numbers
#define EXCEPTION_DIVIDE_ERROR          0   // #DE
#define EXCEPTION_DEBUG                 1   // #DB
#define EXCEPTION_NMI                   2   // NMI
#define EXCEPTION_BREAKPOINT            3   // #BP
#define EXCEPTION_OVERFLOW              4   // #OF
#define EXCEPTION_BOUND_RANGE           5   // #BR
#define EXCEPTION_INVALID_OPCODE        6   // #UD
#define EXCEPTION_DEVICE_NOT_AVAILABLE  7   // #NM
#define EXCEPTION_DOUBLE_FAULT          8   // #DF
#define EXCEPTION_COPROCESSOR_OVERRUN   9   // (obsolete)
#define EXCEPTION_INVALID_TSS           10  // #TS
#define EXCEPTION_SEGMENT_NOT_PRESENT   11  // #NP
#define EXCEPTION_STACK_SEGMENT_FAULT   12  // #SS
#define EXCEPTION_GENERAL_PROTECTION    13  // #GP
#define EXCEPTION_PAGE_FAULT            14  // #PF
#define EXCEPTION_RESERVED_15           15  // Reserved
#define EXCEPTION_X87_FPU               16  // #MF
#define EXCEPTION_ALIGNMENT_CHECK       17  // #AC
#define EXCEPTION_MACHINE_CHECK         18  // #MC
#define EXCEPTION_SIMD_FPU              19  // #XM
#define EXCEPTION_VIRTUALIZATION        20  // #VE
#define EXCEPTION_CONTROL_PROTECTION    21  // #CP
#define EXCEPTION_RESERVED_22           22  // Reserved
#define EXCEPTION_RESERVED_23           23  // Reserved
#define EXCEPTION_RESERVED_24           24  // Reserved
#define EXCEPTION_RESERVED_25           25  // Reserved
#define EXCEPTION_RESERVED_26           26  // Reserved
#define EXCEPTION_RESERVED_27           27  // Reserved
#define EXCEPTION_HV_INJECTION          28  // #HV
#define EXCEPTION_VMM_COMMUNICATION     29  // #VC
#define EXCEPTION_SECURITY              30  // #SX
#define EXCEPTION_RESERVED_31           31  // Reserved

#ifdef __cplusplus
extern "C" {
#endif

void initIDT(void);
void set_idt_entry(int index, UINT64 handler, UINT8 flags);

#ifdef __cplusplus
}
#endif

// extern void divide_by_zero_handler(void);
// extern void generic_exception_handler(void);

// Exception handlers (declared in asm file)
extern void exception_handler_0(void);   // Divide Error
extern void exception_handler_1(void);   // Debug
extern void exception_handler_2(void);   // NMI
extern void exception_handler_3(void);   // Breakpoint
extern void exception_handler_4(void);   // Overflow
extern void exception_handler_5(void);   // Bound Range Exceeded
extern void exception_handler_6(void);   // Invalid Opcode
extern void exception_handler_7(void);   // Device Not Available
extern void exception_handler_8(void);   // Double Fault
extern void exception_handler_9(void);   // Coprocessor Segment Overrun
extern void exception_handler_10(void);  // Invalid TSS
extern void exception_handler_11(void);  // Segment Not Present
extern void exception_handler_12(void);  // Stack Segment Fault
extern void exception_handler_13(void);  // General Protection Fault
extern void exception_handler_14(void);  // Page Fault
extern void exception_handler_15(void);  // Reserved
extern void exception_handler_16(void);  // x87 FPU Error
extern void exception_handler_17(void);  // Alignment Check
extern void exception_handler_18(void);  // Machine Check
extern void exception_handler_19(void);  // SIMD FPU Exception
extern void exception_handler_20(void);  // Virtualization Exception
extern void exception_handler_21(void);  // Control Protection Exception
extern void exception_handler_22(void);  // Reserved
extern void exception_handler_23(void);  // Reserved
extern void exception_handler_24(void);  // Reserved
extern void exception_handler_25(void);  // Reserved
extern void exception_handler_26(void);  // Reserved
extern void exception_handler_27(void);  // Reserved
extern void exception_handler_28(void);  // Hypervisor Injection Exception
extern void exception_handler_29(void);  // VMM Communication Exception
extern void exception_handler_30(void);  // Security Exception
extern void exception_handler_31(void);  // Reserved

#endif // IDT_H