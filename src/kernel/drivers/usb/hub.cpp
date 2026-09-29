// USB hubs (class 9): USB 2.0 hubs (USB 2.0, chapter 11) and SuperSpeed
// hubs (USB 3.x, chapter 10). A USB 3 hub is two hubs to the host - one on
// a USB 3 root port, one on its USB 2 companion - and each is driven here
// on its own.
//
// At probe the hub's ports are powered and what is on them enumerated,
// through the USB core, which offers each new device to the class drivers
// again - another hub included. Later changes come as a bitmap on the hub's
// interrupt endpoint (bit n: port n changed, bit 0: the hub itself); the
// completion only notes them, and the usb kernel process handles them in
// work(), where waiting for the hub's answers is allowed.

#include "../../../include/drivers/usb/usb.h"

#define USB_CLASS_HUB           0x09

#define HUB_DESC_USB2           0x29
#define HUB_DESC_USB3           0x2A
#define HUB_REQ_SET_HUB_DEPTH   12

// Hub features (clear)
#define C_HUB_LOCAL_POWER       0
#define C_HUB_OVER_CURRENT      1

// Port features
#define PORT_RESET              4
#define PORT_POWER              8
#define C_PORT_CONNECTION       16
#define C_PORT_RESET            20

// Port status bits (the USB 2 and USB 3 layouts share these two)
#define PORT_STAT_CONNECTION    (1 << 0)
#define PORT_STAT_ENABLE        (1 << 1)
// USB 2 only: the speed of what is attached
#define PORT_STAT_LOW_SPEED     (1 << 9)
#define PORT_STAT_HIGH_SPEED    (1 << 10)
// Port change bits
#define PORT_CHANGE_CONNECTION  (1 << 0)
#define PORT_CHANGE_RESET       (1 << 4)

// A route string has five tiers; a hub at the last one has nowhere to put
// the devices on its ports.
#define MAX_HUB_TIER            4

// Ports we handle per hub: the change bitmap then fits in 32 bits.
#define MAX_HUB_PORTS           31

static const uint32_t RESET_TIMEOUT_MS  = 500;
static const uint32_t RESET_RECOVERY_MS = 10;    // TRSTRCY
static const uint32_t DEBOUNCE_MS       = 100;   // TATTDB

// Each set change bit, by the feature that clears it; bit n of wPortChange.
static const uint8_t usb2_change_features[] = { 16, 17, 18, 19, 20 };
static const uint8_t usb3_change_features[] = { 16, 0, 0, 19, 20, 29, 25, 26 };

struct hub
{
    usb_device* dev;                // null: a free entry
    usb_endpoint* ep;
    uint8_t ports;
    bool usb3;
    uint8_t* change;                // DMA: the change bitmap being received
    uint8_t change_len;
    volatile uint32_t pending;      // changes reported, not handled yet
    bool failed;                    // interrupt endpoint error: not polled
};

#define MAX_HUBS 8
static hub hubs[MAX_HUBS];

static bool port_status(hub* h, uint8_t port, uint16_t* status, uint16_t* change)
{
    uint8_t buf[4];
    uint16_t got = 0;
    if (usb::control(h->dev, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER, USB_REQ_GET_STATUS,
                     0, port, buf, sizeof(buf), &got) != USB_OK || got < 4)
        return false;
    *status = (uint16_t)(buf[0] | (buf[1] << 8));
    *change = (uint16_t)(buf[2] | (buf[3] << 8));
    return true;
}

static void port_feature(hub* h, bool set, uint16_t feature, uint8_t port)
{
    usb::control(h->dev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER,
                 set ? USB_REQ_SET_FEATURE : USB_REQ_CLEAR_FEATURE, feature, port,
                 nullptr, 0, nullptr);
}

// Acknowledge every change the port reports, so the hub stops reporting it.
static void clear_changes(hub* h, uint8_t port, uint16_t change)
{
    const uint8_t* features = h->usb3 ? usb3_change_features : usb2_change_features;
    uint8_t count = h->usb3 ? sizeof(usb3_change_features) : sizeof(usb2_change_features);
    for (uint8_t bit = 0; bit < count; bit++)
        if ((change & (1 << bit)) && features[bit])
            port_feature(h, false, features[bit], port);
}

// Reset the port and wait for it to come out enabled; *speed is then what
// runs on it, as an xHCI speed ID.
static bool reset_port(hub* h, uint8_t port, uint8_t* speed)
{
    port_feature(h, true, PORT_RESET, port);

    uint16_t status = 0, change = 0;
    uint32_t waited = 0;
    for (;;)
    {
        xhci::delay_ms(10);
        waited += 10;
        if (!port_status(h, port, &status, &change))
            return false;
        if (change & PORT_CHANGE_RESET)
            break;
        if (waited >= RESET_TIMEOUT_MS)
            return false;
    }
    clear_changes(h, port, change);
    xhci::delay_ms(RESET_RECOVERY_MS);

    if (!port_status(h, port, &status, &change) || !(status & PORT_STAT_ENABLE))
        return false;

    if (h->usb3)
        *speed = 4;                             // SuperSpeed
    else if (status & PORT_STAT_LOW_SPEED)
        *speed = 2;
    else if (status & PORT_STAT_HIGH_SPEED)
        *speed = 3;
    else
        *speed = 1;
    return true;
}

// A port that may have changed. Its device, if it had one, is gone when the
// port is empty or saw a new connection; a connected port with no device
// gets one. `debounce`: wait for the contacts to settle first (a plug just
// went in); at probe the power-on wait did that already.
static void port_changed(hub* h, uint8_t port, bool debounce)
{
    uint16_t status, change;
    if (!port_status(h, port, &status, &change))
        return;
    clear_changes(h, port, change);

    bool connected = status & PORT_STAT_CONNECTION;
    usb_device* child = usb::device_on_hub_port(h->dev, port);
    if (child && (!connected || (change & PORT_CHANGE_CONNECTION)))
    {
        usb::detach_device(child);
        child = nullptr;
    }
    if (!connected || child)
        return;

    if (debounce)
    {
        xhci::delay_ms(DEBOUNCE_MS);
        if (!port_status(h, port, &status, &change) || !(status & PORT_STAT_CONNECTION))
            return;
    }

    uint8_t speed;
    if (!reset_port(h, port, &speed))
    {
        uart::printf("hub: slot %u port %u: reset failed\n", (uint32_t)h->dev->slot, (uint32_t)port);
        return;
    }
    usb::enumerate_hub_port(h->dev, port, speed);
}

static void change_done(usb_endpoint* ep, usb_status status, uint32_t actual)
{
    hub* h = (hub*)ep->owner;
    if (!h->dev)
        return;         // unplugged meanwhile
    if (status != USB_OK)
    {
        // Recovering needs synchronous requests, which cannot be made from
        // here; the hub's ports are no longer watched until it is plugged
        // in again.
        uart::printf("hub: slot %u: change report failed (%u), no longer polled\n",
                     (uint32_t)h->dev->slot, (uint32_t)status);
        h->failed = true;
        return;
    }

    uint32_t bits = 0;
    for (uint32_t i = 0; i < actual && i < 4; i++)
        bits |= (uint32_t)h->change[i] << (8 * i);
    __atomic_or_fetch(&h->pending, bits, __ATOMIC_RELAXED);

    usb::submit_in(h->dev, ep, h->change, h->change_len);
    if (bits)
        usb::wake_hotplug();
}

// In the usb kernel process: the changes the hubs reported.
static void hub_work()
{
    for (uint8_t i = 0; i < MAX_HUBS; i++)
    {
        hub* h = &hubs[i];
        if (!h->dev)
            continue;
        uint32_t bits = __atomic_exchange_n(&h->pending, 0, __ATOMIC_RELAXED);

        // The hub itself: power or over-current changed. Nothing to do but
        // acknowledge it, or it keeps being reported.
        if (bits & 1)
        {
            uint8_t type = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_DEVICE;
            usb::control(h->dev, type, USB_REQ_CLEAR_FEATURE, C_HUB_LOCAL_POWER, 0, nullptr, 0, nullptr);
            usb::control(h->dev, type, USB_REQ_CLEAR_FEATURE, C_HUB_OVER_CURRENT, 0, nullptr, 0, nullptr);
        }

        // A port change may unplug a hub further down, h included once the
        // entry is reused: stop when it is no longer this hub's.
        usb_device* dev = h->dev;
        for (uint8_t port = 1; port <= h->ports && h->dev == dev; port++)
            if (bits & (1u << port))
                port_changed(h, port, true);
    }
}

static bool hub_probe(usb_device* dev, const usb_interface_descriptor* iface)
{
    if (iface->bInterfaceClass != USB_CLASS_HUB)
        return false;

    hub* h = nullptr;
    for (uint8_t i = 0; i < MAX_HUBS && !h; i++)
        if (!hubs[i].dev)
            h = &hubs[i];
    if (!h)
    {
        uart::printf("hub: too many hubs\n");
        return false;
    }
    if (dev->tier > MAX_HUB_TIER)
    {
        uart::printf("hub: slot %u: too deep in the tree\n", (uint32_t)dev->slot);
        return false;
    }

    const usb_endpoint_descriptor* int_in = nullptr;
    for (uint8_t i = 0;; i++)
    {
        const usb_endpoint_descriptor* ep = usb::interface_endpoint(dev, iface, i);
        if (!ep)
            break;
        if ((ep->bmAttributes & 0x03) == USB_EP_INTERRUPT && (ep->bEndpointAddress & 0x80))
        {
            int_in = ep;
            break;
        }
    }
    if (!int_in)
        return false;

    // The hub descriptor: port count, characteristics, power-on time.
    bool usb3 = dev->speed >= 4;
    uint8_t desc[16];
    uint16_t got = 0;
    if (usb::control(dev, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR,
                     (usb3 ? HUB_DESC_USB3 : HUB_DESC_USB2) << 8, 0, desc, sizeof(desc), &got) != USB_OK ||
        got < 7)
    {
        uart::printf("hub: slot %u: no hub descriptor\n", (uint32_t)dev->slot);
        return false;
    }
    uint8_t ports = desc[2];
    uint16_t characteristics = (uint16_t)(desc[3] | (desc[4] << 8));
    uint32_t power_ms = (uint32_t)desc[5] * 2;
    if (ports > MAX_HUB_PORTS)
        ports = MAX_HUB_PORTS;

    if (usb::set_configuration(dev) != USB_OK)
        return false;

    // A SuperSpeed hub routes by the route string, and has to know which
    // nibble of it is its own.
    if (usb3 && usb::control(dev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_DEVICE, HUB_REQ_SET_HUB_DEPTH,
                             dev->tier, 0, nullptr, 0, nullptr) != USB_OK)
        return false;

    // The controller has to know it is a hub; a high speed hub's TT think
    // time (bits 5-6) comes along.
    dev->hub_ports = ports;
    dev->hub_think_time = dev->speed == 3 ? (uint8_t)((characteristics >> 5) & 3) : 0;
    usb_endpoint* ep;
    if (usb::open_endpoints(dev, &int_in, 1, &ep) != USB_OK)
        return false;

    memory::memset((uint8_t*)h, 0, sizeof(*h));
    h->ep = ep;
    h->ports = ports;
    h->usb3 = usb3;
    h->change_len = (uint8_t)((ports + 1 + 7) / 8);
    h->change = (uint8_t*)usb::dma_alloc(h->change_len);
    if (!h->change)
        return false;
    ep->on_complete = change_done;
    ep->owner = h;
    h->dev = dev;

    uart::printf("hub: slot %u: %u ports%s\n", (uint32_t)dev->slot, (uint32_t)ports,
                 usb3 ? ", SuperSpeed" : "");

    // Power every port, give it time to come up and what is on it time to
    // connect, then enumerate what is there.
    for (uint8_t port = 1; port <= ports; port++)
        port_feature(h, true, PORT_POWER, port);
    xhci::delay_ms(power_ms + DEBOUNCE_MS);

    for (uint8_t port = 1; port <= ports && h->dev == dev; port++)
        port_changed(h, port, false);

    usb::submit_in(dev, ep, h->change, h->change_len);
    return true;
}

static void hub_disconnect(usb_device* dev)
{
    for (uint8_t i = 0; i < MAX_HUBS; i++)
    {
        hub* h = &hubs[i];
        if (h->dev != dev)
            continue;
        // What was on its ports is gone by now (the core detaches it
        // first).
        h->dev = nullptr;
        usb::dma_free(h->change);
        h->change = nullptr;
        uart::printf("hub: slot %u gone\n", (uint32_t)dev->slot);
    }
}

extern const usb_class_driver hub_driver = {
    "hub", hub_probe, hub_disconnect, nullptr, hub_work
};
