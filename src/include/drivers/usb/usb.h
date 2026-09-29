#ifndef USB_H
#define USB_H

// The USB core: devices, their descriptors, enumeration, and the calls a
// class driver (msc.cpp, ...) uses to talk to its device. The controller
// underneath is xhci.cpp.
//
// A class driver is an entry in the table in usb.cpp. Enumeration offers it
// every interface of every device; the driver takes the ones it knows
// (probe returns true) and from then on owns them.

#include "xhci.h"

// USB standard descriptors

struct usb_device_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed));

struct usb_config_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed));

struct usb_interface_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed));

struct usb_endpoint_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed));


// Descriptor types
#define USB_DESC_DEVICE         1
#define USB_DESC_CONFIG         2
#define USB_DESC_INTERFACE      4
#define USB_DESC_ENDPOINT       5

// bmRequestType
#define USB_DIR_OUT             0x00
#define USB_DIR_IN              0x80
#define USB_TYPE_STANDARD       0x00
#define USB_TYPE_CLASS          0x20
#define USB_RECIP_DEVICE        0x00
#define USB_RECIP_INTERFACE     0x01
#define USB_RECIP_ENDPOINT      0x02

// Standard requests
#define USB_REQ_CLEAR_FEATURE       1
#define USB_REQ_GET_DESCRIPTOR      6
#define USB_REQ_SET_CONFIGURATION   9

#define USB_FEATURE_ENDPOINT_HALT   0

// Endpoint transfer types (bmAttributes & 3)
#define USB_EP_CONTROL          0
#define USB_EP_ISOCHRONOUS      1
#define USB_EP_BULK             2
#define USB_EP_INTERRUPT        3

enum usb_status {
    USB_OK = 0,
    USB_ERR_NOT_FOUND,
    USB_ERR_NOT_READY,
    USB_ERR_TIMEOUT,
    USB_ERR_STALL,
    USB_ERR_IO,
    USB_ERR_INVALID_PARAM,
    USB_ERR_NO_DEVICE
};

struct usb_endpoint
{
    uint8_t  address;           // bEndpointAddress: number, 0x80 for IN
    uint8_t  type;              // USB_EP_*
    uint16_t max_packet;
    uint8_t  dci;               // the controller's index for it
    xhci_transfer_ring ring;
};

#define USB_MAX_ENDPOINTS 8     // besides EP0

struct usb_class_driver;

struct usb_device
{
    xhci_controller* hc;
    uint8_t slot;
    uint8_t port;               // root port, 0-based
    uint8_t speed;              // xHCI speed ID

    usb_device_descriptor desc;
    uint8_t* config;            // the whole configuration descriptor
    uint16_t config_len;
    bool configured;            // SET_CONFIGURATION done

    usb_endpoint ep0;
    usb_endpoint eps[USB_MAX_ENDPOINTS];
    uint8_t ep_count;

    const usb_class_driver* driver;     // the one that took it, or null
    // What the driver found out about it, for lsusb (MSC: from INQUIRY).
    char vendor[9];
    char product[17];
};

struct usb_class_driver
{
    const char* name;
    // Offered one interface of a device; true when the driver took it.
    bool (*probe)(usb_device* dev, const usb_interface_descriptor* iface);
};

// For lsusb/usbinfo.
struct usb_device_info {
    uint8_t  slot_id;
    uint8_t  port_index;
    uint8_t  port_speed;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t bcd_usb;
    uint8_t  device_class;
    uint8_t  device_subclass;
    uint8_t  device_protocol;
    char     vendor_str[9];
    char     product_str[17];
    const char* driver;         // the class driver's name, or null
};

namespace usb
{
    // Bring up the controllers and enumerate what is attached. False when
    // no device ended up with a driver.
    bool init();

    // --- For class drivers --------------------------------------------------

    // The i-th endpoint descriptor of an interface, null past the last.
    const usb_endpoint_descriptor* interface_endpoint(usb_device* dev,
                                                      const usb_interface_descriptor* iface,
                                                      uint8_t i);

    // Select the device's (first) configuration. Once per device, whichever
    // driver asks first.
    usb_status set_configuration(usb_device* dev);

    // Open endpoints on the device, in one go; out[i] belongs to descs[i].
    usb_status open_endpoints(usb_device* dev, const usb_endpoint_descriptor* const* descs,
                              uint8_t count, usb_endpoint** out);

    // A control transfer on EP0. `data` is ordinary memory, `length` bytes
    // in the direction bit 7 of request_type gives; *actual (may be null)
    // is what really moved.
    usb_status control(usb_device* dev, uint8_t request_type, uint8_t request, uint16_t value,
                       uint16_t index, void* data, uint16_t length, uint16_t* actual);

    // A bulk transfer. `dma_buf` comes from dma_alloc(); length is at most
    // USB_MAX_XFER_BYTES. *actual (may be null) is what really moved.
    usb_status bulk(usb_device* dev, usb_endpoint* ep, void* dma_buf, uint32_t length,
                    uint32_t* actual, uint32_t timeout_ms);

    // Clear a halted endpoint: on the device and in the controller.
    usb_status clear_halt(usb_device* dev, usb_endpoint* ep);

    // Buffers for bulk transfers, at most USB_MAX_XFER_BYTES; zeroed.
    void* dma_alloc(uint32_t size);
    void  dma_free(void* ptr);

    // --- For reports ---------------------------------------------------------

    uint32_t   get_context_entry_size();
    uint8_t    get_port_count();
    uint32_t   get_port_status(uint8_t port);
    bool       port_is_usb3(uint8_t port);
    uint8_t    get_controller_count();
    void       get_controller_location(uint8_t* bus, uint8_t* dev, uint8_t* fn);
    uint8_t    get_device_count();
    usb_status get_device_info(uint8_t index, usb_device_info* out);

    const char* get_usb_class_name(uint8_t cls);
    const char* get_usb_speed_str(uint8_t speed);
}

// Largest single bulk transfer: what one Normal TRB carries, which may not
// cross a 64 KiB boundary. It covers a 64 KiB FAT32 cluster (a single
// cluster read must not be split by the FS layer); the block layer splits
// anything larger.
#define USB_MAX_XFER_BYTES 65536

#endif // USB_H
