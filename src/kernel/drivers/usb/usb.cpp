// The USB core (usb.h): what is attached to the controllers, what each
// device says about itself, and which class driver takes it.

#include "../../../include/drivers/usb/usb.h"
#include "../../../include/drivers/pit.h"
#include "../../../include/cpu/process.h"
#include "../../../include/cpu/wait.h"

// The class drivers, each in its own file. Enumeration offers each
// interface of each device to them in this order.
extern const usb_class_driver msc_driver;
extern const usb_class_driver hid_kbd_driver;
extern const usb_class_driver hub_driver;

static const usb_class_driver* const class_drivers[] = {
    &msc_driver,
    &hid_kbd_driver,
    &hub_driver,
};

#define MAX_USB_DEVICES 32
// Entries are reused once their device is gone; in_use marks the live ones.
static usb_device devices[MAX_USB_DEVICES];

// The usb kernel process sleeps here until a port changes (hotplug below):
// a root port (the controller says so) or a hub's (its driver says so, by
// wake_hotplug(), and does the work in its work()).
static wait_queue hotplug_wq;
static volatile bool driver_work = false;

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
    for (uint8_t i = 0; i < MAX_USB_DEVICES; i++)
        if (devices[i].config)
            kfree(devices[i].config);
    memory::memset((uint8_t*)devices, 0, sizeof(devices));
}

// "046f" for 0x046F: four hex digits and a terminator.
static void hex4(uint16_t v, char* out)
{
    const char* digits = "0123456789abcdef";
    for (int i = 3; i >= 0; i--, v >>= 4)
        out[i] = digits[v & 0xF];
    out[4] = '\0';
}

// Is the device still plugged in: its root port connected and, behind
// hubs, the port of its hub too (asked of the hub; USB 2.0 11.24.2.7 and
// USB 3 10.16.2.6 both keep "connection" in bit 0 of the port status).
static bool still_attached(usb_device* dev)
{
    if (!xhci::port_connected(dev->hc, dev->port))
        return false;
    if (!dev->parent)
        return true;
    uint8_t status[4];
    uint16_t got = 0;
    if (usb::control(dev->parent, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER, USB_REQ_GET_STATUS,
                     0, dev->hub_port, status, sizeof(status), &got) != USB_OK || got < 4)
        return !dev->parent->gone;
    return status[0] & 1;
}

// A transfer that did not end: the device was unplugged (from now on it
// gets no more requests), or it did not answer in time.
static usb_status not_ended(usb_device* dev)
{
    if (!still_attached(dev))
    {
        dev->gone = true;
        return USB_ERR_NO_DEVICE;
    }
    return USB_ERR_TIMEOUT;
}

// How a transfer on `ring` ended, as a status.
static usb_status ring_status(const xhci_transfer_ring* ring)
{
    if (xhci::completed_ok(ring))
        return USB_OK;
    return ring->cc == XHCI_TRB_COMPLETION_STALL ? USB_ERR_STALL : USB_ERR_IO;
}

static void endpoint_complete(void* owner)
{
    usb_endpoint* ep = (usb_endpoint*)owner;
    uint32_t residue = ep->ring.residue < ep->submitted ? ep->ring.residue : ep->submitted;
    ep->on_complete(ep, ring_status(&ep->ring), ep->submitted - residue);
}

static void setup_packet(uint8_t* setup, uint8_t request_type, uint8_t request, uint16_t value,
                         uint16_t index, uint16_t length)
{
    setup[0] = request_type;
    setup[1] = request;
    setup[2] = (uint8_t)value;
    setup[3] = (uint8_t)(value >> 8);
    setup[4] = (uint8_t)index;
    setup[5] = (uint8_t)(index >> 8);
    setup[6] = (uint8_t)length;
    setup[7] = (uint8_t)(length >> 8);
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

// "0", "0.3", "0.3.2": the root port (0-based, as usbports numbers
// them), then the hub port at each tier (1-based, as hubs number them).
static void device_path(const usb_device* dev, char* out, uint32_t size)
{
    uint8_t ports[8];
    uint8_t n = 0;
    for (const usb_device* d = dev; d->parent && n < 8; d = d->parent)
        ports[n++] = d->hub_port;

    uint32_t i = 0;
    auto put_num = [&](uint32_t v) {
        char tmp[4];
        uint32_t t = 0;
        do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v && t < 4);
        while (t && i + 1 < size)
            out[i++] = tmp[--t];
    };
    put_num(dev->port);
    while (n && i + 2 < size)
    {
        out[i++] = '.';
        put_num(ports[--n]);
    }
    out[i] = '\0';
}

// A new device, at the root port `port` (parent null) or on port `hub_port`
// of the hub `parent`, already reset and running at `speed`: address it,
// read what it is, and hand it to a driver.
static usb_device* attach(xhci_controller* hc, usb_device* parent, uint8_t port, uint8_t hub_port,
                          uint8_t speed)
{
    usb_device* dev = nullptr;
    for (uint8_t i = 0; i < MAX_USB_DEVICES && !dev; i++)
        if (!devices[i].in_use)
            dev = &devices[i];
    if (!dev)
    {
        uart::printf("usb: port %u: too many devices\n", (uint32_t)port);
        return nullptr;
    }

    memory::memset((uint8_t*)dev, 0, sizeof(*dev));
    dev->hc = hc;
    dev->port = port;
    dev->speed = speed;
    dev->parent = parent;
    dev->hub_port = hub_port;

    xhci_dev_location where = {};
    where.root_port = port;
    where.speed = speed;
    if (parent)
    {
        // One route nibble per hub tier below the root port; a port past
        // 15 cannot be expressed and is clamped (xHCI 8.9).
        dev->tier = (uint8_t)(parent->tier + 1);
        dev->route = parent->route | ((uint32_t)(hub_port < 15 ? hub_port : 15) << (4 * parent->tier));

        // A low or full speed device behind a high speed hub goes through
        // the nearest such hub's transaction translator.
        if (speed <= 2)
        {
            if (parent->speed == 3)
            {
                dev->tt_slot = parent->slot;
                dev->tt_port = hub_port;
            }
            else
            {
                dev->tt_slot = parent->tt_slot;
                dev->tt_port = parent->tt_port;
            }
        }
        where.route = dev->route;
        where.tt_hub_slot = dev->tt_slot;
        where.tt_port = dev->tt_port;
    }

    dev->slot = xhci::enable_slot(hc);
    if (dev->slot == 0)
    {
        uart::printf("usb: port %u: no device slot\n", (uint32_t)port);
        return nullptr;
    }

    dev->ep0.type = USB_EP_CONTROL;
    dev->ep0.dci = 1;
    dev->ep0.max_packet = max_packet_size_for_speed(dev->speed);
    if (!xhci::address_device(hc, dev->slot, &where, dev->ep0.max_packet, &dev->ep0.ring))
    {
        xhci::disable_slot(hc, dev->slot);
        xhci::free_ring(&dev->ep0.ring);
        return nullptr;
    }

    // From here on the controller knows the device, and its EP0 ring lives
    // in this entry: keep it, even if the descriptors cannot be read.
    dev->in_use = true;

    if (!read_descriptors(dev))
        return dev;

    char id[10], path[24];
    hex4(dev->desc.idVendor, id);
    id[4] = ':';
    hex4(dev->desc.idProduct, id + 5);
    device_path(dev, path, sizeof(path));
    uart::printf("usb: port %s slot %u: %s %s, %s\n", path, (uint32_t)dev->slot, id,
                 usb_class_name(dev->desc.bDeviceClass), usb_speed_str(dev->speed));

    attach_drivers(dev);
    return dev;
}

// Enumerate the device on one connected root port.
static void enumerate_port(xhci_controller* hc, uint8_t port)
{
    if (!xhci::reset_port(hc, port))
    {
        uart::printf("usb: port %u: reset failed\n", (uint32_t)port);
        return;
    }
    attach(hc, nullptr, port, 0, xhci::port_speed(hc, port));
}

// A device that is gone: the drivers let go of it, the controller forgets
// it, and its entry is free again. Behind a hub, whatever was plugged into
// it went with it and goes first.
static void detach(usb_device* dev)
{
    for (uint8_t i = 0; i < MAX_USB_DEVICES; i++)
        if (devices[i].in_use && devices[i].parent == dev)
            detach(&devices[i]);

    char path[24];
    device_path(dev, path, sizeof(path));
    uart::printf("usb: port %s slot %u: disconnected\n", path, (uint32_t)dev->slot);

    // No request reaches it from here on, and after disable_slot no event
    // reaches its rings - so the drivers can free what the controller wrote
    // into.
    dev->gone = true;
    xhci::disable_slot(dev->hc, dev->slot);
    for (const usb_class_driver* drv : class_drivers)
        if (drv->disconnect)
            drv->disconnect(dev);

    xhci::free_ring(&dev->ep0.ring);
    for (uint8_t i = 0; i < dev->ep_count; i++)
        xhci::free_ring(&dev->eps[i].ring);
    if (dev->config)
        kfree(dev->config);
    memory::memset((uint8_t*)dev, 0, sizeof(*dev));
}

// The device plugged straight into a root port.
static usb_device* device_on(xhci_controller* hc, uint8_t port)
{
    for (uint8_t i = 0; i < MAX_USB_DEVICES; i++)
        if (devices[i].in_use && !devices[i].parent && devices[i].hc == hc && devices[i].port == port)
            return &devices[i];
    return nullptr;
}

static uint8_t devices_on(xhci_controller* hc)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < MAX_USB_DEVICES; i++)
        if (devices[i].in_use && devices[i].hc == hc)
            n++;
    return n;
}

// Hotplug: a root port whose state changed. Its device, if it had one, is
// gone when the port is empty or saw a new connection (a quick unplug and
// plug in); a connected port with no device gets one. Everything else -
// the changes a port reset leaves behind - needs nothing. At boot there is
// no process to put to sleep yet: the debounce spins on the clock instead.
static void port_changed(xhci_controller* hc, uint8_t port, bool at_boot)
{
    uint32_t sc = xhci::port_ack(hc, port);
    bool connected = sc & (1u << 0);            // CCS
    bool new_connection = sc & (1u << 17);      // CSC

    usb_device* dev = device_on(hc, port);
    if (dev && (!connected || new_connection))
    {
        detach(dev);
        dev = nullptr;
    }
    if (!connected || dev)
        return;

    // Contacts bounce as a plug goes in; the device is looked at once the
    // port has stayed connected for a while.
    if (at_boot)
        xhci::delay_ms(XHCI_PORT_DEBOUNCE_MS);
    else
        wait::sleep_until(pit::deadline_ms(XHCI_PORT_DEBOUNCE_MS));
    if (xhci::port_connected(hc, port) && !device_on(hc, port))
        enumerate_port(hc, port);
}

static bool hotplug_pending(void*)
{
    if (driver_work)
        return true;
    for (uint8_t i = 0; xhci::controller(i); i++)
        if (xhci::ports_changed(xhci::controller(i)))
            return true;
    return false;
}

// The usb kernel process: plugging in and pulling out, handled where
// waiting is allowed. The timer tick (root ports) and the hub driver wake
// it.
static void handle_changes(bool at_boot)
{
    for (uint8_t i = 0; xhci::controller(i); i++)
    {
        xhci_controller* hc = xhci::controller(i);
        uint8_t port;
        while (xhci::take_port_change(hc, &port))
            port_changed(hc, port, at_boot);
    }
    if (driver_work)
    {
        driver_work = false;
        for (const usb_class_driver* drv : class_drivers)
            if (drv->work)
                drv->work();
    }
}

static void hotplug_main(void*)
{
    for (;;)
    {
        wait::wait_event(&hotplug_wq, hotplug_pending, nullptr, 0);
        handle_changes(false);
    }
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
    for (uint8_t i = 0; i < MAX_USB_DEVICES; i++)
        if (devices[i].in_use && devices[i].driver)
            n++;
    return n;
}

static uint8_t controller_index(xhci_controller* hc)
{
    uint8_t i = 0;
    while (xhci::controller(i) && xhci::controller(i) != hc)
        i++;
    return i;
}

namespace usb
{
    bool init()
    {
        reset_devices();

        uint8_t count = xhci::find_controllers();
        if (count == 0)
        {
            uart::printf("xhci: no controller found\n");
            return false;
        }
        uart::printf("xhci: %u controller(s) found\n", (uint32_t)count);

        // All of them stay running, the empty ones too: a device can be on
        // any, and a laptop's user-facing ports may well sit on the second
        // controller.
        for (uint8_t i = 0; i < count; i++)
        {
            xhci_controller* hc = xhci::controller(i);
            PCIDevice* pci = xhci::pci_device(hc);

            if (!xhci::start(hc))
            {
                xhci::stop(hc);
                continue;
            }
            enumerate_controller(hc);
            uart::printf("xhci: controller %u:%u.%u: %u device(s)\n",
                         (uint32_t)pci->bus, (uint32_t)pci->device, (uint32_t)pci->function,
                         (uint32_t)devices_on(hc));
        }

        return devices_with_driver() > 0;
    }

    usb_device* enumerate_hub_port(usb_device* hub, uint8_t port, uint8_t speed)
    {
        return attach(hub->hc, hub, hub->port, port, speed);
    }

    usb_device* device_on_hub_port(usb_device* hub, uint8_t port)
    {
        for (uint8_t i = 0; i < MAX_USB_DEVICES; i++)
            if (devices[i].in_use && devices[i].parent == hub && devices[i].hub_port == port)
                return &devices[i];
        return nullptr;
    }

    void detach_device(usb_device* dev)
    {
        detach(dev);
    }

    void wake_hotplug()
    {
        driver_work = true;
        wait::wake_up(&hotplug_wq);
    }

    void boot_changes()
    {
        handle_changes(true);
    }

    void start_hotplug()
    {
        process::start_kernel_process("usb", hotplug_main);
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

        xhci_hub_info hub = { dev->hub_ports, dev->hub_think_time };
        if (!xhci::configure_endpoints(dev->hc, dev->slot, cfg, count, dev->hub_ports ? &hub : nullptr))
            return USB_ERR_IO;

        for (uint8_t i = 0; i < count; i++)
            out[i] = &dev->eps[dev->ep_count + i];
        dev->ep_count += count;
        return USB_OK;
    }

    usb_status control(usb_device* dev, uint8_t request_type, uint8_t request, uint16_t value,
                       uint16_t index, void* data, uint16_t length, uint16_t* actual)
    {
        if (dev->gone)
            return USB_ERR_NO_DEVICE;

        uint8_t setup[8];
        setup_packet(setup, request_type, request, value, index, length);
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
            usb_status st = not_ended(dev);
            if (st == USB_ERR_TIMEOUT)
                uart::printf("usb: slot %u: control request %x timed out\n",
                             (uint32_t)dev->slot, (uint32_t)request);
            return st;
        }

        usb_status st = ring_status(ring);
        if (st != USB_OK)
        {
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
        if (dev->gone)
            return USB_ERR_NO_DEVICE;
        if (length > USB_MAX_XFER_BYTES)
            return USB_ERR_INVALID_PARAM;

        xhci_transfer_ring* ring = &ep->ring;
        if (!xhci::normal(dev->hc, dev->slot, ep->dci, ring, xhci::phys(dma_buf), length, timeout_ms))
        {
            usb_status st = not_ended(dev);
            if (st == USB_ERR_TIMEOUT)
                uart::printf("usb: slot %u ep %x: bulk transfer timed out\n",
                             (uint32_t)dev->slot, (uint32_t)ep->address);
            return st;
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

    usb_status submit_in(usb_device* dev, usb_endpoint* ep, void* dma_buf, uint32_t length)
    {
        if (dev->gone)
            return USB_ERR_NO_DEVICE;
        if (!ep->on_complete || length > USB_MAX_XFER_BYTES)
            return USB_ERR_INVALID_PARAM;
        ep->submitted = length;
        ep->ring.on_complete = endpoint_complete;
        ep->ring.owner = ep;
        xhci::normal_start(dev->hc, dev->slot, ep->dci, &ep->ring, xhci::phys(dma_buf), length);
        return USB_OK;
    }

    usb_status control_start(usb_device* dev, uint8_t request_type, uint8_t request, uint16_t value,
                             uint16_t index, void* dma_buf, uint16_t length)
    {
        if (dev->gone)
            return USB_ERR_NO_DEVICE;
        if (control_pending(dev))
            return USB_ERR_NOT_READY;
        uint8_t setup[8];
        setup_packet(setup, request_type, request, value, index, length);
        xhci::control_start(dev->hc, dev->slot, &dev->ep0.ring, setup,
                            length ? xhci::phys(dma_buf) : 0, length,
                            (request_type & USB_DIR_IN) != 0);
        return USB_OK;
    }

    bool control_pending(usb_device* dev)
    {
        // Every control transfer ends with done set; only one still going
        // (or one that never came back) leaves it clear.
        return !dev->ep0.ring.done;
    }

    void tick()
    {
        for (uint8_t i = 0; xhci::controller(i); i++)
            xhci::poll(xhci::controller(i));
        for (uint8_t i = 0; xhci::controller(i); i++)
            if (xhci::ports_changed(xhci::controller(i)))
                wait::wake_up(&hotplug_wq);
        for (const usb_class_driver* drv : class_drivers)
            if (drv->tick)
                drv->tick();
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

    uint8_t get_controller_count()
    {
        uint8_t n = 0;
        while (xhci::controller(n))
            n++;
        return n;
    }

    void get_controller_location(uint8_t ctrl, uint8_t* bus, uint8_t* dev, uint8_t* fn)
    {
        xhci_controller* hc = xhci::controller(ctrl);
        PCIDevice* pci = hc ? xhci::pci_device(hc) : nullptr;
        *bus = pci ? pci->bus : 0;
        *dev = pci ? pci->device : 0;
        *fn  = pci ? pci->function : 0;
    }

    uint32_t get_context_entry_size(uint8_t ctrl)
    {
        xhci_controller* hc = xhci::controller(ctrl);
        return hc ? xhci::context_entry_size(hc) : 32;
    }

    uint8_t get_port_count(uint8_t ctrl)
    {
        xhci_controller* hc = xhci::controller(ctrl);
        return hc ? xhci::port_count(hc) : 0;
    }

    // Raw PORTSC of one root port, for `usbports`. Zero when the controller
    // did not come up, which the caller reports as such.
    uint32_t get_port_status(uint8_t ctrl, uint8_t port)
    {
        xhci_controller* hc = xhci::controller(ctrl);
        return hc ? xhci::port_status(hc, port) : 0;
    }

    bool port_is_usb3(uint8_t ctrl, uint8_t port)
    {
        xhci_controller* hc = xhci::controller(ctrl);
        return hc && xhci::port_is_usb3(hc, port);
    }

    uint8_t get_device_count()
    {
        uint8_t n = 0;
        for (uint8_t i = 0; i < MAX_USB_DEVICES; i++)
            if (devices[i].in_use)
                n++;
        return n;
    }

    usb_status get_device_info(uint8_t index, usb_device_info* out)
    {
        if (!out)
            return USB_ERR_INVALID_PARAM;

        // The index-th live device.
        const usb_device* dev = nullptr;
        for (uint8_t i = 0; i < MAX_USB_DEVICES && !dev; i++)
            if (devices[i].in_use && index-- == 0)
                dev = &devices[i];
        if (!dev)
            return USB_ERR_NOT_FOUND;
        memory::memset((uint8_t*)out, 0, sizeof(*out));
        out->controller = controller_index(dev->hc);
        out->slot_id = dev->slot;
        device_path(dev, out->path, sizeof(out->path));
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
