#include "../../include/cpu/irq.h"
#include "../../include/cpu/syscall.h"

alignas(8) irq_handler_t irq_handlers[16] = {0};

namespace irq {
    void enable(int irq);
    void disable(int irq);
    void mask_all(void);
    void unmask_all(void);

    // Remap the PIC controllers
    static void pic_remap(int offset1, int offset2) {
        uint8_t a1, a2;
        
        // Save masks
        a1 = port::byte_in(PIC1_DATA);
        a2 = port::byte_in(PIC2_DATA);
        
        // Start initialization sequence (in cascade mode)
        port::byte_out(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
        port::io_wait();
        port::byte_out(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
        port::io_wait();
        
        // ICW2: Master PIC vector offset
        port::byte_out(PIC1_DATA, offset1);
        port::io_wait();
        // ICW2: Slave PIC vector offset
        port::byte_out(PIC2_DATA, offset2);
        port::io_wait();
        
        // ICW3: Tell Master PIC that there is a slave PIC at IRQ2 (0000 0100)
        port::byte_out(PIC1_DATA, 4);
        port::io_wait();
        // ICW3: Tell Slave PIC its cascade identity (0000 0010)
        port::byte_out(PIC2_DATA, 2);
        port::io_wait();
        
        // ICW4: Have the PICs use 8086 mode (and not 8080 mode)
        port::byte_out(PIC1_DATA, ICW4_8086);
        port::io_wait();
        port::byte_out(PIC2_DATA, ICW4_8086);
        port::io_wait();
        
        // Restore saved masks
        port::byte_out(PIC1_DATA, a1);
        port::byte_out(PIC2_DATA, a2);
    }

    // Send End-Of-Interrupt signal
    static void pic_send_eoi(uint8_t irq) {
        if (irq >= 8) {
            port::byte_out(PIC2_COMMAND, PIC_EOI);
        }
        port::byte_out(PIC1_COMMAND, PIC_EOI);
    }

    // Check if IRQ is spurious
    static int is_spurious_irq(uint8_t irq) {
        if (irq == 7) {
            port::byte_out(PIC1_COMMAND, 0x0B);
            return !(port::byte_in(PIC1_COMMAND) & 0x80);
        } else if (irq == 15) {
            port::byte_out(PIC2_COMMAND, 0x0B);
            return !(port::byte_in(PIC2_COMMAND) & 0x80);
        }
        return 0;
    }

    void init(void) {
        asm volatile ("cli");
        pic_remap(IRQ_BASE, IRQ_BASE + 8);
        
        idt::set_entry(32, (uint64_t)irq0,  IDT_FLAG_INTERRUPT_GATE);
        idt::set_entry(33, (uint64_t)irq1,  IDT_FLAG_INTERRUPT_GATE);
        idt::set_entry(34, (uint64_t)irq2,  IDT_FLAG_INTERRUPT_GATE);
        idt::set_entry(35, (uint64_t)irq3,  IDT_FLAG_INTERRUPT_GATE);
        idt::set_entry(36, (uint64_t)irq4,  IDT_FLAG_INTERRUPT_GATE);
        idt::set_entry(37, (uint64_t)irq5,  IDT_FLAG_INTERRUPT_GATE);
        idt::set_entry(38, (uint64_t)irq6,  IDT_FLAG_INTERRUPT_GATE);
        idt::set_entry(39, (uint64_t)irq7,  IDT_FLAG_INTERRUPT_GATE);
        idt::set_entry(40, (uint64_t)irq8,  IDT_FLAG_INTERRUPT_GATE);
        idt::set_entry(41, (uint64_t)irq9,  IDT_FLAG_INTERRUPT_GATE);
        idt::set_entry(42, (uint64_t)irq10, IDT_FLAG_INTERRUPT_GATE);
        idt::set_entry(43, (uint64_t)irq11, IDT_FLAG_INTERRUPT_GATE);
        idt::set_entry(44, (uint64_t)irq12, IDT_FLAG_INTERRUPT_GATE);
        idt::set_entry(45, (uint64_t)irq13, IDT_FLAG_INTERRUPT_GATE);
        idt::set_entry(46, (uint64_t)irq14, IDT_FLAG_INTERRUPT_GATE);
        idt::set_entry(47, (uint64_t)irq15, IDT_FLAG_INTERRUPT_GATE);

        // Syscall interrupt
        idt::set_entry(0x80, (uint64_t)syscall_entry, IDT_FLAG_INTERRUPT_GATE);
        
        mask_all();

        asm volatile("sti");
    }

    void install_handler(int irq, irq_handler_t handler) {
        if (irq >= 0 && irq < 16) {
            irq_handlers[irq] = handler;
            enable(irq);
        }
    }

    void uninstall_handler(int irq) {
        if (irq >= 0 && irq < 16) {
            irq_handlers[irq] = 0;
            disable(irq);
        }
    }

    void enable(int irq) {
        uint16_t port;
        uint8_t value;
        
        if (irq < 8) {
            port = PIC1_DATA;
        } else {
            port = PIC2_DATA;
            irq -= 8;
        }
        
        value = port::byte_in(port) & ~(1 << irq);
        port::byte_out(port, value);
    }

    void disable(int irq) {
        uint16_t port;
        uint8_t value;
        
        if (irq < 8) {
            port = PIC1_DATA;
        } else {
            port = PIC2_DATA;
            irq -= 8;
        }
        
        value = port::byte_in(port) | (1 << irq);
        port::byte_out(port, value);
    }

    void mask_all(void) {
        port::byte_out(PIC1_DATA, 0xFF);
        port::byte_out(PIC2_DATA, 0xFF);
    }

    void unmask_all(void) {
        port::byte_out(PIC1_DATA, 0x00);
        port::byte_out(PIC2_DATA, 0x00);
    }

} // namespace

// Common IRQ handler
void irq_handler(struct interrupt_frame *frame) {
    uint8_t irq_line = frame->int_no - IRQ_BASE;
    
    if (irq::is_spurious_irq(irq_line)) {
        if (irq_line == 15) {
            port::byte_out(PIC1_COMMAND, PIC_EOI);
        }
        return;
    }

    if (irq_handlers[irq_line] != 0) {
        irq_handlers[irq_line]();
    }
    
    irq::pic_send_eoi(irq_line);
}