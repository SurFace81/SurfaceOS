#include "../../include/cpu/idt.h"

// IDT array (256 entries)
static struct interrupt_descriptor idt[IDT_ENTRIES];
static struct idtr idt_reg;

// External function to load IDT (from asm)
extern void load_idt(struct idtr* idtr_addr);

void set_idt_entry(int index, UINT64 handler, UINT16 selector, UINT8 flags) {
    idt[index].address_low = handler & 0xFFFF;
    idt[index].address_mid = (handler >> 16) & 0xFFFF;
    idt[index].address_high = (handler >> 32) & 0xFFFFFFFF;
    idt[index].selector = selector;
    idt[index].ist = 0;  // Use main stack
    idt[index].flags = flags;
    idt[index].reserved = 0;
}

void init_idt(void) {
    // Clear IDT
    for (int i = 32; i < IDT_ENTRIES; i++) {
        idt[i].address_low = 0;
        idt[i].address_mid = 0;
        idt[i].address_high = 0;
        idt[i].selector = 0;
        idt[i].ist = 0;
        idt[i].flags = 0;
        idt[i].reserved = 0;
    }
    
    // Set up CPU exception handlers (0-31)
    set_idt_entry(0,  (UINT64)exception_handler_0,  0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(1,  (UINT64)exception_handler_1,  0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(2,  (UINT64)exception_handler_2,  0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(3,  (UINT64)exception_handler_3,  0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(4,  (UINT64)exception_handler_4,  0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(5,  (UINT64)exception_handler_5,  0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(6,  (UINT64)exception_handler_6,  0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(7,  (UINT64)exception_handler_7,  0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(8,  (UINT64)exception_handler_8,  0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(9,  (UINT64)exception_handler_9,  0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(10, (UINT64)exception_handler_10, 0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(11, (UINT64)exception_handler_11, 0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(12, (UINT64)exception_handler_12, 0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(13, (UINT64)exception_handler_13, 0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(14, (UINT64)exception_handler_14, 0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(15, (UINT64)exception_handler_15, 0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(16, (UINT64)exception_handler_16, 0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(17, (UINT64)exception_handler_17, 0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(18, (UINT64)exception_handler_18, 0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(19, (UINT64)exception_handler_19, 0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(20, (UINT64)exception_handler_20, 0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(21, (UINT64)exception_handler_21, 0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(22, (UINT64)exception_handler_22, 0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(23, (UINT64)exception_handler_23, 0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(24, (UINT64)exception_handler_24, 0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(25, (UINT64)exception_handler_25, 0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(26, (UINT64)exception_handler_26, 0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(27, (UINT64)exception_handler_27, 0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(28, (UINT64)exception_handler_28, 0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(29, (UINT64)exception_handler_29, 0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(30, (UINT64)exception_handler_30, 0x08, IDT_FLAG_INTERRUPT_GATE);
    set_idt_entry(31, (UINT64)exception_handler_31, 0x08, IDT_FLAG_INTERRUPT_GATE);
    
    // Configure IDTR
    idt_reg.limit = sizeof(idt) - 1;  // 256 * 16 - 1 = 4095 (0xFFF)
    idt_reg.base = (UINT64)&idt;
    
    // Load IDT
    load_idt(&idt_reg);
}

// C exception handlers
void handle_exception(int exception_number, UINT64* stack_frame) {
    const char* exception_names[] = {
        "Divide Error (#DE)",                   // 0  - Test: int a = 10/0;
        "Debug (#DB)",                          // 1  - Test: Set breakpoint with debugger
        "Non-Maskable Interrupt (NMI)",         // 2  - Test: Hardware NMI (hard to trigger)
        "Breakpoint (#BP)",                     // 3  - Test: asm volatile("int3");
        "Overflow (#OF)",                       // 4  - Test: asm volatile("into"); with OF flag set
        "Bound Range Exceeded (#BR)",           // 5  - Test: BOUND instruction (rarely used)
        "Invalid Opcode (#UD)",                 // 6  - Test: asm volatile(".byte 0xff, 0xff");
        "Device Not Available (#NM)",           // 7  - Test: Clear CR0.TS, then use FPU instruction
        "Double Fault (#DF)",                   // 8  - Test: Cause exception while handling exception
        "Coprocessor Segment Overrun",          // 9  - Legacy, shouldn't occur on modern CPUs
        "Invalid TSS (#TS)",                    // 10 - Test: Load invalid TSS selector
        "Segment Not Present (#NP)",            // 11 - Test: Load segment with present bit = 0
        "Stack Segment Fault (#SS)",            // 12 - Test: Access beyond stack limit
        "General Protection Fault (#GP)",       // 13 - Test: Access null pointer, privilege violation
        "Page Fault (#PF)",                     // 14 - Test: Access unmapped virtual address
        "Reserved",                             // 15 - Intel reserved
        "x87 FPU Error (#MF)",                  // 16 - Test: Cause FPU exception with unmasked bit
        "Alignment Check (#AC)",                // 17 - Test: Unaligned access with AC flag set
        "Machine Check (#MC)",                  // 18 - Hardware error, hard to test safely
        "SIMD FPU Exception (#XM)",             // 19 - Test: SSE floating point exception
        "Virtualization Exception (#VE)",       // 20 - VM-related, unlikely in your OS
        "Control Protection Exception (#CP)",   // 21 - Intel CET feature, unlikely to trigger
        "Reserved",                             // 22-27 - Intel reserved
        "Reserved",
        "Reserved", 
        "Reserved",
        "Reserved",
        "Reserved",
        "Hypervisor Injection Exception (#HV)", // 28 - Hypervisor-related
        "VMM Communication Exception (#VC)",    // 29 - VM-related
        "Security Exception (#SX)",             // 30 - Security-related
        "Reserved"                              // 31 - Intel reserved
    };
    
    // Skip the faulting instruction for most exceptions
    // Note: Some exceptions like #PF might need special handling
    if (exception_number != 8 && exception_number != 18) { // Don't skip for Double Fault or Machine Check
        uart_write("\n");
        uart_write(exception_names[exception_number]);
        uart_write("\n");
    } else {
        while (1) asm volatile("hlt");
    }
}