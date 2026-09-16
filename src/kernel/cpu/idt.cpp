#include "../../include/cpu/idt.h"
#include "../../include/cpu/tss.h"
#include "../../include/cpu/process.h"

// External function to load IDT
extern "C" void load_idt(struct idtr* idtr_addr);

namespace idt {
    static struct interrupt_descriptor idt[IDT_ENTRIES];
    static struct idtr idt_reg;

    void set_entry(int index, uint64_t handler, uint8_t flags, uint8_t ist) {
        idt[index].address_low  = handler & 0xFFFF;
        idt[index].address_mid  = (handler >> 16) & 0xFFFF;
        idt[index].address_high = (handler >> 32) & 0xFFFFFFFF;
        idt[index].selector     = (uint16_t)0x08; // kernel code selector
        idt[index].ist          = ist & 0x7;
        idt[index].flags        = flags;
        idt[index].reserved     = 0;
    }

    void init(void) {
        static void (*const handlers[32])(void) = {
            exception_handler_0,  exception_handler_1,  exception_handler_2,
            exception_handler_3,  exception_handler_4,  exception_handler_5,
            exception_handler_6,  exception_handler_7,  exception_handler_8,
            exception_handler_9,  exception_handler_10, exception_handler_11,
            exception_handler_12, exception_handler_13, exception_handler_14,
            exception_handler_15, exception_handler_16, exception_handler_17,
            exception_handler_18, exception_handler_19, exception_handler_20,
            exception_handler_21, exception_handler_22, exception_handler_23,
            exception_handler_24, exception_handler_25, exception_handler_26,
            exception_handler_27, exception_handler_28, exception_handler_29,
            exception_handler_30, exception_handler_31,
        };

        // Vectors the CPU may raise when the current stack is already
        // unusable get their own stack from the TSS; without that the CPU
        // faults again while pushing the frame and the machine triple-faults
        // with nothing on screen and nothing on the serial line.
        for (int i = 0; i < 32; i++)
        {
            uint8_t ist = 0;
            if (i == EXCEPTION_DOUBLE_FAULT)  ist = IST_DOUBLE_FAULT;
            if (i == EXCEPTION_NMI)           ist = IST_NMI;
            if (i == EXCEPTION_MACHINE_CHECK) ist = IST_MACHINE_CHECK;

            set_entry(i, (uint64_t)handlers[i], IDT_FLAG_INTERRUPT_GATE, ist);
        }

        // Everything else stays not-present: a stray vector then raises #NP
        // with a diagnostic instead of jumping wherever the entry points.
        for (int i = 32; i < IDT_ENTRIES; i++)
            set_entry(i, 0, 0, 0);

        idt_reg.limit = sizeof(idt) - 1;
        idt_reg.base = (uint64_t)&idt;

        load_idt(&idt_reg);
    }
} // namespace

static inline uint64_t read_cr2()
{
    uint64_t cr2;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    return cr2;
}

static const char* const exception_names[32] = {
    "Divide Error (#DE)",
    "Debug (#DB)",
    "Non-Maskable Interrupt (NMI)",
    "Breakpoint (#BP)",
    "Overflow (#OF)",
    "Bound Range Exceeded (#BR)",
    "Invalid Opcode (#UD)",
    "Device Not Available (#NM)",
    "Double Fault (#DF)",
    "Coprocessor Segment Overrun",
    "Invalid TSS (#TS)",
    "Segment Not Present (#NP)",
    "Stack Segment Fault (#SS)",
    "General Protection Fault (#GP)",
    "Page Fault (#PF)",
    "Reserved (15)",
    "x87 FPU Error (#MF)",
    "Alignment Check (#AC)",
    "Machine Check (#MC)",
    "SIMD FPU Exception (#XM)",
    "Virtualization Exception (#VE)",
    "Control Protection Exception (#CP)",
    "Reserved (22)",
    "Reserved (23)",
    "Reserved (24)",
    "Reserved (25)",
    "Reserved (26)",
    "Reserved (27)",
    "Hypervisor Injection Exception (#HV)",
    "VMM Communication Exception (#VC)",
    "Security Exception (#SX)",
    "Reserved (31)",
};

// stack_frame points at the CPU-pushed frame: [0]=RIP [1]=CS [2]=RFLAGS
// [3]=RSP [4]=SS. The error code is passed separately; the asm stubs push a
// zero for the vectors that do not have one, so the layout is uniform.
extern "C" void handle_exception(uint64_t vector, uint64_t* stack_frame, uint64_t error_code)
{
    const char* name = (vector < 32) ? exception_names[vector] : "Unknown vector";
    bool from_user = (stack_frame[1] & 3) == 3;

    uart::printf("%s: vector=%llu err=%llx rip=%llx cs=%llx rflags=%llx "
                 "rsp=%llx cr2=%llx\n",
                 from_user ? "app fault" : "kernel fault",
                 vector, error_code, stack_frame[0], stack_frame[1],
                 stack_frame[2], stack_frame[3], read_cr2());

    // NMI, #DF and #MC are machine-level events delivered on IST stacks; they
    // are not the app's fault and must not be turned into a context switch
    // on a stack another NMI could reuse.
    bool machine_event = vector == EXCEPTION_NMI ||
                         vector == EXCEPTION_DOUBLE_FAULT ||
                         vector == EXCEPTION_MACHINE_CHECK;

    if (from_user && !machine_event)
    {
        // A fault in ring 3 kills the process, never the machine. This
        // includes #DE: the old code special-cased vector 0 and blindly
        // advanced RIP by two bytes, which guessed the instruction length and
        // let a user program loop on a division by zero forever.
        //
        // The stub pushed the GPRs and the error code right below the frame.
        user_regs* regs = (user_regs*)(stack_frame - 16);
        process::on_user_fault(vector, regs, (iret_frame*)stack_frame);
        return;     // the stub now returns into whichever process runs next
    }

    // Faults in ring 0 are bugs in this kernel; there is nothing safe to
    // resume into.
    print("\n\rKERNEL PANIC: ");
    print(name);
    print("\n\r");

    while (1) asm volatile("cli; hlt");
}
