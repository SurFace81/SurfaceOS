#include "../../include/drivers/uart.h"
#include "../../include/cpu/paging.h"
#include "../../include/drivers/screen.h"

namespace uart {

    enum Backend { NONE, PCI_IO, PCI_MMIO };
    static Backend backend = NONE;
    static bool initialized = false;

    static uint16_t io_base = 0;
    static volatile uint8_t* mmio_base = nullptr;
    static uint32_t reg_stride = 1;

    enum Reg : uint8_t {
        THR = 0, IER = 1, FCR = 2, LCR = 3,
        MCR = 4, LSR = 5, MSR = 6, SCR = 7,
    };

    static inline void reg_write(uint8_t reg, uint8_t val) {
        if (backend == PCI_IO)
            port::byte_out(io_base + reg, val);
        else
            mmio_base[reg * reg_stride] = val;
    }

    static inline uint8_t reg_read(uint8_t reg) {
        if (backend == PCI_IO)
            return port::byte_in(io_base + reg);
        else
            return mmio_base[reg * reg_stride];
    }

    static void pci_enable_device(PCIDevice* dev) {
        uint32_t addr = (1U << 31)
                      | ((uint32_t)dev->bus << 16)
                      | ((uint32_t)dev->device << 11)
                      | ((uint32_t)dev->function << 8)
                      | (PCI_COMMAND & 0xFC);
        port::dword_out(PCI_CONFIG_ADDRESS, addr);
        uint32_t val = port::dword_in(PCI_CONFIG_DATA);
        uint16_t cmd = val & 0xFFFF;
        cmd |= (1 << 0) | (1 << 1) | (1 << 2);
        val = (val & 0xFFFF0000) | cmd;
        port::dword_out(PCI_CONFIG_ADDRESS, addr);
        port::dword_out(PCI_CONFIG_DATA, val);
    }

    static bool init_pci() {
        for (uint32_t i = 0; i < pci::device_count(); i++) {
            PCIDevice* d = pci::get_by_id(i);
            if (!d || !d->valid) continue;
            if (d->class_code != 0x07 || d->subclass != 0x00) continue;

            uint32_t bar0 = d->bar[0];
            if (!bar0) continue;

            pci_enable_device(d);

            if (bar0 & 1) {
                io_base = bar0 & ~0x3;
                reg_stride = 1;
                backend = PCI_IO;
            } else {
                uint8_t bar_type = (bar0 >> 1) & 0x3;
                uint64_t base_addr = 0;

                if (bar_type == 0x00)
                    base_addr = bar0 & 0xFFFFFFF0ULL;
                else if (bar_type == 0x02)
                    base_addr = ((uint64_t)d->bar[1] << 32) | (bar0 & 0xFFFFFFF0ULL);
                else
                    continue;

                if (base_addr == 0) continue;

                uint64_t page_base = base_addr & ~(0x200000ULL - 1);
                paging::allocate_pages(page_base, page_base, 1);

                mmio_base = (volatile uint8_t*)base_addr;
                reg_stride = 4;
                backend = PCI_MMIO;
            }
            return true;
        }
        return false;
    }

    static void hw_init() {
        reg_write(IER, 0x00);
        reg_write(LCR, 0x80);
        reg_write(THR, 0x01);
        reg_write(IER, 0x00);
        reg_write(LCR, 0x03);
        reg_write(FCR, 0xC7);
        reg_write(MCR, 0x0B);
    }

    int init() {
        if (!init_pci()) {
            return 1;
        }

        hw_init();
        initialized = true;

        reg_write(SCR, 0x55);
        uint8_t test = reg_read(SCR);

        return 0;
    }

    static void send_char(char c) {
        if (!initialized) return;
        while (!(reg_read(LSR) & 0x20));
        reg_write(THR, c);
    }

    static void write_str(const char* str) {
        while (*str) send_char(*str++);
    }

    void listen_loop() {
        if (!initialized) return;
        printf("UART listening...\n");
        screen::printf("UART listening...\n\r");

        while (1) {
            if (reg_read(LSR) & 0x01) {
                char c = reg_read(THR);
                char buf[2] = {c, '\0'};
                screen::write(buf);
                printf("%c", c);
            }
        }
    }

    static void log_uint64_hex(uint64_t n) {
        char b[19]; b[0]='0'; b[1]='x'; b[18]='\0';
        for (int i=17; i>=2; i--) { b[i]="0123456789ABCDEF"[n&0xF]; n>>=4; }
        write_str(b);
    }

    static void log_uint32_hex(uint32_t n) {
        char b[11]; b[0]='0'; b[1]='x'; b[10]='\0';
        for (int i=9; i>=2; i--) { b[i]="0123456789ABCDEF"[n&0xF]; n>>=4; }
        write_str(b);
    }

    static void log_uint64_dec(uint64_t n) {
        char b[21]; int i=19; b[20]='\0';
        if (!n) b[i--]='0';
        else while(n) { b[i--]='0'+(n%10); n/=10; }
        write_str(&b[i+1]);
    }

    static void log_uint32_dec(uint32_t n) {
        char b[12]; int i=10; b[11]='\0';
        if (!n) b[i--]='0';
        else while(n) { b[i--]='0'+(n%10); n/=10; }
        write_str(&b[i+1]);
    }

    void printf(const char* fmt, ...) {
        if (!initialized) return;
        __builtin_va_list args;
        __builtin_va_start(args, fmt);

        while (*fmt) {
            if (*fmt != '%') { send_char(*fmt++); continue; }
            fmt++;
            bool ll = (*fmt=='l' && *(fmt+1)=='l');
            if (ll) fmt+=2;

            switch (*fmt) {
                case 's': write_str(__builtin_va_arg(args, const char*)); break;
                case 'c': send_char((char)__builtin_va_arg(args, int)); break;
                case 'i': case 'u':
                    if (ll) log_uint64_dec(__builtin_va_arg(args, uint64_t));
                    else    log_uint32_dec(__builtin_va_arg(args, uint32_t));
                    break;
                case 'x':
                    if (ll) log_uint64_hex(__builtin_va_arg(args, uint64_t));
                    else    log_uint32_hex(__builtin_va_arg(args, uint32_t));
                    break;
                case '%': send_char('%'); break;
                default:  send_char('%'); send_char(*fmt); break;
            }
            fmt++;
        }
        __builtin_va_end(args);
    }

} // namespace uart