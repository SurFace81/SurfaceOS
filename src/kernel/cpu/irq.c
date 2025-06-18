#include "../../include/cpu/irq.h"
#include "../../include/cpu/idt.h"
#include "../../include/cpu/ports.h"

static irq_handler_t irq_handlers[16] = {0};

// Remap the PIC controllers
static void pic_remap(int offset1, int offset2) {
    UINT8 a1, a2;
    
    // Save masks
    a1 = port_byte_in(PIC1_DATA);
    a2 = port_byte_in(PIC2_DATA);
    
    // Start initialization sequence (in cascade mode)
    port_byte_out(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    port_byte_out(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    
    // ICW2: Master PIC vector offset
    port_byte_out(PIC1_DATA, offset1);
    io_wait();
    // ICW2: Slave PIC vector offset
    port_byte_out(PIC2_DATA, offset2);
    io_wait();
    
    // ICW3: Tell Master PIC that there is a slave PIC at IRQ2 (0000 0100)
    port_byte_out(PIC1_DATA, 4);
    io_wait();
    // ICW3: Tell Slave PIC its cascade identity (0000 0010)
    port_byte_out(PIC2_DATA, 2);
    io_wait();
    
    // ICW4: Have the PICs use 8086 mode (and not 8080 mode)
    port_byte_out(PIC1_DATA, ICW4_8086);
    io_wait();
    port_byte_out(PIC2_DATA, ICW4_8086);
    io_wait();
    
    // Restore saved masks
    port_byte_out(PIC1_DATA, a1);
    port_byte_out(PIC2_DATA, a2);
}

// Send End-Of-Interrupt signal
static void pic_send_eoi(UINT8 irq) {
    if (irq >= 8) {
        port_byte_out(PIC2_COMMAND, PIC_EOI);
    }
    port_byte_out(PIC1_COMMAND, PIC_EOI);
}

// Check if IRQ is spurious
static int is_spurious_irq(UINT8 irq) {
    if (irq == 7) {
        port_byte_out(PIC1_COMMAND, 0x0B);
        return !(port_byte_in(PIC1_COMMAND) & 0x80);
    } else if (irq == 15) {
        port_byte_out(PIC2_COMMAND, 0x0B);
        return !(port_byte_in(PIC2_COMMAND) & 0x80);
    }
    return 0;
}

// Initialize IRQ system
void initIRQ(void) {
    asm volatile ("cli");
    pic_remap(IRQ_BASE, IRQ_BASE + 8);
    
    set_idt_entry(32, (UINT64)irq0,  IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(33, (UINT64)irq1,  IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(34, (UINT64)irq2,  IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(35, (UINT64)irq3,  IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(36, (UINT64)irq4,  IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(37, (UINT64)irq5,  IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(38, (UINT64)irq6,  IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(39, (UINT64)irq7,  IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(40, (UINT64)irq8,  IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(41, (UINT64)irq9,  IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(42, (UINT64)irq10, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(43, (UINT64)irq11, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(44, (UINT64)irq12, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(45, (UINT64)irq13, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(46, (UINT64)irq14, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(47, (UINT64)irq15, IDT_FLAG_INTERRUPT_GATE);
    
    irq_mask_all();

    asm volatile("sti");
}

// Install IRQ handler
void irq_install_handler(int irq, irq_handler_t handler) {
    if (irq >= 0 && irq < 16) {
        irq_handlers[irq] = handler;
        irq_enable(irq);
    }
}

// Remove IRQ handler
void irq_uninstall_handler(int irq) {
    if (irq >= 0 && irq < 16) {
        irq_handlers[irq] = 0;
        irq_disable(irq);
    }
}

// Enable specific IRQ
void irq_enable(int irq) {
    UINT16 port;
    UINT8 value;
    
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    
    value = port_byte_in(port) & ~(1 << irq);
    port_byte_out(port, value);
}

// Disable specific IRQ
void irq_disable(int irq) {
    UINT16 port;
    UINT8 value;
    
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    
    value = port_byte_in(port) | (1 << irq);
    port_byte_out(port, value);
}

// Mask all IRQs
void irq_mask_all(void) {
    port_byte_out(PIC1_DATA, 0xFF);
    port_byte_out(PIC2_DATA, 0xFF);
}

// Unmask all IRQs
void irq_unmask_all(void) {
    port_byte_out(PIC1_DATA, 0x00);
    port_byte_out(PIC2_DATA, 0x00);
}

// Common IRQ handler called from assembly
void irq_handler(struct interrupt_frame *frame) {
    UINT8 irq_line = frame->int_no - IRQ_BASE;
    
    if (is_spurious_irq(irq_line)) {
        if (irq_line == 15) {
            port_byte_out(PIC1_COMMAND, PIC_EOI);
        }
        return;
    }

    if (irq_handlers[irq_line] != 0) {
        irq_handlers[irq_line]();
    }
    
    pic_send_eoi(irq_line);
}