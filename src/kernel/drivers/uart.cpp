#include "../../include/drivers/uart.h"

namespace uart {
    UINT16 port = COM1; // default

    int init(UINT16 _port) {
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

    void send(char c) {
        while ((port::byte_in(port + 5) & 0x20) == 0);
        return port::byte_out(port, c);
    }

    void write(const char *str) {
        while (*str) {
            send(*str++);
        }
    }

    char receive(void) {
        return port::byte_in(port + 5) & 0x01;
    }

    char read(void) {
        while (receive() == 0);
        return port::byte_in(port);
    }

    void log_uint16(UINT16 number) {
        char buffer[8];
        buffer[0] = '0';
        buffer[1] = 'x';
        buffer[6] = '\n';
        buffer[7] = '\0';

        for (int i = 5; i >= 2; i--) {
            UINT8 nibble = number & 0xF;
            buffer[i] = (nibble < 10) ? ('0' + nibble) : ('A' + (nibble - 10));
            number >>= 4;
        }

        write(buffer);
    }

    void log_uint64_hex(UINT64 number) {
        char buffer[20];
        buffer[0] = '0';
        buffer[1] = 'x';
        buffer[18] = '\n';
        buffer[19] = '\0';

        for (int i = 17; i >= 2; i--) {
            UINT8 nibble = number & 0xF;
            buffer[i] = (nibble < 10) ? ('0' + nibble) : ('A' + (nibble - 10));
            number >>= 4;
        }

        write(buffer);
    }

    void log_uint64_dec(UINT64 number) {
        char buffer[21];
        int index = 20;
        buffer[index--] = '\0';
        buffer[index--] = '\n';

        if (number == 0) {
            buffer[index--] = '0';
        } else {
            while (number > 0) {
                buffer[index--] = '0' + (number % 10);
                number /= 10;
            }
        }

        write(&buffer[index + 1]);
    }
} // namespace