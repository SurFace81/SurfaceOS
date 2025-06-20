#include "../../include/cpu/idt.h"

// External function to load IDT
extern "C" void load_idt(struct idtr* idtr_addr);

namespace idt {
    static struct interrupt_descriptor idt[IDT_ENTRIES] = {0};
    static struct idtr idt_reg;    

    void set_entry(int index, UINT64 handler, UINT8 flags) {
        idt[index].address_low  = handler & 0xFFFF;
        idt[index].address_mid  = (handler >> 16) & 0xFFFF;
        idt[index].address_high = (handler >> 32) & 0xFFFFFFFF;
        idt[index].selector     = (UINT16)0x08; // code selector
        idt[index].ist          = 0;            // Use main stack
        idt[index].flags        = flags;
        idt[index].reserved     = 0;
    }

    void init(void) {  
        // Set up CPU exception handlers (0-31)
        set_entry(0,  (UINT64)exception_handler_0,  IDT_FLAG_INTERRUPT_GATE);
        set_entry(1,  (UINT64)exception_handler_1,  IDT_FLAG_INTERRUPT_GATE);
        set_entry(2,  (UINT64)exception_handler_2,  IDT_FLAG_INTERRUPT_GATE);
        set_entry(3,  (UINT64)exception_handler_3,  IDT_FLAG_INTERRUPT_GATE);
        set_entry(4,  (UINT64)exception_handler_4,  IDT_FLAG_INTERRUPT_GATE);
        set_entry(5,  (UINT64)exception_handler_5,  IDT_FLAG_INTERRUPT_GATE);
        set_entry(6,  (UINT64)exception_handler_6,  IDT_FLAG_INTERRUPT_GATE);
        set_entry(7,  (UINT64)exception_handler_7,  IDT_FLAG_INTERRUPT_GATE);
        set_entry(8,  (UINT64)exception_handler_8,  IDT_FLAG_INTERRUPT_GATE);
        set_entry(9,  (UINT64)exception_handler_9,  IDT_FLAG_INTERRUPT_GATE);
        set_entry(10, (UINT64)exception_handler_10, IDT_FLAG_INTERRUPT_GATE);
        set_entry(11, (UINT64)exception_handler_11, IDT_FLAG_INTERRUPT_GATE);
        set_entry(12, (UINT64)exception_handler_12, IDT_FLAG_INTERRUPT_GATE);
        set_entry(13, (UINT64)exception_handler_13, IDT_FLAG_INTERRUPT_GATE);
        set_entry(14, (UINT64)exception_handler_14, IDT_FLAG_INTERRUPT_GATE);
        set_entry(15, (UINT64)exception_handler_15, IDT_FLAG_INTERRUPT_GATE);
        set_entry(16, (UINT64)exception_handler_16, IDT_FLAG_INTERRUPT_GATE);
        set_entry(17, (UINT64)exception_handler_17, IDT_FLAG_INTERRUPT_GATE);
        set_entry(18, (UINT64)exception_handler_18, IDT_FLAG_INTERRUPT_GATE);
        set_entry(19, (UINT64)exception_handler_19, IDT_FLAG_INTERRUPT_GATE);
        set_entry(20, (UINT64)exception_handler_20, IDT_FLAG_INTERRUPT_GATE);
        set_entry(21, (UINT64)exception_handler_21, IDT_FLAG_INTERRUPT_GATE);
        set_entry(22, (UINT64)exception_handler_22, IDT_FLAG_INTERRUPT_GATE);
        set_entry(23, (UINT64)exception_handler_23, IDT_FLAG_INTERRUPT_GATE);
        set_entry(24, (UINT64)exception_handler_24, IDT_FLAG_INTERRUPT_GATE);
        set_entry(25, (UINT64)exception_handler_25, IDT_FLAG_INTERRUPT_GATE);
        set_entry(26, (UINT64)exception_handler_26, IDT_FLAG_INTERRUPT_GATE);
        set_entry(27, (UINT64)exception_handler_27, IDT_FLAG_INTERRUPT_GATE);
        set_entry(28, (UINT64)exception_handler_28, IDT_FLAG_INTERRUPT_GATE);
        set_entry(29, (UINT64)exception_handler_29, IDT_FLAG_INTERRUPT_GATE);
        set_entry(30, (UINT64)exception_handler_30, IDT_FLAG_INTERRUPT_GATE);
        set_entry(31, (UINT64)exception_handler_31, IDT_FLAG_INTERRUPT_GATE);
        
        // Configure IDTR
        idt_reg.limit = sizeof(idt) - 1;  // 256 * 16 - 1 = 4095 (0xFFF)
        idt_reg.base = (UINT64)&idt;
        
        // Load IDT
        load_idt(&idt_reg);
    }
} // namespace

extern "C" void handle_exception(int exception_number, UINT64* stack_frame) {
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

    switch (exception_number) {
        case 0:
            stack_frame[0] += 2;    // Increase RIP
            break;
        default:
            print("\n\r");
            print(exception_names[exception_number]);
            print("\n\r");
            while (1) asm volatile ("hlt");
            break;
    }

    uart::write("\n");
    uart::write(exception_names[exception_number]);
    uart::write("\n");
}