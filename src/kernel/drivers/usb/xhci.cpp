#include "../../../include/drivers/usb/xhci.h"
#include "../../../include/drivers/uart.h"
#include "../../../include/cpu/paging.h"

namespace xhci {
    static PCIDevice* controller_pci = nullptr;
    static XHCICapabilityRegs*  cap_regs = nullptr;
    static XHCIOperationalRegs* op_regs = nullptr;
    static XHCIPortRegs*        port_regs = nullptr;
    static XHCIRuntimeRegs*     runtime_regs = nullptr;
    static XHCIInterrupterRegs* interrupter_regs = nullptr;
    static UINT32*     doorbell_regs = nullptr;
    static USBDeviceList device_list = {0};
    
    // CAPLENGTH
    UINT8 cap_regs_length;

    // HCSPARAMS1
    UINT8 max_device_slots;
    UINT8 max_interrupters;
    UINT8 max_ports;

    // HCSPARAMS2
    UINT8 isochronous_sheduling_threshold;
    UINT8 erst_max;
    UINT8 scratchpad_count;

    // HCCPARAMS1
    bool bit64_addr_cap;
    bool bandwitdh_negotiation_cap;
    bool byte64_context_size;
    bool port_power_control;
    bool port_indicators;
    bool light_reset_cap;
    UINT32 ext_cap_offset;

    // Rings
    size_t segment_trb_count;
    XHCITrb* trbs;
    uintptr_t phys_base;
    XHCIErstEntry* segment_table;
    UINT64 dequeue_ptr;
    UINT8 rcs_bit;

    static UINT64 __attribute__((aligned(64))) dcbaa[256];
    constexpr UINT32 MAX_SCRATCHPADS = 64;
    static UINT64 __attribute__((aligned(64))) scratchpad_array[64];
    static UINT8 __attribute__((aligned(4096))) scratchpad_pages[64][4096];

    constexpr UINT32 COMMAND_RING_TRBS = 256;
    static XHCITrb __attribute__((aligned(64))) command_ring[COMMAND_RING_TRBS];
    static UINT32 cmd_ring_enq = 0;
    static UINT8  cmd_ring_cycle   = 1;

    constexpr UINT32 EVENT_RING_TRBS = 256;
    constexpr UINT32 EVENT_RING_SEGMENTS = 1;
    static XHCITrb __attribute__((aligned(64))) event_ring[EVENT_RING_TRBS];
    static XHCIErstEntry __attribute__((aligned(64))) event_ring_erst[EVENT_RING_SEGMENTS];
    static UINT32 event_ring_deq = 0;
    static UINT8  event_ring_cycle = 1;
    static UINT64 event_ring_phys_base = 0;
    static UINT64 erst_phys_base = 0;

    // Address device
    enum {
        XHCI_TRB_TYPE_ENABLE_SLOT_CMD          = 9,
        XHCI_TRB_TYPE_ADDRESS_DEVICE_CMD       = 11,
        XHCI_TRB_TYPE_CONFIGURE_ENDPOINT_CMD   = 12,
        XHCI_TRB_TYPE_COMMAND_COMPLETION_EVENT = 33,
        XHCI_TRB_TYPE_TRANSFER_EVENT           = 32,

        XHCI_TRB_TYPE_SETUP_STAGE              = 2,
        XHCI_TRB_TYPE_DATA_STAGE               = 3,
        XHCI_TRB_TYPE_STATUS_STAGE             = 4,
    };

    // Минимальные оффсеты/флаги для контекстов
    static inline UINT32 ctx_stride() { return byte64_context_size ? 64 : 32; }

    // Input Context: Control Context + (Slot + EP0 + ...)
    struct __attribute__((packed)) XHCIInputControlContext {
        UINT32 drop_context_flags;
        UINT32 add_context_flags;
        UINT32 rsvd0[5];
        UINT32 configuration_value;
        UINT32 interface_number;
        UINT32 alternate_setting;
        UINT32 rsvd1;
    };

    // Slot Context (минимально нужные поля)
    // Мы будем писать через dword-доступ, чтобы не попасть на битфилды/эндиан.
    struct __attribute__((packed)) XHCISlotContext {
        UINT32 dw0;
        UINT32 dw1;
        UINT32 dw2;
        UINT32 dw3;
        UINT32 rsvd[4]; // довести до 32 байт минимум (дальше stride решит)
    };

    // Endpoint Context (минимально)
    struct __attribute__((packed)) XHCIEndpointContext {
        UINT32 dw0;
        UINT32 dw1;
        UINT32 tr_dequeue_ptr_lo;
        UINT32 tr_dequeue_ptr_hi;
        UINT32 dw4;
        UINT32 rsvd[3];
    };

    // Один девайс: Device Context + Input Context
    static UINT8 __attribute__((aligned(4096))) input_context_mem[4096];
    static UINT8 __attribute__((aligned(4096))) device_context_mem[4096];

    // EP0 Transfer Ring (минимально)
    constexpr UINT32 EP0_RING_TRBS = 32;
    static XHCITrb __attribute__((aligned(64))) ep0_ring[EP0_RING_TRBS];
    static UINT32 ep0_enq = 0;
    static UINT8  ep0_cycle = 1;

    static UINT8 g_slot_id = 0;
    static UINT8 g_port_id = 0;
    static UINT32 g_port_speed = 0; // из PORTSC (PSIV)

    struct __attribute__((packed)) USBSetupPacket {
        UINT8  bmRequestType;
        UINT8  bRequest;
        UINT16 wValue;
        UINT16 wIndex;
        UINT16 wLength;
    };

    // стандартные запросы
    constexpr UINT8 USB_REQ_GET_DESCRIPTOR = 6;
    constexpr UINT8 USB_DESC_DEVICE = 1;
    constexpr UINT8 USB_DESC_CONFIGURATION = 2;

    constexpr UINT32 BULK_RING_TRBS = 256;

    static XHCITrb __attribute__((aligned(64))) bulk_in_ring[BULK_RING_TRBS];
    static XHCITrb __attribute__((aligned(64))) bulk_out_ring[BULK_RING_TRBS];

    static UINT32 bulk_in_enq = 0;
    static UINT32 bulk_out_enq = 0;
    static UINT8  bulk_in_cycle = 1;
    static UINT8  bulk_out_cycle = 1;

    static UINT8  g_bulk_ep_in_dci  = 0;
    static UINT8  g_bulk_ep_out_dci = 0;

    constexpr UINT8 USB_REQ_SET_CONFIGURATION = 9;
    constexpr UINT8 XHCI_TRB_TYPE_NORMAL = 1;

    struct __attribute__((packed)) MSC_CBW {
        UINT32 dCBWSignature;   // 'USBC' 0x43425355
        UINT32 dCBWTag;
        UINT32 dCBWDataTransferLength;
        UINT8  bmCBWFlags;      // 0x80 IN, 0x00 OUT
        UINT8  bCBWLUN;
        UINT8  bCBWCBLength;
        UINT8  CBWCB[16];
    };

    struct __attribute__((packed)) MSC_CSW {
        UINT32 dCSWSignature;   // 'USBS' 0x53425355
        UINT32 dCSWTag;
        UINT32 dCSWDataResidue;
        UINT8  bCSWStatus;      // 0=pass
    };

    static UINT32 g_msc_tag = 1;

    static PCIDevice* find_xhci_controller() {
        for (UINT32 i = 0; i < pci::device_count(); i++) {
            PCIDevice* d = pci::get_by_id(i);
            if (d && d->valid && d->class_code == 0x0C && d->subclass == 0x03 && d->prog_if == 0x30) {
                return d;
            }
        }
        return nullptr;
    }

    static bool map_registers() {
        UINT64 base_address = controller_pci->bar[0] & ~0xF;

        cap_regs = (XHCICapabilityRegs*)base_address;
        cap_regs_length = cap_regs->caplength;

        max_device_slots = ((cap_regs->hcsparams1) & 0xFF);
        max_interrupters = ((cap_regs->hcsparams1 >> 8) & 0x7FF);
        max_ports = (cap_regs->hcsparams1 >> 24) & 0xFF;

        isochronous_sheduling_threshold = ((cap_regs->hcsparams2) & 0xF);
        erst_max = ((cap_regs->hcsparams2 >> 4) & 0xF);

        bit64_addr_cap = ((cap_regs->hccparams1) & 0x1);
        bandwitdh_negotiation_cap = ((cap_regs->hccparams1 >> 1) & 0x1);
        byte64_context_size = ((cap_regs->hccparams1 >> 2) & 0x1);
        port_power_control = ((cap_regs->hccparams1 >> 3) & 0x1);
        port_indicators = ((cap_regs->hccparams1 >> 4) & 0x1);
        light_reset_cap = ((cap_regs->hccparams1 >> 5) & 0x1);
        ext_cap_offset = ((cap_regs->hccparams1 >> 16) & 0xFFFF) * sizeof(UINT32);
        scratchpad_count = ((cap_regs->hcsparams2 >> 27) & 0x1F) << 5 | ((cap_regs->hcsparams2 >> 21) & 0x1F);

        op_regs = (XHCIOperationalRegs*)(base_address + cap_regs->caplength);
        port_regs = (XHCIPortRegs*)(base_address + cap_regs->caplength + 0x400);
        doorbell_regs = (UINT32*)(base_address + cap_regs->dboff);

        // Update the base pointer to the runtime register set
        runtime_regs = (XHCIRuntimeRegs*)(base_address + cap_regs->rtsoff);

        return true;
    }

    void log_regs() {
        uart::printf("\n=== xHCI Cap regs ===\n\n");
        uart::printf("Length:                   %i\n", cap_regs_length);
        uart::printf("Max Device Slots:         %i\n", max_device_slots);
        uart::printf("Max interrupters:         %i\n", max_interrupters);
        uart::printf("Max ports:                %i\n", max_ports);
        uart::printf("Scratchpads:              %i\n", scratchpad_count);
        uart::printf("IST:                      %i\n", isochronous_sheduling_threshold);
        uart::printf("ERST Max size:            %i\n", erst_max);
        uart::printf("64-bit addressing:        %s\n", bit64_addr_cap ? "yes" : "no");
        uart::printf("Bandwidth negotiation:    %i\n", bandwitdh_negotiation_cap);
        uart::printf("64-byte context size:     %s\n", byte64_context_size ? "yes" : "no");
        uart::printf("Port power control:       %i\n", port_power_control);
        uart::printf("Port indicators:          %i\n", port_indicators);
        uart::printf("Light reset available:    %i\n", light_reset_cap);

        uart::printf("\n=== xHCI Ops regs ===\n\n");
        uart::printf("usbcmd     : %i\n", op_regs->usbcmd);
        uart::printf("usbsts     : %i\n", op_regs->usbsts);
        uart::printf("pagesize   : %i\n", op_regs->pagesize);
        uart::printf("dnctrl     : %i\n", op_regs->dnctrl);
        uart::printf("crcr       : %llx\n", op_regs->crcr);
        uart::printf("dcbaap     : %llx\n", op_regs->dcbaap);
        uart::printf("config     : %x\n\n", op_regs->config);
    }

    void log_usbsts() {
        UINT32 status = op_regs->usbsts;
        uart::printf("===== USBSTS =====\n");
        if (status & (1 << 0))  uart::printf("    Host Controlled Halted\n");
        if (status & (1 << 2))  uart::printf("    Host System Error\n");
        if (status & (1 << 3))  uart::printf("    Event Interrupt\n");
        if (status & (1 << 4))  uart::printf("    Port Change Detect\n");
        if (status & (1 << 8))  uart::printf("    Save State Status\n");
        if (status & (1 << 9))  uart::printf("    Restore State Status\n");
        if (status & (1 << 10)) uart::printf("    Save/Restore Error\n");
        if (status & (1 << 11)) uart::printf("    Controller Not Ready\n");
        if (status & (1 << 12)) uart::printf("    Host Controller Error\n");
        uart::printf("\n");
    }

    static inline void ring_command_doorbell() {
        // Target = 0 → Command Ring
        doorbell_regs[0] = 0;
    }

    static inline void ring_control_ep0_doorbell(UINT8 slot_id) {
        // DB[slot_id], target=1 -> Control Endpoint Ring
        doorbell_regs[slot_id] = 1;
    }

    static inline void ring_ep_doorbell(UINT8 slot_id, UINT8 dci) {
        doorbell_regs[slot_id] = dci;
    }

    static void command_ring_enqueue_trb(const XHCITrb& src) {
        XHCITrb trb = src;
        trb.cycle_bit = cmd_ring_cycle;

        command_ring[cmd_ring_enq] = trb;

        if (++cmd_ring_enq == COMMAND_RING_TRBS - 1) {
            // Update Link TRB
            command_ring[COMMAND_RING_TRBS - 1].control = (6 << 10) | (1 << 1) | cmd_ring_cycle;
            cmd_ring_enq = 0;
            cmd_ring_cycle = !cmd_ring_cycle;
        }
    }

    static bool event_ring_has_event() {
        return event_ring[event_ring_deq].cycle_bit == event_ring_cycle;
    }

    static XHCITrb* event_ring_dequeue() {
        if (!event_ring_has_event())
            return nullptr;

        XHCITrb* trb = &event_ring[event_ring_deq];

        if (++event_ring_deq == EVENT_RING_TRBS) {
            event_ring_deq = 0;
            event_ring_cycle ^= 1;
        }

        // Update ERDP
        volatile XHCIInterrupterRegs* ir = &runtime_regs->ir[0];
        UINT64 deq_phys =
            event_ring_phys_base +
            (event_ring_deq * sizeof(XHCITrb));

        ir->erdp = deq_phys | (1 << 3); // Event Handler Busy

        return trb;
    }

    static XHCITrb* send_command_trb(const XHCITrb& cmd) {
        // 1. enqueue
        command_ring_enqueue_trb(cmd);

        // 2. memory barrier (обязательно)
        asm volatile("mfence" ::: "memory");

        // 3. ring doorbell
        ring_command_doorbell();

        // 4. wait for completion
        for (int i = 0; i < 1000000; i++) {
            if (event_ring_has_event()) {
                XHCITrb* evt = event_ring_dequeue();
                if (!evt) continue;

                if (evt->trb_type == 0x21) {
                    return evt;
                }
            }
        }

        // for (;;) {
        //     if (op_regs->usbsts & (1 << 3)) { // EINT
        //         xhci_irq_handler();
        //         return nullptr;
        //     }
        // }

        return nullptr;
    }

    void acknowledge_irq(UINT8 interrupter) {
        // Clear the EINT bit in USBSTS by writting '1' to it
        op_regs->usbsts = (1 << 3);

        // Get the interrupter registers
        volatile XHCIInterrupterRegs* interrupter_regs = &runtime_regs->ir[interrupter];

        // Read the current value of IMAN
        UINT32 iman = interrupter_regs->iman;

        // Set the IP bit to '1' to clear it, preserve other bits including PE
        iman |= (1 << 0);

        // Write back to IMAN
        interrupter_regs->iman = iman;
    }

    void xhci_irq_handler() {
        while (event_ring_has_event()) {
            XHCITrb* trb = event_ring_dequeue();
            if (!trb) break;

            switch (trb->trb_type) {
                case 0x21:
                    uart::printf("xHCI: Command Completion Event\n");
                    break;
                case 0x22:
                    uart::printf("xHCI: Port Status Change Event\n");
                    break;
                default:
                    uart::printf("xHCI: Event TRB type %u\n", trb->trb_type);
                    break;
            }
        }

        acknowledge_irq(0);
    }

    static UINT16 ep0_max_packet_from_speed(UINT32 psiv) {
        // PSIV: 1=LS,2=FS,3=HS,4=SS (как ты уже используешь)
        switch (psiv) {
            case 1: return 8;     // Low Speed
            case 2: return 64;    // Full Speed
            case 3: return 64;    // High Speed
            case 4: return 512;   // SuperSpeed (control MPS обычно 512)
            default: return 64;
        }
    }

    static UINT32 get_port_speed(UINT32 portsc) {
        if (!(portsc & 0x1)) return 0;
        return (portsc >> 10) & 0xF;
    }

    static bool find_first_connected_port(UINT8* out_port, UINT32* out_speed) {
        for (UINT8 i = 0; i < max_ports; i++) {
            UINT32 portsc = port_regs[i].portsc;
            if (portsc & 0x1) { // CCS
                *out_port = (UINT8)(i + 1); // ports are 1-based in contexts
                *out_speed = get_port_speed(portsc);
                return true;
            }
        }
        return false;
    }

    static void ep0_ring_init() {
        memory::memset((UINT8*)ep0_ring, 0, sizeof(ep0_ring));
        ep0_enq = 0;
        ep0_cycle = 1;

        UINT64 ring_phys = paging::get_phys_addr((UINT64)&ep0_ring[0]);

        // Link TRB в конец, замыкаем на начало
        ep0_ring[EP0_RING_TRBS - 1].parameter = ring_phys;
        ep0_ring[EP0_RING_TRBS - 1].control = (6 << 10) | (1 << 1) | ep0_cycle; // type=Link, TC=1, cycle=1

        uart::printf("EP0 ring phys: %llx\n", ring_phys);
    }

    static void bulk_ring_init(XHCITrb* ring, UINT32 trbs, UINT32& enq, UINT8& cycle) {
        memory::memset((UINT8*)ring, 0, sizeof(XHCITrb) * trbs);
        enq = 0;
        cycle = 1;

        UINT64 phys = paging::get_phys_addr((UINT64)&ring[0]);
        ring[trbs - 1].parameter = phys;
        ring[trbs - 1].control = (6 << 10) | (1 << 1) | cycle;

        uart::printf("Bulk ring phys: %llx\n", phys);
    }

    static void bulk_ring_enqueue_trb(XHCITrb* ring, UINT32 trbs, UINT32& enq, UINT8& cycle, const XHCITrb& src) {
        XHCITrb trb = src;
        trb.cycle_bit = cycle;
        ring[enq] = trb;

        if (++enq == trbs - 1) {
            ring[trbs - 1].control = (6 << 10) | (1 << 1) | cycle; // Link
            enq = 0;
            cycle = !cycle;
        }
    }

    static bool wait_transfer_event(const char* tag) {
        for (int i = 0; i < 4000000; i++) {
            if (event_ring_has_event()) {
                XHCITrb* evt = event_ring_dequeue();
                if (!evt) continue;

                if (evt->trb_type == XHCI_TRB_TYPE_TRANSFER_EVENT) {
                    UINT8 cc = (evt->status >> 24) & 0xFF;
                    uart::printf("%s: Transfer Event cc=%u\n", tag, cc);
                    return (cc == 1);
                }
            }
        }
        uart::printf("%s: timeout waiting Transfer Event\n", tag);
        return false;
    }

    static bool bulk_out_xfer(UINT8 slot_id, void* buf, UINT32 len) {
        XHCITrb trb = {};
        trb.parameter = paging::get_phys_addr((UINT64)buf);
        trb.status = len;
        trb.trb_type = XHCI_TRB_TYPE_NORMAL;
        trb.control |= (1u << 5); // IOC

        bulk_ring_enqueue_trb(bulk_out_ring, BULK_RING_TRBS, bulk_out_enq, bulk_out_cycle, trb);
        asm volatile("mfence" ::: "memory");
        ring_ep_doorbell(slot_id, g_bulk_ep_out_dci);
        return wait_transfer_event("BULK OUT");
    }

    static bool bulk_in_xfer(UINT8 slot_id, void* buf, UINT32 len) {
        XHCITrb trb = {};
        trb.parameter = paging::get_phys_addr((UINT64)buf);
        trb.status = len;
        trb.trb_type = XHCI_TRB_TYPE_NORMAL;
        trb.control |= (1u << 5); // IOC

        bulk_ring_enqueue_trb(bulk_in_ring, BULK_RING_TRBS, bulk_in_enq, bulk_in_cycle, trb);
        asm volatile("mfence" ::: "memory");
        ring_ep_doorbell(slot_id, g_bulk_ep_in_dci);
        return wait_transfer_event("BULK IN");
    }

    static UINT8 ep_addr_to_dci(UINT8 ep_addr) {
        UINT8 ep_num = ep_addr & 0x0F;
        bool is_in = (ep_addr & 0x80) != 0;
        return (ep_num * 2) + (is_in ? 1 : 0);
    }

    static void ep0_ring_enqueue_trb(const XHCITrb& src) {
        XHCITrb trb = src;
        trb.cycle_bit = ep0_cycle;

        ep0_ring[ep0_enq] = trb;

        if (++ep0_enq == EP0_RING_TRBS - 1) {
            ep0_ring[EP0_RING_TRBS - 1].control = (6 << 10) | (1 << 1) | ep0_cycle; // Link TRB
            ep0_enq = 0;
            ep0_cycle = !ep0_cycle;
        }
    }

    static void* input_ctx_ptr(UINT32 index) {
        // index 0 = Input Control Context
        return (void*)(input_context_mem + (index * ctx_stride()));
    }

    static void* device_ctx_ptr(UINT32 index) {
        // Device Context: index 0 = Slot Context, 1 = EP0, ...
        return (void*)(device_context_mem + (index * ctx_stride()));
    }

    static bool address_device(UINT8 slot_id, UINT8 port_id, UINT32 port_speed) {
        uart::printf("=== Address Device: slot=%u port=%u speed=%u ===\n", slot_id, port_id, port_speed);

        // 1) Инициализируем EP0 ring (TR Dequeue Pointer обязан быть валиден)
        ep0_ring_init();
        UINT64 ep0_ring_phys = paging::get_phys_addr((UINT64)&ep0_ring[0]);

        // 2) Чистим контексты
        memory::memset((UINT8*)input_context_mem, 0, sizeof(input_context_mem));
        memory::memset((UINT8*)device_context_mem, 0, sizeof(device_context_mem));

        // 3) Прописываем DCBAA entry для slot_id -> Device Context phys
        UINT64 dev_ctx_phys = paging::get_phys_addr((UINT64)&device_context_mem[0]);
        dcbaa[slot_id] = dev_ctx_phys;

        uart::printf("DeviceCtx phys: %llx -> DCBAA[%u]\n", dev_ctx_phys, slot_id);

        // 4) Input Control Context: add slot + ep0
        XHCIInputControlContext* icc = (XHCIInputControlContext*)input_ctx_ptr(0);
        icc->drop_context_flags = 0;
        icc->add_context_flags = (1u << 0) | (1u << 1); // slot ctx + ep0 ctx

        // 5) Slot Context (Input Context slot ctx = index 1)
        XHCISlotContext* slot = (XHCISlotContext*)input_ctx_ptr(1);

        // dw0:
        // bits 27..31 = Context Entries (сколько endpoint context активно). Нужно минимум 1 (т.е. EP0).
        // bits 20..23 = Speed (порт-скорость)
        // Остальное оставляем 0.
        slot->dw0 = 0;
        slot->dw0 |= ((1u & 0x1F) << 27);          // Context Entries = 1
        slot->dw0 |= ((port_speed & 0xF) << 20);   // Speed

        // dw1:
        // bits 16..23 = Root Hub Port Number
        slot->dw1 = 0;
        slot->dw1 |= ((UINT32)port_id << 16);

        // 6) EP0 Endpoint Context (Input Context ep0 ctx = index 2)
        XHCIEndpointContext* ep0 = (XHCIEndpointContext*)input_ctx_ptr(2);

        UINT16 mps = ep0_max_packet_from_speed(port_speed);

        // dw1:
        // bits 16..31 = Max Packet Size
        // bits  3..5  = EP Type (Control = 4)
        // Для простоты выставим только EP Type и MPS.
        ep0->dw1 = 0;
        ep0->dw1 |= ((UINT32)mps << 16);
        ep0->dw1 |= (4u << 3); // EP Type = Control

        // TR Dequeue Pointer: физ адрес ринга + DCS (bit0)
        UINT64 trdp = (ep0_ring_phys & ~0xFULL) | (ep0_cycle & 1);
        ep0->tr_dequeue_ptr_lo = (UINT32)(trdp & 0xFFFFFFFF);
        ep0->tr_dequeue_ptr_hi = (UINT32)((trdp >> 32) & 0xFFFFFFFF);

        uart::printf("EP0 MPS=%u TRDP=%llx\n", mps, trdp);

        // 7) Формируем Address Device Command TRB
        XHCITrb cmd = {};
        cmd.parameter = paging::get_phys_addr((UINT64)&input_context_mem[0]);
        cmd.status = 0;
        cmd.trb_type = XHCI_TRB_TYPE_ADDRESS_DEVICE_CMD;

        // slot id у Address Device кладётся в control[31:24] (как и в completion event)
        cmd.control |= ((UINT32)slot_id << 24);

        uart::printf("InputCtx phys: %llx\n", cmd.parameter);

        // 8) Шлём команду и ждём completion
        XHCITrb* completion = send_command_trb(cmd);
        if (!completion) {
            uart::printf("Address Device FAILED: no completion\n");
            return false;
        }

        UINT8 cc = (completion->status >> 24) & 0xFF;
        UINT8 sid = (completion->control >> 24) & 0xFF;

        uart::printf("Address Device completion: cc=%u slot_id=%u\n", cc, sid);

        // 1 = SUCCESS
        if (cc != 1) {
            uart::printf("Address Device FAILED: completion_code=%u\n", cc);
            return false;
        }

        return true;
    }

    static bool ep0_control_transfer(UINT8 slot_id, const USBSetupPacket& setup, void* data_buf) {
        UINT64 data_phys  = data_buf ? paging::get_phys_addr((UINT64)data_buf) : 0;

        bool has_data = (setup.wLength != 0);
        bool dir_in = (setup.bmRequestType & 0x80) != 0;

        // TRT: 0 = no data, 2 = IN, 3 = OUT
        UINT32 trt = 0;
        if (!has_data) trt = 0;
        else trt = dir_in ? 2 : 3;

        // Упаковка 8 байт setup packet прямо в parameter (little-endian)
        UINT32 p0 =
            ((UINT32)setup.bmRequestType) |
            ((UINT32)setup.bRequest << 8) |
            ((UINT32)setup.wValue << 16);

        UINT32 p1 =
            ((UINT32)setup.wIndex) |
            ((UINT32)setup.wLength << 16);

        UINT64 setup_param = ((UINT64)p1 << 32) | (UINT64)p0;

        uart::printf("EP0 setup: bm=%02x req=%02x val=%04x idx=%04x len=%u TRT=%u\n",
            setup.bmRequestType, setup.bRequest, setup.wValue, setup.wIndex, setup.wLength, trt);

        // 1) Setup Stage TRB
        XHCITrb setup_trb = {};
        setup_trb.parameter = setup_param;
        setup_trb.status = 8; // length = 8
        setup_trb.trb_type = XHCI_TRB_TYPE_SETUP_STAGE;
        setup_trb.control |= (trt & 0x3) << 16;
        setup_trb.control |= (1u << 6); // IDT=1 (Setup packet is immediate data)

        // 2) Data Stage TRB (если есть)
        XHCITrb data_trb = {};
        if (has_data) {
            data_trb.parameter = data_phys;
            data_trb.status = setup.wLength;
            data_trb.trb_type = XHCI_TRB_TYPE_DATA_STAGE;
            if (dir_in) data_trb.control |= (1u << 16); // DIR=IN
        }

        // 3) Status Stage TRB
        XHCITrb status_trb = {};
        status_trb.parameter = 0;
        status_trb.status = 0;
        status_trb.trb_type = XHCI_TRB_TYPE_STATUS_STAGE;

        // status stage direction opposite of data stage
        if (has_data) {
            if (!dir_in) status_trb.control |= (1u << 16); // data OUT -> status IN
        } else {
            status_trb.control |= (1u << 16); // no-data -> status IN
        }

        // IOC обязательно
        status_trb.control |= (1u << 5);

        // enqueue в EP0 ring
        ep0_ring_enqueue_trb(setup_trb);
        if (has_data) ep0_ring_enqueue_trb(data_trb);
        ep0_ring_enqueue_trb(status_trb);

        asm volatile("mfence" ::: "memory");
        ring_control_ep0_doorbell(slot_id);

        for (int i = 0; i < 3000000; i++) {
            if (event_ring_has_event()) {
                XHCITrb* evt = event_ring_dequeue();
                if (!evt) continue;

                UINT8 t  = (UINT8)evt->trb_type;
                UINT8 cc = (UINT8)((evt->status >> 24) & 0xFF);

                uart::printf("EVT: type=%u cc=%u\n", t, cc);

                if (t == XHCI_TRB_TYPE_TRANSFER_EVENT) {
                    return (cc == 1);
                }
                // если придёт PORT_STATUS_CHANGE или что-то ещё — ты это увидишь
            }
        }

        uart::printf("EP0 transfer timeout (no Transfer Event)\n");
        return false;
    }

    static bool get_device_descriptor(UINT8 slot_id) {
        static UINT8 __attribute__((aligned(16))) dev_desc[18];
        memory::memset(dev_desc, 0, sizeof(dev_desc));

        USBSetupPacket s = {};
        s.bmRequestType = 0x80; // IN, Standard, Device
        s.bRequest = USB_REQ_GET_DESCRIPTOR;
        s.wValue = (USB_DESC_DEVICE << 8) | 0;
        s.wIndex = 0;
        s.wLength = sizeof(dev_desc);

        uart::printf("GET_DESCRIPTOR(Device)\n");
        if (!ep0_control_transfer(slot_id, s, dev_desc)) {
            uart::printf("Device descriptor FAILED\n");
            return false;
        }

        uart::printf("DeviceDesc: ");
        for (int i = 0; i < 18; i++) uart::printf("%02x ", dev_desc[i]);
        uart::printf("\n");

        UINT16 vid = (UINT16)(dev_desc[8] | (dev_desc[9] << 8));
        UINT16 pid = (UINT16)(dev_desc[10] | (dev_desc[11] << 8));
        uart::printf("VID=%04x PID=%04x\n", vid, pid);

        return true;
    }

    static bool get_config_descriptor_header(UINT8 slot_id, UINT16* out_total_len) {
        static UINT8 __attribute__((aligned(16))) cfg_hdr[9];
        memory::memset(cfg_hdr, 0, sizeof(cfg_hdr));

        USBSetupPacket s = {};
        s.bmRequestType = 0x80; // IN, Standard, Device
        s.bRequest = USB_REQ_GET_DESCRIPTOR;
        s.wValue = (USB_DESC_CONFIGURATION << 8) | 0;
        s.wIndex = 0;
        s.wLength = sizeof(cfg_hdr);

        uart::printf("GET_DESCRIPTOR(Config header)\n");
        if (!ep0_control_transfer(slot_id, s, cfg_hdr)) {
            uart::printf("Config header FAILED\n");
            return false;
        }

        uart::printf("CfgHdr: ");
        for (int i = 0; i < 9; i++) uart::printf("%02x ", cfg_hdr[i]);
        uart::printf("\n");

        UINT16 total = (UINT16)(cfg_hdr[2] | (cfg_hdr[3] << 8));
        uart::printf("wTotalLength=%u\n", total);
        *out_total_len = total;
        return true;
    }

    static bool get_full_config_descriptor(UINT8 slot_id, UINT16 total_len, UINT8* out_buf) {
        memory::memset(out_buf, 0, total_len);

        USBSetupPacket s = {};
        s.bmRequestType = 0x80;
        s.bRequest = USB_REQ_GET_DESCRIPTOR;
        s.wValue = (USB_DESC_CONFIGURATION << 8) | 0;
        s.wIndex = 0;
        s.wLength = total_len;

        uart::printf("GET_DESCRIPTOR(Config full, %u bytes)\n", total_len);
        if (!ep0_control_transfer(slot_id, s, out_buf)) {
            uart::printf("Full config FAILED\n");
            return false;
        }
        uart::printf("Full config OK\n");
        return true;
    }

    static bool set_configuration(UINT8 slot_id, UINT8 config_value) {
        USBSetupPacket s = {};
        s.bmRequestType = 0x00; // OUT, Standard, Device
        s.bRequest = USB_REQ_SET_CONFIGURATION;
        s.wValue = config_value;
        s.wIndex = 0;
        s.wLength = 0;

        uart::printf("SET_CONFIGURATION(%u)\n", config_value);
        bool ok = ep0_control_transfer(slot_id, s, nullptr);
        uart::printf("SET_CONFIGURATION %s\n", ok ? "OK" : "FAILED");
        return ok;
    }

    static bool parse_mass_storage_and_bulk_eps(
        const UINT8* cfg, UINT16 len,
        UINT8* out_ifnum,
        UINT8* out_ep_in_addr,
        UINT8* out_ep_out_addr,
        UINT16* out_mps_in,
        UINT16* out_mps_out
    ) {
        *out_ifnum = 0xFF;
        *out_ep_in_addr = 0;
        *out_ep_out_addr = 0;
        *out_mps_in = 0;
        *out_mps_out = 0;

        bool in_ms_interface = false;

        for (UINT16 i = 0; i + 2 <= len; ) {
            UINT8 bLength = cfg[i + 0];
            UINT8 bType   = cfg[i + 1];

            if (bLength == 0 || (i + bLength) > len) break;

            if (bType == 4 && bLength >= 9) { // INTERFACE
                UINT8 ifnum   = cfg[i + 2];
                UINT8 alt     = cfg[i + 3];
                UINT8 cls     = cfg[i + 5];
                UINT8 subcls  = cfg[i + 6];
                UINT8 proto   = cfg[i + 7];

                // Mass Storage: class 0x08, subclass 0x06 (SCSI), protocol 0x50 (BOT)
                in_ms_interface = (alt == 0 && cls == 0x08 && subcls == 0x06 && proto == 0x50);

                if (in_ms_interface) {
                    *out_ifnum = ifnum;
                    uart::printf("MassStorage IF found: if=%u\n", ifnum);
                }
            }
            else if (bType == 5 && bLength >= 7) { // ENDPOINT
                if (in_ms_interface) {
                    UINT8  ep_addr = cfg[i + 2];     // bEndpointAddress
                    UINT8  attrs   = cfg[i + 3];     // bmAttributes
                    UINT16 mps     = (UINT16)(cfg[i + 4] | (cfg[i + 5] << 8));

                    UINT8 transfer_type = attrs & 0x3;
                    bool is_in = (ep_addr & 0x80) != 0;

                    // Bulk endpoints only
                    if (transfer_type == 2) {
                        if (is_in && *out_ep_in_addr == 0) {
                            *out_ep_in_addr = ep_addr;
                            *out_mps_in = mps;
                            uart::printf("Bulk IN  EP: addr=0x%02x mps=%u\n", ep_addr, mps);
                        } else if (!is_in && *out_ep_out_addr == 0) {
                            *out_ep_out_addr = ep_addr;
                            *out_mps_out = mps;
                            uart::printf("Bulk OUT EP: addr=0x%02x mps=%u\n", ep_addr, mps);
                        }
                    }
                }
            }

            i = (UINT16)(i + bLength);
        }

        if (*out_ifnum == 0xFF || *out_ep_in_addr == 0 || *out_ep_out_addr == 0) {
            uart::printf("MassStorage/Bulk EPs not found\n");
            return false;
        }
        return true;
    }

    static bool configure_bulk_endpoints(
        UINT8 slot_id,
        UINT8 ep_in_addr,
        UINT8 ep_out_addr,
        UINT16 mps_in,
        UINT16 mps_out
    ) {
        uart::printf("=== Configure Bulk Endpoints ===\n");

        g_bulk_ep_in_dci  = ep_addr_to_dci(ep_in_addr);
        g_bulk_ep_out_dci = ep_addr_to_dci(ep_out_addr);

        uart::printf("Bulk IN  DCI=%u\n", g_bulk_ep_in_dci);
        uart::printf("Bulk OUT DCI=%u\n", g_bulk_ep_out_dci);

        bulk_ring_init(bulk_in_ring,  BULK_RING_TRBS, bulk_in_enq,  bulk_in_cycle);
        bulk_ring_init(bulk_out_ring, BULK_RING_TRBS, bulk_out_enq, bulk_out_cycle);

        UINT64 bulk_in_phys  = paging::get_phys_addr((UINT64)&bulk_in_ring[0]);
        UINT64 bulk_out_phys = paging::get_phys_addr((UINT64)&bulk_out_ring[0]);

        memory::memset(input_context_mem, 0, sizeof(input_context_mem));

        XHCIInputControlContext* icc = (XHCIInputControlContext*)input_ctx_ptr(0);
        icc->drop_context_flags = 0;
        icc->add_context_flags =
            (1u << 0) |
            (1u << g_bulk_ep_in_dci) |
            (1u << g_bulk_ep_out_dci);

        XHCISlotContext* slot_out = (XHCISlotContext*)device_ctx_ptr(0);
        XHCISlotContext* slot_in = (XHCISlotContext*)input_ctx_ptr(1);
        *slot_in = *slot_out;

        // Bulk IN Endpoint Context
        XHCIEndpointContext* ep_in =
            (XHCIEndpointContext*)input_ctx_ptr(g_bulk_ep_in_dci + 1);

        ep_in->dw1 = (mps_in << 16) | (15 << 8) | (6 << 3); // Bulk IN
        UINT64 trdp_in = (bulk_in_phys & ~0xFULL) | bulk_in_cycle;
        ep_in->tr_dequeue_ptr_lo = (UINT32)(trdp_in & 0xFFFFFFFF);
        ep_in->tr_dequeue_ptr_hi = (UINT32)(trdp_in >> 32);

        // Bulk OUT Endpoint Context
        XHCIEndpointContext* ep_out = (XHCIEndpointContext*)input_ctx_ptr(g_bulk_ep_out_dci + 1);

        ep_out->dw1 = (mps_out << 16) | (15 << 8) | (2 << 3); // Bulk OUT
        UINT64 trdp_out = (bulk_out_phys & ~0xFULL) | bulk_out_cycle;
        ep_out->tr_dequeue_ptr_lo = (UINT32)(trdp_out & 0xFFFFFFFF);
        ep_out->tr_dequeue_ptr_hi = (UINT32)(trdp_out >> 32);

        XHCITrb cmd = {};
        cmd.parameter = paging::get_phys_addr((UINT64)&input_context_mem[0]);
        cmd.trb_type = XHCI_TRB_TYPE_CONFIGURE_ENDPOINT_CMD;
        cmd.control |= ((UINT32)slot_id << 24);

        XHCITrb* evt = send_command_trb(cmd);
        if (!evt) {
            uart::printf("Configure Endpoint FAILED: no event\n");
            return false;
        }

        UINT8 cc = (evt->status >> 24) & 0xFF;
        uart::printf("Configure Endpoint completion=%u\n", cc);

        return (cc == 1);
    }

    static void reset_controller() {
        // Stop controller
        op_regs->usbcmd &= ~0x1;
        for (int i = 0; i < 1000000 && !(op_regs->usbsts & 0x1); i++);

        // Reset controller
        op_regs->usbcmd |= 0x2;
        for (int i = 0; i < 1000000 && (op_regs->usbcmd & 0x2); i++);
        for (int i = 0; i < 1000000 && (op_regs->usbsts & 0x800); i++);
    }

    static void config_op_regs() {
        // Device notifications
        op_regs->dnctrl = 0xFFFF;

        // Configure usbconfig field
        op_regs->config = static_cast<UINT32>(max_device_slots);

        // DCBAA
        //size_t dcbaa_size = sizeof(uintptr_t) * (max_device_slots + 1);
        memory::memset((UINT8*)dcbaa, 0, sizeof(UINT64) * 256);

        // Scratchpads
        if (scratchpad_count > 0) {
            if (scratchpad_count > MAX_SCRATCHPADS) {
                scratchpad_count = MAX_SCRATCHPADS;
            }

            for (UINT32 i = 0; i < scratchpad_count; i++) {
                scratchpad_array[i] = paging::get_phys_addr((UINT64)&scratchpad_pages[i][0]);
            }

            dcbaa[0] = paging::get_phys_addr((UINT64)&scratchpad_array[0]);
        }

        // Setup the device context base address array with scratchpad buffers
        op_regs->dcbaap = paging::get_phys_addr((UINT64)&dcbaa[0]);

        // Setup the command ring and write CRCM
        memory::memset((UINT8*)command_ring, 0, sizeof(command_ring));
        cmd_ring_enq = 0;
        cmd_ring_cycle = 1;

        UINT64 cr_phys = paging::get_phys_addr((UINT64)&command_ring[0]);

        // Link TRB
        command_ring[COMMAND_RING_TRBS - 1].parameter = cr_phys;
        command_ring[COMMAND_RING_TRBS - 1].control = (6 << 10) | (1 << 1) | cmd_ring_cycle;

        // CRCR: physical address + RCS
        op_regs->crcr = cr_phys | cmd_ring_cycle;
    }

    static void config_runtime_regs() {
        // Get the primary interrupter registers
        volatile XHCIInterrupterRegs* interrupter_regs = &runtime_regs->ir[0];

        // Enable interrupts
        UINT32 iman = interrupter_regs->iman;
        iman |= (1 << 1);
        interrupter_regs->iman = iman;

        // Setup the event ring and write to interrupter
        // registers to set ERSTZ, ERDP, and ERSTBA
        memory::memset((UINT8*)event_ring, 0, sizeof(event_ring));
        memory::memset((UINT8*)event_ring_erst, 0, sizeof(event_ring_erst));
        event_ring_deq = 0;
        event_ring_cycle = 1;

        event_ring_phys_base = paging::get_phys_addr((UINT64)&event_ring[0]);
        erst_phys_base = paging::get_phys_addr((UINT64)&event_ring_erst[0]);

        // ERST entry
        event_ring_erst[0].ring_segment_base_addr = event_ring_phys_base;
        event_ring_erst[0].ring_segment_size = EVENT_RING_TRBS;
        event_ring_erst[0].rsvd = 0;

        // Program interrupter registers
        interrupter_regs->erstsz = EVENT_RING_SEGMENTS;
        interrupter_regs->erstba = erst_phys_base;
        // ERDP = dequeue pointer
        interrupter_regs->erdp = event_ring_phys_base;

        // Clear any pending interrupts for the primary interrupter
        acknowledge_irq(0);
    }

    static bool msc_command_in(UINT8 slot_id, const UINT8* cdb, UINT8 cdb_len, void* data, UINT32 data_len) {
        static MSC_CBW __attribute__((aligned(16))) cbw;
        static MSC_CSW __attribute__((aligned(16))) csw;

        memory::memset((UINT8*)&cbw, 0, sizeof(cbw));
        memory::memset((UINT8*)&csw, 0, sizeof(csw));

        cbw.dCBWSignature = 0x43425355;
        cbw.dCBWTag = g_msc_tag++;
        cbw.dCBWDataTransferLength = data_len;
        cbw.bmCBWFlags = 0x80;
        cbw.bCBWLUN = 0;
        cbw.bCBWCBLength = cdb_len;
        for (UINT8 i = 0; i < cdb_len; i++) cbw.CBWCB[i] = cdb[i];

        uart::printf("MSC IN cmd: tag=%u data_len=%u\n", cbw.dCBWTag, data_len);

        if (!bulk_out_xfer(slot_id, &cbw, 31)) return false;
        if (data_len && !bulk_in_xfer(slot_id, data, data_len)) return false;
        if (!bulk_in_xfer(slot_id, &csw, 13)) return false;

        uart::printf("CSW: sig=%08x tag=%u status=%u\n", csw.dCSWSignature, csw.dCSWTag, csw.bCSWStatus);
        return (csw.dCSWSignature == 0x53425355) && (csw.dCSWTag == cbw.dCBWTag) && (csw.bCSWStatus == 0);
    }

    static UINT32 be32(const UINT8* p) {
        return ((UINT32)p[0] << 24) | ((UINT32)p[1] << 16) | ((UINT32)p[2] << 8) | (UINT32)p[3];
    }

    static bool msc_inquiry(UINT8 slot_id) {
        static UINT8 __attribute__((aligned(16))) buf[36];
        memory::memset(buf, 0, sizeof(buf));

        UINT8 cdb[6] = { 0x12, 0x00, 0x00, 0x00, 36, 0x00 }; // INQUIRY
        uart::printf("SCSI INQUIRY\n");
        if (!msc_command_in(slot_id, cdb, 6, buf, 36)) return false;

        uart::printf("INQ: ");
        for (int i = 0; i < 36; i++) uart::printf("%02x ", buf[i]);
        uart::printf("\n");
        return true;
    }

    static bool msc_read_capacity(UINT8 slot_id, UINT32* out_block_size) {
        static UINT8 __attribute__((aligned(16))) buf[8];
        memory::memset(buf, 0, sizeof(buf));

        UINT8 cdb[10] = { 0x25,0,0,0,0,0,0,0,0,0 }; // READ CAPACITY(10)
        uart::printf("SCSI READ CAPACITY(10)\n");
        if (!msc_command_in(slot_id, cdb, 10, buf, 8)) return false;

        UINT32 last_lba = be32(&buf[0]);
        UINT32 blk_sz   = be32(&buf[4]);
        uart::printf("Capacity: last_lba=%u block_size=%u\n", last_lba, blk_sz);
        *out_block_size = blk_sz;
        return true;
    }

    static bool msc_read10_lba0(UINT8 slot_id, void* dst, UINT32 block_size) {
        // читаем 1 блок (обычно 512)
        UINT8 cdb[10] = {0};
        cdb[0] = 0x28; // READ(10)
        // LBA = 0 -> already 0
        cdb[7] = 0;
        cdb[8] = 1;    // transfer length = 1 block

        uart::printf("SCSI READ(10) LBA=0 -> %p (%u bytes)\n", dst, block_size);
        return msc_command_in(slot_id, cdb, 10, dst, block_size);
    }

    static bool start_controller() {
        op_regs->usbcmd |= (1 << 0);
        op_regs->usbcmd |= (1 << 2);
        for (int i = 0; i < 1000000 && (op_regs->usbsts & (1 << 0)); i++);
        
        if (op_regs->usbsts & (1 << 11)) {
            return false;
        }

        return true;
    }

    static void reset_ports() {
        for (UINT8 i = 0; i < max_ports; i++) {
            UINT32* portsc = &port_regs[i].portsc;
            if ((*portsc & 0x1) && !(*portsc & (1 << 4))) {
                *portsc |= (1 << 4); // PR
                for (int j = 0; j < 100000 && (*portsc & (1 << 21)); j++);
            }
        }
    }

    static void scan_ports() {
        device_list.length = 0;
        if (!port_regs || max_ports == 0) return;

        for (UINT8 i = 0; i < max_ports; i++) {
            UINT32 portsc = port_regs[i].portsc;

            USBDevice* device = &device_list.devices[device_list.length];
            device->port_number = i + 1;
            device->speed = get_port_speed(portsc);
            device->connected = (portsc & 0x1) ? true : false;
            device->vendor_id = 0;
            device->device_id = 0;

            if (device->connected) device_list.length++;
        }
    }    

    void init() {
        controller_pci = find_xhci_controller();
        if (!controller_pci) return;

        if (!map_registers()) return;

        reset_controller();
        config_op_regs();
        config_runtime_regs();

        if (!start_controller()) return;

        XHCITrb cmd = {};
        cmd.trb_type = 9;

        XHCITrb* completion = send_command_trb(cmd);
        UINT8 slot_id;

        if (!completion) {
            uart::printf("Enable Slot FAILED: no completion\n");
        } else {
            UINT8 completion_code = (completion->status >> 24) & 0xFF;
            slot_id               = (completion->control >> 24) & 0xFF;

            uart::printf(
                "Enable Slot OK: completion=%u slot_id=%u\n",
                completion_code,
                slot_id
            );
        }

        // Найдём порт с подключенным устройством
        if (!find_first_connected_port(&g_port_id, &g_port_speed)) {
            uart::printf("No connected ports found\n");
            return;
        }
        uart::printf("Connected port: %u speed=%u\n", g_port_id, g_port_speed);

        // Address Device
        if (!address_device(slot_id, g_port_id, g_port_speed)) {
            uart::printf("Address Device FAILED\n");
            return;
        }

        uart::printf("Address Device OK\n\n");

        if (!get_device_descriptor(slot_id)) return;

        UINT16 total_len = 0;
        if (!get_config_descriptor_header(slot_id, &total_len)) return;

        uart::printf("Step2 OK (descriptors)\n\n");

        static UINT8 __attribute__((aligned(16))) cfg_full[256];

        if (total_len > sizeof(cfg_full)) {
            uart::printf("Config too big: %u\n", total_len);
            return;
        }

        if (!get_full_config_descriptor(slot_id, total_len, cfg_full)) return;

        UINT8 ifnum = 0xFF;
        UINT8 ep_in = 0, ep_out = 0;
        UINT16 mps_in = 0, mps_out = 0;

        if (!parse_mass_storage_and_bulk_eps(cfg_full, total_len, &ifnum, &ep_in, &ep_out, &mps_in, &mps_out)) {
            uart::printf("Step3 FAILED (no bulk endpoints)\n");
            return;
        }

        uart::printf("Step3 OK: if=%u bulk_in=0x%02x bulk_out=0x%02x\n\n", ifnum, ep_in, ep_out);

        if (!configure_bulk_endpoints(slot_id, ep_in, ep_out, mps_in, mps_out)) {
            uart::printf("Step4 FAILED (configure endpoints)\n");
            return;
        }

        uart::printf("Step4 OK (bulk endpoints ready)\n\n");

        UINT8 config_value = cfg_full[5];
        uart::printf("bConfigurationValue=%u\n", config_value);

        if (!set_configuration(slot_id, config_value)) return;
        
        uart::printf("Step5 OK!\n\n");

        UINT32 block_size = 512;

        if (!msc_inquiry(slot_id)) {
            uart::printf("MSC INQUIRY FAILED\n");
            return;
        }

        if (!msc_read_capacity(slot_id, &block_size)) {
            uart::printf("READ CAPACITY FAILED\n");
            return;
        }

        // Читаем LBA0 в 0x150000
        if (!msc_read10_lba0(slot_id, (void*)0x150000, block_size)) {
            uart::printf("READ(10) LBA0 FAILED\n");
            return;
        }

        uart::printf("READ(10) OK. Sector at 0x150000\n");

        // reset_ports();
        // scan_ports();
    }

    UINT32 device_count() {
        scan_ports();
        return device_list.length;
    }

    USBDevice* get_device(UINT32 idx) {
        if (idx >= device_list.length) return nullptr;
        return &device_list.devices[idx];
    }

    USBDeviceList* get_device_list() {
        scan_ports();
        return &device_list;
    }

    const char* get_speed_name(UINT32 speed) {
        switch (speed) {
            case 0: return "Disconnected";
            case 1: return "Low Speed";
            case 2: return "Full Speed";
            case 3: return "High Speed";
            case 4: return "Super Speed";
            default: return "Unknown";
        }
    }
}