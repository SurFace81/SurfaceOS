#ifndef IRQ_H
#define IRQ_H

#include "types.h"
#include "idt.h"
#include "ports.h"

// IRQ numbers (hardware interrupts)
#define IRQ0_TIMER          0
#define IRQ1_KEYBOARD       1
#define IRQ2_CASCADE        2
#define IRQ3_COM2           3
#define IRQ4_COM1           4
#define IRQ5_LPT2           5
#define IRQ6_FLOPPY         6
#define IRQ7_LPT1           7
#define IRQ8_RTC            8
#define IRQ9_FREE           9
#define IRQ10_FREE          10
#define IRQ11_FREE          11
#define IRQ12_MOUSE         12
#define IRQ13_FPU           13
#define IRQ14_PRIMARY_ATA   14
#define IRQ15_SECONDARY_ATA 15

// PIC ports
#define PIC1_COMMAND    0x20
#define PIC1_DATA       0x21
#define PIC2_COMMAND    0xA0
#define PIC2_DATA       0xA1

// PIC commands
#define PIC_EOI         0x20
#define ICW1_ICW4       0x01
#define ICW1_SINGLE     0x02
#define ICW1_INTERVAL4  0x04
#define ICW1_LEVEL      0x08
#define ICW1_INIT       0x10

#define ICW4_8086       0x01
#define ICW4_AUTO       0x02
#define ICW4_BUF_SLAVE  0x08
#define ICW4_BUF_MASTER 0x0C
#define ICW4_SFNM       0x10

// IRQ base vectors (remapped)
#define IRQ_BASE        32

// IRQ handler function type
typedef void (*irq_handler_t)(void);

// Structure to hold CPU state during interrupt (x86_64)
struct interrupt_frame {
    UINT64 r15, r14, r13, r12, r11, r10, r9, r8;  // Additional x86_64 registers
    UINT64 rdi, rsi, rbp, rdx, rcx, rbx, rax;     // General purpose registers
    UINT64 int_no, err_code;                      // Interrupt number and error code
    UINT64 rip, cs, rflags, rsp, ss;              // Automatically pushed by processor
};

// Functions
namespace irq {
    void init(void);
    void install_handler(int irq, irq_handler_t handler);
    void uninstall_handler(int irq);
}

// Assembly interrupt stubs (defined in interrupts.asm)
extern "C" void irq0(void);
extern "C" void irq1(void);
extern "C" void irq2(void);
extern "C" void irq3(void);
extern "C" void irq4(void);
extern "C" void irq5(void);
extern "C" void irq6(void);
extern "C" void irq7(void);
extern "C" void irq8(void);
extern "C" void irq9(void);
extern "C" void irq10(void);
extern "C" void irq11(void);
extern "C" void irq12(void);
extern "C" void irq13(void);
extern "C" void irq14(void);
extern "C" void irq15(void);

// Common IRQ handler (called from assembly)
extern "C" void irq_handler(struct interrupt_frame *frame);

#endif // IRQ_H