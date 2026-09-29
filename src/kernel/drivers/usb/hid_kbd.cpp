// USB keyboard in the HID boot protocol: an 8-byte report per change -
// a modifier bitmap, a reserved byte and up to six keys held down, as HID
// usage IDs. The difference from the previous report is what went down and
// up; each such key goes to kbd.cpp as the same physical key a PS/2 board
// would report, so both kinds of keyboard work side by side.
//
// A USB keyboard does not repeat a held key the way a PS/2 one does; the
// repeat is made here, from the timer tick.

#include "../../../include/drivers/usb/usb.h"
#include "../../../include/drivers/kbd.h"
#include "../../../include/drivers/pit.h"

#define USB_CLASS_HID           0x03
#define HID_SUBCLASS_BOOT       0x01
#define HID_PROTOCOL_KEYBOARD   0x01

// Class requests (HID 1.11, 7.2)
#define HID_REQ_SET_REPORT      0x09
#define HID_REQ_SET_IDLE        0x0A
#define HID_REQ_SET_PROTOCOL    0x0B
#define HID_REPORT_OUTPUT       2
#define HID_PROTOCOL_BOOT       0

#define REPORT_LEN              8
#define ERROR_ROLLOVER          0x01    // in every key slot: too many keys held

static const uint32_t REPEAT_DELAY_MS  = 500;
static const uint32_t REPEAT_PERIOD_MS = 33;

// Keyboard page usage ID -> physical key (keyevent.h: set-1 scancode, or
// scancode | 0x80 behind 0xE0). 0: nothing we report.
static const uint8_t usage_to_key[] = {
    // 0x00..0x03: no key, error codes
    0, 0, 0, 0,
    // 0x04..0x1D: a..z
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
    0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,
    // 0x1E..0x27: 1..9, 0
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    // 0x28..0x38: Enter Esc Backspace Tab Space - = [ ] \ Non-US# ; ' ` , . /
    0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x2B, 0x27, 0x28,
    0x29, 0x33, 0x34, 0x35,
    // 0x39: Caps Lock; 0x3A..0x45: F1..F12
    0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,
    // 0x46..0x48: Print Screen, Scroll Lock, Pause
    KEY_PRINT_SCREEN, 0x46, KEY_PAUSE,
    // 0x49..0x52: Insert Home PgUp Delete End PgDn Right Left Down Up
    KEY_INSERT, KEY_HOME, KEY_PAGE_UP, KEY_DELETE, KEY_END, KEY_PAGE_DOWN,
    KEY_ARROW_RIGHT, KEY_ARROW_LEFT, KEY_ARROW_DOWN, KEY_ARROW_UP,
    // 0x53..0x58: Num Lock, KP / * - + Enter
    0x45, KEY_KP_SLASH, 0x37, 0x4A, 0x4E, KEY_KP_ENTER,
    // 0x59..0x63: KP 1..9, 0, . - the keypad block itself; kbd decides
    // by Num Lock whether it types or navigates
    0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49, 0x52, 0x53,
    // 0x64: Non-US \, 0x65: Menu
    0x56, KEY_MENU,
};

// Modifier bitmap, bit 0..7.
static const uint8_t modifier_keys[8] = {
    KEY_LEFT_CTRL, KEY_LEFT_SHIFT, KEY_LEFT_ALT, KEY_LEFT_WIN,
    KEY_RIGHT_CTRL, KEY_RIGHT_SHIFT, KEY_RIGHT_ALT, KEY_RIGHT_WIN,
};

struct hid_kbd
{
    usb_device* dev;
    usb_endpoint* ep;
    uint8_t interface;
    uint8_t* report;            // DMA: the report being received
    uint8_t prev[REPORT_LEN];   // the last one taken in
    uint8_t* led_buf;           // DMA: the output report being sent
    uint8_t leds;               // lock state the LEDs show; 0xFF: unknown
    uint8_t repeat_key;         // held key to repeat, 0: none
    uint64_t repeat_at;         // uptime (ms) of its next repeat
    bool failed;                // endpoint error: no longer polled
};

#define MAX_KEYBOARDS 4
static hid_kbd keyboards[MAX_KEYBOARDS];
static volatile uint8_t keyboard_count = 0;

static uint8_t key_of(uint8_t usage)
{
    return usage < sizeof(usage_to_key) ? usage_to_key[usage] : 0;
}

static bool holds(const uint8_t* report, uint8_t usage)
{
    for (uint8_t i = 2; i < REPORT_LEN; i++)
        if (report[i] == usage)
            return true;
    return false;
}

// Held down, a key repeats - except the locks, which would toggle, and
// Pause, which has no release to stop it.
static bool repeats(uint8_t key)
{
    return key != KEY_CAPS_LOCK && key != KEY_NUM_LOCK && key != KEY_SCROLL_LOCK && key != KEY_PAUSE;
}

static void take_report(hid_kbd* kb, const uint8_t* r)
{
    // Too many keys at once: the report says nothing about which. Keep
    // the old state until the board knows again.
    if (r[2] == ERROR_ROLLOVER)
        return;

    uint8_t changed = r[0] ^ kb->prev[0];
    for (uint8_t bit = 0; bit < 8; bit++)
        if (changed & (1 << bit))
            kbd::key(modifier_keys[bit], (r[0] >> bit) & 1);

    for (uint8_t i = 2; i < REPORT_LEN; i++)
    {
        uint8_t usage = kb->prev[i];
        uint8_t key = key_of(usage);
        if (!key || holds(r, usage))
            continue;
        if (key == kb->repeat_key)
            kb->repeat_key = 0;
        if (key != KEY_PAUSE)           // PS/2 reports no release for it either
            kbd::key(key, false);
    }

    for (uint8_t i = 2; i < REPORT_LEN; i++)
    {
        uint8_t usage = r[i];
        uint8_t key = key_of(usage);
        if (!key || holds(kb->prev, usage))
            continue;
        kbd::key(key, true);
        if (repeats(key))
        {
            kb->repeat_key = key;
            kb->repeat_at = pit::uptime_ms() + REPEAT_DELAY_MS;
        }
    }

    memory::memcpy(kb->prev, r, REPORT_LEN);
}

static void report_done(usb_endpoint* ep, usb_status status, uint32_t actual)
{
    hid_kbd* kb = (hid_kbd*)ep->owner;
    if (status != USB_OK)
    {
        // Recovering needs synchronous requests, which cannot be made from
        // here; the keyboard stays silent until it is plugged in again.
        uart::printf("hid: keyboard on slot %u: report failed (%u), no longer polled\n",
                     (uint32_t)kb->dev->slot, (uint32_t)status);
        kb->failed = true;
        kb->repeat_key = 0;
        return;
    }

    if (actual >= 3)        // modifiers, reserved, at least one key slot
    {
        uint8_t r[REPORT_LEN] = {};
        memory::memcpy(r, kb->report, actual < REPORT_LEN ? actual : REPORT_LEN);
        take_report(kb, r);
    }
    usb::submit_in(kb->dev, ep, kb->report, REPORT_LEN);
}

static void hid_kbd_tick()
{
    uint64_t now = pit::uptime_ms();
    uint8_t locks = kbd::locks();

    for (uint8_t i = 0; i < keyboard_count; i++)
    {
        hid_kbd* kb = &keyboards[i];
        if (kb->failed)
            continue;

        if (kb->repeat_key && now >= kb->repeat_at)
        {
            kbd::key(kb->repeat_key, true);
            kb->repeat_at = now + REPEAT_PERIOD_MS;
        }

        // The LEDs follow the lock state, whichever keyboard changed it.
        if (locks != kb->leds && !usb::control_pending(kb->dev))
        {
            uint8_t out = 0;
            if (locks & KMOD_NUM)    out |= 0x01;
            if (locks & KMOD_CAPS)   out |= 0x02;
            if (locks & KMOD_SCROLL) out |= 0x04;
            kb->led_buf[0] = out;
            usb::control_start(kb->dev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                               HID_REQ_SET_REPORT, HID_REPORT_OUTPUT << 8, kb->interface,
                               kb->led_buf, 1);
            kb->leds = locks;
        }
    }
}

static bool hid_kbd_probe(usb_device* dev, const usb_interface_descriptor* iface)
{
    if (iface->bInterfaceClass != USB_CLASS_HID || iface->bInterfaceSubClass != HID_SUBCLASS_BOOT ||
        iface->bInterfaceProtocol != HID_PROTOCOL_KEYBOARD)
        return false;
    if (keyboard_count >= MAX_KEYBOARDS)
    {
        uart::printf("hid: too many keyboards\n");
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

    if (usb::set_configuration(dev) != USB_OK)
        return false;

    // The boot report is the one format known without parsing the report
    // descriptor. Idle 0: a report only when something changes - the
    // repeat is ours. Some boards refuse SET_IDLE; they still work.
    uint8_t type = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    if (usb::control(dev, type, HID_REQ_SET_PROTOCOL, HID_PROTOCOL_BOOT, iface->bInterfaceNumber,
                     nullptr, 0, nullptr) != USB_OK)
        return false;
    usb::control(dev, type, HID_REQ_SET_IDLE, 0, iface->bInterfaceNumber, nullptr, 0, nullptr);

    usb_endpoint* ep;
    if (usb::open_endpoints(dev, &int_in, 1, &ep) != USB_OK)
        return false;

    hid_kbd* kb = &keyboards[keyboard_count];
    memory::memset((uint8_t*)kb, 0, sizeof(*kb));
    kb->dev = dev;
    kb->ep = ep;
    kb->interface = iface->bInterfaceNumber;
    kb->report = (uint8_t*)usb::dma_alloc(REPORT_LEN);
    kb->led_buf = (uint8_t*)usb::dma_alloc(1);
    kb->leds = 0xFF;
    if (!kb->report || !kb->led_buf)
        return false;

    ep->on_complete = report_done;
    ep->owner = kb;

    // Counted before the first report can come: the tick walks only the
    // keyboards counted, and this one is complete by now.
    keyboard_count++;
    usb::submit_in(dev, ep, kb->report, REPORT_LEN);

    uart::printf("hid: keyboard on slot %u, interface %u\n", (uint32_t)dev->slot,
                 (uint32_t)kb->interface);
    return true;
}

extern const usb_class_driver hid_kbd_driver = { "hid-kbd", hid_kbd_probe, hid_kbd_tick };
