#include "../../include/drivers/uart.h"

namespace uart_legacy {
    UINT16 port = COM1; // default

    int init(UINT16 _port = COM1) {
        port = _port;

        port::byte_out(port + 1, 0x00);  // Disable interrupts
        port::byte_out(port + 3, 0x80);  // Enable DLAB (set baud rate divisor)
        port::byte_out(port + 0, 0x03);  // Set divisor to 3 (low byte, 38400 baud)
        port::byte_out(port + 1, 0x00);  // High byte
        port::byte_out(port + 3, 0x03);  // 8 bits, no parity, one stop bit
        port::byte_out(port + 2, 0xC7);  // Enable FIFO, clear them, with 14-byte threshold
        port::byte_out(port + 4, 0x0B);  // IRQs enabled, RTS/DSR set
        port::byte_out(port + 4, 0x1E);  // Set in loopback mode, test the serial chip
        port::byte_out(port + 0, 0xAE);  // Test serial chip (send byte 0xAE and check if serial returns same byte)

        if (port::byte_in(port + 0) != 0xAE){
            return 1;
        }

        port::byte_out(port + 4, 0x0F);
        write("\n");

        return 0;
    }

    void _send(char c) {
        while ((port::byte_in(port + 5) & 0x20) == 0);
        return port::byte_out(port, c);
    }

    void write(const char *str) {
        while (*str) {
            uart_legacy::_send(*str++);
        }
    }

    char _receive() {
        return port::byte_in(port + 5) & 0x01;
    }

    char read() {
        while (uart_legacy::_receive() == 0);
        return port::byte_in(port);
    }
    
} // namespace uart_legacy

namespace uart {
    
    enum Backend { 
        NONE, 
        PCI_IO, 
        PCI_MMIO, 
        LEGACY 
    };
    static Backend backend = NONE;

    enum Reg : UINT8 {
        DATA                = 0x00, // THR / RBR / DLL
        INTERRUPT_ENABLE    = 0x01, // IER / DLM
        INTERRUPT_ID_FIFO   = 0x02, // IIR (R) / FCR (W)
        LINE_CONTROL        = 0x03, // LCR
        MODEM_CONTROL       = 0x04, // MCR
        LINE_STATUS         = 0x05, // LSR
        MODEM_STATUS        = 0x06, // MSR
        SCRATCH             = 0x07  // SCR
    };    

    static UINT16 io = 0;
    static volatile UINT8* mmio = 0;

    static inline void _out_byte(UINT8 r, UINT8 v) {
        if (backend == PCI_IO) {
            port::byte_out(io + r, v);
        }
        else {
            mmio[r] = v;
        }
    }

    static inline UINT8 _in_byte(UINT8 r) {
        return (backend == PCI_IO)
            ? port::byte_in(io + r)
            : mmio[r];
    }

    static void _hw_init() {
        _out_byte(INTERRUPT_ENABLE, 0x00);
        _out_byte(LINE_CONTROL,     0x80); // DLAB = 1
        _out_byte(DATA,             0x03); // DLL
        _out_byte(INTERRUPT_ENABLE, 0x00); // DLM
        _out_byte(LINE_CONTROL,     0x03); // 8N1, DLAB = 0
        _out_byte(INTERRUPT_ID_FIFO,0xC7); // FIFO enable/reset
        _out_byte(MODEM_CONTROL,    0x0B); // RTS/DTR/OUT2
    }

    static bool _init_pci() {
        for (UINT32 i = 0; i < pci::device_count(); i++) {
            PCIDevice* d = pci::get_by_id(i);
            if (!d || !d->valid) continue;
            if (d->class_code != 0x07 || d->subclass != 0x00 || d->prog_if < 0x02) continue;

            UINT32 bar = d->bar[0];
            if (!bar) continue;

            if (bar & 1) {
                io = bar & ~0x3;
                backend = PCI_IO;
            } else {
                mmio = (volatile UINT8*)(bar & ~0xF);
                backend = PCI_MMIO;
            }

            _hw_init();
            return true;
        }
        return false;
    }

    int init() {
        if (_init_pci()) return 0;

        uart_legacy::init(COM1);    // Hardcoded
        backend = LEGACY;
        return 0;
    }

    void _send(char c) {
        if (backend == LEGACY) {
            uart_legacy::_send(c);
            return;
        }
        while (!(_in_byte(LINE_STATUS) & 0x20));
        _out_byte(DATA, c);
    }

    void _write(const char* str) {
        while (*str) {
            uart::_send(*str++);
        }
    }

    char _receive() {
        if (backend == LEGACY) {
            return uart_legacy::_receive();
        }
        return _in_byte(LINE_STATUS) & 0x01;
    }

    char read() {
        while (_receive() == 0);

        if (backend == LEGACY) {
            return port::byte_in(uart_legacy::port + DATA);
        }
        return _in_byte(DATA);
    }

    void log_uint64_hex(UINT64 number) {
        char buffer[19];
        buffer[0] = '0';
        buffer[1] = 'x';
        buffer[18] = '\0';

        for (int i = 17; i >= 2; i--) {
            UINT8 nibble = number & 0xF;
            buffer[i] = (nibble < 10) ? ('0' + nibble) : ('A' + (nibble - 10));
            number >>= 4;
        }

        uart::_write(buffer);
    }

    void log_uint64_dec(UINT64 number) {
        char buffer[20];
        int index = 19;
        buffer[index--] = '\0';

        if (number == 0) {
            buffer[index--] = '0';
        } else {
            while (number > 0) {
                buffer[index--] = '0' + (number % 10);
                number /= 10;
            }
        }

        uart::_write(&buffer[index + 1]);
    }

    void log_uint32_hex(UINT32 number) {
        char buffer[11];
        buffer[0] = '0';
        buffer[1] = 'x';
        buffer[10] = '\0';

        for (int i = 9; i >= 2; i--) {
            UINT8 nibble = number & 0xF;
            buffer[i] = (nibble < 10) ? ('0' + nibble) : ('A' + (nibble - 10));
            number >>= 4;
        }

        uart::_write(buffer);
    }

    void log_uint32_dec(UINT32 number) {
        char buffer[11];
        int index = 10;
        buffer[index--] = '\0';

        if (number == 0) {
            buffer[index--] = '0';
        } else {
            while (number > 0) {
                buffer[index--] = '0' + (number % 10);
                number /= 10;
            }
        }

        uart::_write(&buffer[index + 1]);
    }

    void printf(const char* fmt, ...) {
        __builtin_va_list args;
        __builtin_va_start(args, fmt);

        while (*fmt) {
            if (*fmt != '%') {
                _send(*fmt++);
                continue;
            }

            fmt += 1; //%

            bool ll = false;
            if (*fmt == 'l' && *(fmt + 1) == 'l') {
                ll = true;
                fmt += 2;
            }

            switch (*fmt) {
                case 's':
                    uart::_write(__builtin_va_arg(args, const char*));
                    break;
                case 'i':
                case 'u':
                    if (ll)
                        log_uint64_dec(__builtin_va_arg(args, UINT64));
                    else
                        log_uint32_dec(__builtin_va_arg(args, UINT32));
                    break;
                case 'x':
                    if (ll)
                        log_uint64_hex(__builtin_va_arg(args, UINT64));
                    else
                        log_uint32_hex(__builtin_va_arg(args, UINT32));
                    break;
                default:
                    uart::_write(__builtin_va_arg(args, const char*));
                    break;
            }

            fmt += 1;
        }

        __builtin_va_end(args);
    }
} // namespace uart