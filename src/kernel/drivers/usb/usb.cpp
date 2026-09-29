// The USB core (usb.h): what is attached to the controllers, what each
// device says about itself, and which class driver takes it.

#include "../../../include/drivers/usb/usb.h"

// The class drivers, each in its own file. Enumeration offers each
// interface of each device to them in this order.
extern const usb_class_driver msc_driver;

static const usb_class_driver* const class_drivers[] = {
    &msc_driver,
};

#define MAX_USB_DEVICES 32
static usb_device devices[MAX_USB_DEVICES];
static uint8_t device_count = 0;

// The controller we drive; lsusb/boot diagnostics report it on machines
// with no serial port.
static xhci_controller* active = nullptr;

// Longest configuration descriptor we read; the rest is cut off.
static const uint16_t MAX_CONFIG_LEN = 1024;

static const char* usb_speed_str(uint8_t speed)
{
    switch (speed)
    {
        case 1: return "Full (12 Mb/s)";
        case 2: return "Low (1.5 Mb/s)";
        case 3: return "High (480 Mb/s)";
        case 4: return "SS (5 Gb/s)";
        case 5: return "SS+ (10 Gb/s)";
        default: return "Unknown";
    }
}

static const char* usb_class_name(uint8_t cls)
{
    switch (cls)
    {
        case 0x00: return "Composite";
        case 0x01: return "Audio";
        case 0x02: return "CDC";
        case 0x03: return "HID";
        case 0x05: return "Physical";
        case 0x06: return "Image";
        case 0x07: return "Printer";
        case 0x08: return "Mass Storage";
        case 0x09: return "Hub";
        case 0x0A: return "CDC-Data";
        case 0x0B: return "Smart Card";
        case 0x0E: return "Video";
        case 0x0F: return "Healthcare";
        case 0xE0: return "Wireless";
        case 0xEF: return "Misc";
        case 0xFE: return "App Specific";
        case 0xFF: return "Vendor Specific";
        default:   return "Unknown";
    }
}

static uint16_t max_packet_size_for_speed(uint8_t speed)
{
    switch (speed)
    {
        case 1:
            return 64;
        case 2:
            return 8;
        case 3:
            return 64;
        case 4:
            return 512;
        case 5:
            return 512;
        default:
            return 64;
    }
}

// Forget the devices found so far. A controller that is tried and dropped
// takes its devices with it.
static void reset_devices()
{
    for (uint8_t i = 0; i < device_count; i++)
        if (devices[i].config)
            kfree(devices[i].config);
    memory::memset((uint8_t*)devices, 0, sizeof(devices));
    device_count = 0;
}

// "046f" for 0x046F: four hex digits and a terminator.
static void hex4(uint16_t v, char* out)
{
    const char* digits = "0123456789abcdef";
    for (int i = 3; i >= 0; i--, v >>= 4)
        out[i] = digits[v & 0xF];
    out[4] = '\0';
}

static uint8_t endpoint_dci(uint8_t address)
{
    return (uint8_t)((address & 0x0F) * 2 + ((address & 0x80) ? 1 : 0));
}

// Endpoint Context Interval for a periodic endpoint: 2^n * 125 us. Full and
// low speed devices give bInterval in 1 ms frames, faster ones as the
// exponent (plus one) already.
static uint8_t periodic_interval(uint8_t speed, uint8_t b_interval)
{
    if (speed >= 3)
        return b_interval ? (uint8_t)(b_interval - 1) : 0;

    uint32_t microframes = (uint32_t)(b_interval ? b_interval : 1) * 8;
    uint8_t n = 0;
    while ((2u << n) <= microframes)
        n++;
    if (n < 3)  n = 3;
    if (n > 10) n = 10;
    return n;
}

// Read the device and configuration descriptors of a freshly addressed
// device into it.
static bool read_descriptors(usb_device* dev)
{
    // The first 8 bytes carry bMaxPacketSize0, which may differ from the
    // speed's default EP0 was addressed with.
    uint16_t got = 0;
    if (usb::control(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR,
                     USB_DESC_DEVICE << 8, 0, &dev->desc, 8, &got) != USB_OK || got < 8)
    {
        uart::printf("usb: port %u: device descriptor (8 bytes) failed\n", (uint32_t)dev->port);
        return false;
    }

    uint16_t max_packet = dev->desc.bMaxPacketSize0;
    if (dev->speed >= 4 && max_packet <= 16)
        max_packet = (uint16_t)(1 << max_packet);   // SuperSpeed gives the exponent
    if (max_packet != dev->ep0.max_packet)
    {
        if (!xhci::set_ep0_max_packet(dev->hc, dev->slot, max_packet))
            return false;
        dev->ep0.max_packet = max_packet;
    }

    if (usb::control(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR,
                     USB_DESC_DEVICE << 8, 0, &dev->desc, sizeof(usb_device_descriptor), &got) != USB_OK ||
        got < sizeof(usb_device_descriptor))
    {
        uart::printf("usb: port %u: device descriptor failed\n", (uint32_t)dev->port);
        return false;
    }

    // The configuration: its 9-byte header says how long all of it is.
    usb_config_descriptor header;
    if (usb::control(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR,
                     USB_DESC_CONFIG << 8, 0, &header, sizeof(header), &got) != USB_OK ||
        got < sizeof(header))
    {
        uart::printf("usb: port %u: configuration descriptor failed\n", (uint32_t)dev->port);
        return false;
    }

    uint16_t total = header.wTotalLength;
    if (total > MAX_CONFIG_LEN)
        total = MAX_CONFIG_LEN;
    if (total < sizeof(header))
        return false;

    dev->config = (uint8_t*)kmalloc(total);
    if (!dev->config)
        return false;
    if (usb::control(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR,
                     USB_DESC_CONFIG << 8, 0, dev->config, total, &got) != USB_OK || got < total)
    {
        uart::printf("usb: port %u: configuration descriptor (%u bytes) failed\n",
                     (uint32_t)dev->port, (uint32_t)total);
        return false;
    }
    dev->config_len = total;
    return true;
}

// Offer every interface (alternate setting 0) to the class drivers.
static void attach_drivers(usb_device* dev)
{
    uint16_t offset = 0;
    while (offset + 2 <= dev->config_len)
    {
        uint8_t len = dev->config[offset];
        uint8_t type = dev->config[offset + 1];
        if (len == 0 || offset + len > dev->config_len)
            break;

        if (type == USB_DESC_INTERFACE && len >= sizeof(usb_interface_descriptor))
        {
            const usb_interface_descriptor* iface = (const usb_interface_descriptor*)(dev->config + offset);
            if (iface->bAlternateSetting == 0)
            {
                for (const usb_class_driver* drv : class_drivers)
                {
                    if (drv->probe(dev, iface))
                    {
                        if (!dev->driver)
                            dev->driver = drv;
                        break;
                    }
                }
            }
        }
        offset += len;
    }
}

// Enumerate the device on one connected root port: reset, address, read
// what it is, and hand it to a driver.
static void enumerate_port(xhci_controller* hc, uint8_t port)
{
    if (!xhci::reset_port(hc, port))
    {
        uart::printf("usb: port %u: reset failed\n", (uint32_t)port);
        return;
    }
    if (device_count >= MAX_USB_DEVICES)
    {
        uart::printf("usb: port %u: too many devices\n", (uint32_t)port);
        return;
    }

    usb_device* dev = &devices[device_count];
    memory::memset((uint8_t*)dev, 0, sizeof(*dev));
    dev->hc = hc;
    dev->port = port;
    dev->speed = xhci::port_speed(hc, port);

    dev->slot = xhci::enable_slot(hc);
    if (dev->slot == 0)
    {
        uart::printf("usb: port %u: no device slot\n", (uint32_t)port);
        return;
    }

    dev->ep0.type = USB_EP_CONTROL;
    dev->ep0.dci = 1;
    dev->ep0.max_packet = max_packet_size_for_speed(dev->speed);
    if (!xhci::address_device(hc, dev->slot, port, dev->speed, dev->ep0.max_packet, &dev->ep0.ring))
        return;

    // From here on the controller knows the device, and its EP0 ring lives
    // in this entry: keep it, even if the descriptors cannot be read.
    device_count++;

    if (!read_descriptors(dev))
        return;

    char id[10];
    hex4(dev->desc.idVendor, id);
    id[4] = ':';
    hex4(dev->desc.idProduct, id + 5);
    uart::printf("usb: port %u slot %u: %s %s, %s\n", (uint32_t)port, (uint32_t)dev->slot, id,
                 usb_class_name(dev->desc.bDeviceClass), usb_speed_str(dev->speed));

    attach_drivers(dev);
}

static void enumerate_controller(xhci_controller* hc)
{
    for (uint8_t port = 0; port < xhci::port_count(hc); port++)
        if (xhci::port_connected(hc, port))
            enumerate_port(hc, port);
}

static uint8_t devices_with_driver()
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < device_count; i++)
        if (devices[i].driver)
            n++;
    return n;
}

namespace usb
{
    bool init()
    {
        active = nullptr;
        reset_devices();

        uint8_t count = xhci::find_controllers();
        if (count == 0)
        {
            uart::printf("xhci: no controller found\n");
            return false;
        }
        uart::printf("xhci: %u controller(s) found\n", (uint32_t)count);

        // Try them in turn and keep the first that has a device a driver
        // took. One that has none is halted again before the next attempt,
        // so an abandoned controller cannot keep DMAing into memory its
        // successor is about to allocate.
        uint8_t best = 0;
        uint8_t best_devices = 0;
        bool have_best = false;

        for (uint8_t i = 0; i < count; i++)
        {
            xhci_controller* hc = xhci::controller(i);
            PCIDevice* pci = xhci::pci_device(hc);
            reset_devices();
            if (xhci::start(hc))
                enumerate_controller(hc);

            if (devices_with_driver() > 0)
            {
                active = hc;
                uart::printf("xhci: using controller %u:%u.%u (%u device(s) with a driver)\n",
                             (uint32_t)pci->bus, (uint32_t)pci->device, (uint32_t)pci->function,
                             (uint32_t)devices_with_driver());
                return true;
            }

            if (!have_best || device_count > best_devices)
            {
                best = i;
                best_devices = device_count;
                have_best = true;
            }

            uart::printf("xhci: controller %u:%u.%u: %u device(s), none with a driver\n",
                         (uint32_t)pci->bus, (uint32_t)pci->device, (uint32_t)pci->function,
                         (uint32_t)device_count);

            if (i + 1 < count)
                xhci::stop(hc);
        }

        // Nothing usable anywhere. Leave the controller that at least saw
        // devices running, so lsusb/usbinfo still have something to report.
        // The last one tried is still running; it stops before the best one
        // restarts.
        if (best_devices > 0 && best != count - 1)
        {
            xhci::stop(xhci::controller(count - 1));
            reset_devices();
            active = xhci::controller(best);
            if (xhci::start(active))
                enumerate_controller(active);
        }
        else
            active = xhci::controller(count - 1);

        return false;
    }

    const usb_endpoint_descriptor* interface_endpoint(usb_device* dev,
                                                      const usb_interface_descriptor* iface,
                                                      uint8_t i)
    {
        uint16_t offset = (uint16_t)((const uint8_t*)iface - dev->config) + iface->bLength;
        while (offset + 2 <= dev->config_len)
        {
            uint8_t len = dev->config[offset];
            uint8_t type = dev->config[offset + 1];
            if (len == 0 || offset + len > dev->config_len || type == USB_DESC_INTERFACE)
                break;
            if (type == USB_DESC_ENDPOINT && len >= sizeof(usb_endpoint_descriptor))
            {
                if (i == 0)
                    return (const usb_endpoint_descriptor*)(dev->config + offset);
                i--;
            }
            offset += len;
        }
        return nullptr;
    }

    usb_status set_configuration(usb_device* dev)
    {
        if (dev->configured)
            return USB_OK;

        uint8_t value = ((const usb_config_descriptor*)dev->config)->bConfigurationValue;
        usb_status st = control(dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                                USB_REQ_SET_CONFIGURATION, value, 0, nullptr, 0, nullptr);
        if (st != USB_OK)
        {
            uart::printf("usb: slot %u: SET_CONFIGURATION failed\n", (uint32_t)dev->slot);
            return st;
        }
        dev->configured = true;
        return USB_OK;
    }

    usb_status open_endpoints(usb_device* dev, const usb_endpoint_descriptor* const* descs,
                              uint8_t count, usb_endpoint** out)
    {
        if (count == 0 || dev->ep_count + count > USB_MAX_ENDPOINTS)
            return USB_ERR_INVALID_PARAM;

        xhci_ep_config cfg[USB_MAX_ENDPOINTS];
        for (uint8_t i = 0; i < count; i++)
        {
            const usb_endpoint_descriptor* d = descs[i];
            usb_endpoint* ep = &dev->eps[dev->ep_count + i];
            bool in = (d->bEndpointAddress & 0x80) != 0;

            ep->address = d->bEndpointAddress;
            ep->type = d->bmAttributes & 0x03;
            ep->max_packet = d->wMaxPacketSize & 0x07FF;
            ep->dci = endpoint_dci(d->bEndpointAddress);

            cfg[i].dci = ep->dci;
            cfg[i].max_packet = ep->max_packet;
            cfg[i].ring = &ep->ring;
            cfg[i].interval = 0;
            switch (ep->type)
            {
                case USB_EP_BULK:
                    cfg[i].type = in ? XHCI_EP_TYPE_BULK_IN : XHCI_EP_TYPE_BULK_OUT;
                    break;
                case USB_EP_INTERRUPT:
                    cfg[i].type = in ? XHCI_EP_TYPE_INTERRUPT_IN : XHCI_EP_TYPE_INTERRUPT_OUT;
                    cfg[i].interval = periodic_interval(dev->speed, d->bInterval);
                    break;
                default:
                    return USB_ERR_INVALID_PARAM;   // control and isochronous: not used
            }
        }

        if (!xhci::configure_endpoints(dev->hc, dev->slot, cfg, count))
            return USB_ERR_IO;

        for (uint8_t i = 0; i < count; i++)
            out[i] = &dev->eps[dev->ep_count + i];
        dev->ep_count += count;
        return USB_OK;
    }

    usb_status control(usb_device* dev, uint8_t request_type, uint8_t request, uint16_t value,
                       uint16_t index, void* data, uint16_t length, uint16_t* actual)
    {
        uint8_t setup[8] = {
            request_type, request,
            (uint8_t)value, (uint8_t)(value >> 8),
            (uint8_t)index, (uint8_t)(index >> 8),
            (uint8_t)length, (uint8_t)(length >> 8),
        };
        bool in = (request_type & USB_DIR_IN) != 0;

        // The controller needs memory it can reach; control transfers are
        // small and rare enough to bounce through a fresh buffer.
        uint8_t* buf = nullptr;
        uintptr_t phys = 0;
        if (length > 0)
        {
            buf = (uint8_t*)xhci::dma_alloc(length, 64, 65536);
            phys = xhci::phys(buf);
            if (!in)
                memory::memcpy(buf, (const uint8_t*)data, length);
        }

        xhci_transfer_ring* ring = &dev->ep0.ring;
        if (!xhci::control(dev->hc, dev->slot, ring, setup, phys, length, in, 500))
        {
            // The controller may still write the buffer: it is not freed.
            uart::printf("usb: slot %u: control request %x timed out\n",
                         (uint32_t)dev->slot, (uint32_t)request);
            return USB_ERR_TIMEOUT;
        }

        usb_status st = USB_OK;
        if (!xhci::completed_ok(ring))
        {
            st = ring->cc == XHCI_TRB_COMPLETION_STALL ? USB_ERR_STALL : USB_ERR_IO;
            uart::printf("usb: slot %u: control request %x failed code=%u (%s)\n",
                         (uint32_t)dev->slot, (uint32_t)request, (uint32_t)ring->cc,
                         xhci::completion_code_str(ring->cc));
        }

        uint16_t moved = (uint16_t)(length - (ring->residue < length ? ring->residue : length));
        if (st == USB_OK && in && moved > 0)
            memory::memcpy((uint8_t*)data, buf, moved);
        if (actual)
            *actual = st == USB_OK ? moved : 0;

        if (buf)
            xhci::dma_free(buf);
        return st;
    }

    usb_status bulk(usb_device* dev, usb_endpoint* ep, void* dma_buf, uint32_t length,
                    uint32_t* actual, uint32_t timeout_ms)
    {
        if (length > USB_MAX_XFER_BYTES)
            return USB_ERR_INVALID_PARAM;

        xhci_transfer_ring* ring = &ep->ring;
        if (!xhci::normal(dev->hc, dev->slot, ep->dci, ring, xhci::phys(dma_buf), length, timeout_ms))
        {
            uart::printf("usb: slot %u ep %x: bulk transfer timed out\n",
                         (uint32_t)dev->slot, (uint32_t)ep->address);
            return USB_ERR_TIMEOUT;
        }
        if (ring->cc == XHCI_TRB_COMPLETION_STALL)
            return USB_ERR_STALL;
        if (!xhci::completed_ok(ring))
        {
            uart::printf("usb: slot %u ep %x: bulk transfer failed code=%u (%s)\n",
                         (uint32_t)dev->slot, (uint32_t)ep->address, (uint32_t)ring->cc,
                         xhci::completion_code_str(ring->cc));
            return USB_ERR_IO;
        }
        if (actual)
            *actual = length - (ring->residue < length ? ring->residue : length);
        return USB_OK;
    }

    usb_status clear_halt(usb_device* dev, usb_endpoint* ep)
    {
        uart::printf("usb: slot %u ep %x: clearing halt\n", (uint32_t)dev->slot, (uint32_t)ep->address);

        usb_status st = control(dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_ENDPOINT,
                                USB_REQ_CLEAR_FEATURE, USB_FEATURE_ENDPOINT_HALT, ep->address,
                                nullptr, 0, nullptr);
        if (st != USB_OK)
            return st;
        if (!xhci::reset_endpoint(dev->hc, dev->slot, ep->dci, &ep->ring))
            return USB_ERR_IO;
        return USB_OK;
    }

    void* dma_alloc(uint32_t size)
    {
        if (size == 0 || size > USB_MAX_XFER_BYTES)
            return nullptr;
        // One Normal TRB carries it, and a TRB's buffer may not cross a
        // 64 KiB boundary.
        return xhci::dma_alloc(size, 64, 65536);
    }

    void dma_free(void* ptr)
    {
        xhci::dma_free(ptr);
    }

    // --- Reports ---------------------------------------------------------------

    const char* get_usb_class_name(uint8_t cls)
    {
        return usb_class_name(cls);
    }

    const char* get_usb_speed_str(uint8_t speed)
    {
        return usb_speed_str(speed);
    }

    uint32_t get_context_entry_size()
    {
        return active ? xhci::context_entry_size(active) : 32;
    }

    uint8_t get_port_count()
    {
        return active ? xhci::port_count(active) : 0;
    }

    // Raw PORTSC of one root port, for `usbports`. Zero when no controller
    // came up, which the caller reports as such.
    uint32_t get_port_status(uint8_t port)
    {
        return active ? xhci::port_status(active, port) : 0;
    }

    bool port_is_usb3(uint8_t port)
    {
        return active && xhci::port_is_usb3(active, port);
    }

    uint8_t get_controller_count()
    {
        uint8_t n = 0;
        while (xhci::controller(n))
            n++;
        return n;
    }

    // Bus/device/function of the controller we are driving, or 0:0.0 when
    // none came up.
    void get_controller_location(uint8_t* bus, uint8_t* dev, uint8_t* fn)
    {
        PCIDevice* pci = active ? xhci::pci_device(active) : nullptr;
        *bus = pci ? pci->bus : 0;
        *dev = pci ? pci->device : 0;
        *fn  = pci ? pci->function : 0;
    }

    uint8_t get_device_count()
    {
        return device_count;
    }

    usb_status get_device_info(uint8_t index, usb_device_info* out)
    {
        if (index >= device_count)
            return USB_ERR_NOT_FOUND;
        if (!out)
            return USB_ERR_INVALID_PARAM;

        const usb_device* dev = &devices[index];
        memory::memset((uint8_t*)out, 0, sizeof(*out));
        out->slot_id = dev->slot;
        out->port_index = dev->port;
        out->port_speed = dev->speed;
        out->vendor_id = dev->desc.idVendor;
        out->product_id = dev->desc.idProduct;
        out->bcd_usb = dev->desc.bcdUSB;
        out->device_class = dev->desc.bDeviceClass;
        out->device_subclass = dev->desc.bDeviceSubClass;
        out->device_protocol = dev->desc.bDeviceProtocol;
        memory::memcpy((uint8_t*)out->vendor_str, (const uint8_t*)dev->vendor, sizeof(out->vendor_str));
        memory::memcpy((uint8_t*)out->product_str, (const uint8_t*)dev->product, sizeof(out->product_str));
        out->driver = dev->driver ? dev->driver->name : nullptr;
        return USB_OK;
    }
}
