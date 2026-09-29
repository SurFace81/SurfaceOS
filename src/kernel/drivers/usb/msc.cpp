// USB mass storage: Bulk-Only Transport carrying SCSI commands. Every such
// device becomes a whole disk, usb0, usb1, ..., in the block layer.

#include "../../../include/drivers/usb/usb.h"
#include "../../../include/dev/blkdev.h"
#include "../../../include/errno.h"

#define USB_CLASS_MASS_STORAGE  0x08
#define USB_SUBCLASS_SCSI       0x06
#define USB_PROTOCOL_BBB        0x50

// Bulk-Only Transport structures (USB Mass Storage spec)

struct usb_cbw {
    uint32_t dCBWSignature;
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t  bmCBWFlags;
    uint8_t  bCBWLUN;
    uint8_t  bCBWCBLength;
    uint8_t  CBWCB[16];
} __attribute__((packed));

struct usb_csw {
    uint32_t dCSWSignature;
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t  bCSWStatus;
} __attribute__((packed));

// BOT signatures and flags
#define USB_CBW_SIGNATURE  0x43425355
#define USB_CSW_SIGNATURE  0x53425355
#define USB_CBW_FLAG_IN    0x80
#define USB_CBW_FLAG_OUT   0x00

// SCSI opcodes
#define SCSI_TEST_UNIT_READY    0x00
#define SCSI_REQUEST_SENSE      0x03
#define SCSI_INQUIRY            0x12
#define SCSI_READ_CAPACITY_10   0x25
#define SCSI_READ_10            0x28
#define SCSI_WRITE_10           0x2A
#define SCSI_SYNCHRONIZE_CACHE  0x35

// Sense keys (REQUEST SENSE, fixed format byte 2)
#define SCSI_SENSE_ILLEGAL_REQUEST  0x05

static const uint32_t BULK_TIMEOUT_MS = 2000;

// READ(10)/WRITE(10) carry a 32-bit LBA: a device whose last LBA does not
// fit, or that reports READ CAPACITY(10)'s 0xFFFFFFFF "use READ(16)"
// marker, is refused rather than silently wrapped.
static const uint64_t MAX_LBA32_SECTORS = 0xFFFFFFFFULL;

struct msc_dev
{
    usb_device* dev;
    usb_endpoint* in;
    usb_endpoint* out;
    uint32_t block_size;
    uint32_t last_lba;
    // One reusable DMA bounce buffer, USB_MAX_XFER_BYTES long. Allocating
    // per request made every FAT sector read a kmalloc + a DMA-capable
    // carve-out; on real USB sticks that dominated small-transfer latency.
    uint8_t* dma_buf;
    // SYNCHRONIZE CACHE was rejected as an unknown command: the device has
    // no cache it lets us flush, so flushes are skipped from then on.
    bool no_sync_cache;
    // Unplugged: its disk stays registered, but every request fails.
    bool gone;
};

#define MAX_MSC_DEVS 8
static msc_dev msc_devs[MAX_MSC_DEVS];
static uint8_t msc_count = 0;

// Command and status blocks, shared: one command is in flight at a time.
static usb_cbw* cbw = nullptr;
static usb_csw* csw = nullptr;
static uint32_t bot_tag = 1;

// BOT (Bulk-Only Transport)

// A bulk transfer; a stalled endpoint is cleared and reported as a stall.
static usb_status bulk(msc_dev* m, usb_endpoint* ep, void* buf, uint32_t length, uint32_t* actual)
{
    usb_status st = usb::bulk(m->dev, ep, buf, length, actual, BULK_TIMEOUT_MS);
    if (st == USB_ERR_STALL)
        usb::clear_halt(m->dev, ep);
    return st;
}

// One SCSI command through BOT: the command block out, the data in or out,
// the status block in. 0 on success, the CSW status (1: failed, 2: phase
// error) when the device refused, -1 when the transport broke.
static sint32_t bot_scsi_command(msc_dev* m, const uint8_t* scsi_cmd, uint8_t scsi_cmd_len, void* data_buf,
                                 uint32_t data_length, uint8_t direction)
{
    memory::memset((uint8_t*)cbw, 0, sizeof(usb_cbw));
    cbw->dCBWSignature = USB_CBW_SIGNATURE;
    cbw->dCBWTag = bot_tag++;
    cbw->dCBWDataTransferLength = data_length;
    cbw->bmCBWFlags = direction;
    cbw->bCBWLUN = 0;
    cbw->bCBWCBLength = scsi_cmd_len;
    memory::memcpy(cbw->CBWCB, scsi_cmd, scsi_cmd_len);

    if (bulk(m, m->out, cbw, 31, nullptr) != USB_OK)
    {
        uart::printf("bot: CBW send failed\n");
        return -1;
    }

    // A stall in the data phase ends it early; the status still follows.
    if (data_length > 0 && data_buf)
    {
        usb_endpoint* ep = direction == USB_CBW_FLAG_IN ? m->in : m->out;
        usb_status st = bulk(m, ep, data_buf, data_length, nullptr);
        if (st != USB_OK && st != USB_ERR_STALL)
            return -1;
    }

    // The status phase may stall once; after clearing it the CSW comes.
    memory::memset((uint8_t*)csw, 0, sizeof(usb_csw));
    uint32_t got = 0;
    usb_status st = bulk(m, m->in, csw, 13, &got);
    if (st == USB_ERR_STALL)
        st = bulk(m, m->in, csw, 13, &got);
    if (st != USB_OK || got < 13)
    {
        uart::printf("bot: CSW receive failed\n");
        return -1;
    }

    if (csw->dCSWSignature != USB_CSW_SIGNATURE)
    {
        uart::printf("bot: invalid CSW signature 0x%x\n", csw->dCSWSignature);
        return -1;
    }

    if (csw->bCSWStatus != 0)
    {
        uart::printf("bot: command 0x%x failed status=%u\n", (uint32_t)scsi_cmd[0],
                     (uint32_t)csw->bCSWStatus);
        return (sint32_t)csw->bCSWStatus;
    }

    return 0;
}

// SCSI commands

static bool scsi_inquiry(msc_dev* m)
{
    uint8_t* data = (uint8_t*)usb::dma_alloc(36);
    if (!data)
        return false;

    uint8_t cmd[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    sint32_t result = bot_scsi_command(m, cmd, 6, data, 36, USB_CBW_FLAG_IN);
    if (result != 0)
    {
        uart::printf("scsi: INQUIRY failed\n");
        usb::dma_free(data);
        return false;
    }

    // Vendor and product identification, space-padded ASCII.
    memory::memcpy((uint8_t*)m->dev->vendor, data + 8, 8);
    m->dev->vendor[8] = '\0';
    memory::memcpy((uint8_t*)m->dev->product, data + 16, 16);
    m->dev->product[16] = '\0';

    usb::dma_free(data);
    return true;
}

// Wait for the device to accept commands. Real sticks need noticeably longer
// after reset than QEMU's emulated one, so the deadline is wall-clock (PIT),
// not a fixed retry count: poll TEST UNIT READY for up to 5 s.
static bool scsi_test_unit_ready(msc_dev* m)
{
    uint8_t cmd[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };

    const uint32_t TIMEOUT_MS = 5000;
    const uint32_t POLL_MS    = 100;

    for (uint32_t waited = 0; waited <= TIMEOUT_MS; waited += POLL_MS)
    {
        sint32_t result = bot_scsi_command(m, cmd, 6, nullptr, 0, USB_CBW_FLAG_OUT);
        if (result == 0)
            return true;
        if (waited == TIMEOUT_MS)
            break;
        xhci::delay_ms(POLL_MS);
    }
    uart::printf("scsi: TEST UNIT READY timed out\n");
    return false;
}

static bool scsi_read_capacity(msc_dev* m)
{
    uint8_t* data = (uint8_t*)usb::dma_alloc(8);
    if (!data)
        return false;

    uint8_t cmd[10] = { SCSI_READ_CAPACITY_10 };
    sint32_t result = bot_scsi_command(m, cmd, 10, data, 8, USB_CBW_FLAG_IN);
    if (result != 0)
    {
        uart::printf("scsi: READ CAPACITY failed\n");
        usb::dma_free(data);
        return false;
    }

    m->last_lba =
        ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | (uint32_t)data[3];
    m->block_size =
        ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) | ((uint32_t)data[6] << 8) | (uint32_t)data[7];

    usb::dma_free(data);
    return true;
}

// READ(10) / WRITE(10) of `count` blocks through the bounce buffer.
static bool scsi_read_write_10(msc_dev* m, uint8_t opcode, uint32_t lba, uint16_t count)
{
    uint8_t cmd[10] = {
        opcode, 0,
        (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
        0,
        (uint8_t)(count >> 8), (uint8_t)count,
        0,
    };

    // The transfer length the CDB/CBW advertise must match the real sector
    // size: it used to be hardcoded *512, which silently transferred 1/8 of
    // the data on a 4096-byte-sector device.
    uint32_t byte_count = (uint32_t)count * m->block_size;
    uint8_t direction = opcode == SCSI_READ_10 ? USB_CBW_FLAG_IN : USB_CBW_FLAG_OUT;
    return bot_scsi_command(m, cmd, 10, m->dma_buf, byte_count, direction) == 0;
}

// Fetch the sense data of the command that just failed. Returns the sense
// key, or -1 when even that did not work.
static sint32_t scsi_request_sense(msc_dev* m)
{
    const uint32_t len = 18;            // fixed-format sense data
    uint8_t* data = (uint8_t*)usb::dma_alloc(len);
    if (!data)
        return -1;

    uint8_t cmd[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, (uint8_t)len, 0 };
    sint32_t result = bot_scsi_command(m, cmd, 6, data, len, USB_CBW_FLAG_IN);
    sint32_t key = result == 0 ? (sint32_t)(data[2] & 0x0F) : -1;
    usb::dma_free(data);
    return key;
}

static bool scsi_synchronize_cache(msc_dev* m)
{
    if (m->no_sync_cache)
        return true;

    uint8_t cmd[10] = { SCSI_SYNCHRONIZE_CACHE };  // whole LBA range, no data phase
    sint32_t result = bot_scsi_command(m, cmd, 10, nullptr, 0, USB_CBW_FLAG_OUT);
    if (result == 0)
        return true;

    // Plenty of USB sticks do not implement SYNCHRONIZE CACHE and answer
    // with ILLEGAL REQUEST. They write through (or manage their cache on
    // their own), so there is nothing to flush. Anything else is a real
    // failure.
    if (result == 1 && scsi_request_sense(m) == SCSI_SENSE_ILLEGAL_REQUEST)
    {
        uart::printf("scsi: SYNCHRONIZE CACHE not supported, flushes skipped\n");
        m->no_sync_cache = true;
        return true;
    }
    return false;
}

// The block layer's view (blkdev.h)

// One request never exceeds the bounce buffer (max_sectors_per_io) nor the
// device (block::read/write check the range); checked again here, since a
// bogus LBA from a broken filesystem must never reach the device.
static bool request_fits(msc_dev* m, uint64_t lba, uint32_t count)
{
    return count > 0 && (uint64_t)count * m->block_size <= USB_MAX_XFER_BYTES &&
           lba + count <= (uint64_t)m->last_lba + 1;
}

static sint64_t msc_read(blkdev* bdev, uint64_t lba, uint32_t count, void* buf)
{
    msc_dev* m = (msc_dev*)bdev->priv;
    if (m->gone)
        return -EIO;
    if (!request_fits(m, lba, count))
        return -EINVAL;
    if (!scsi_read_write_10(m, SCSI_READ_10, (uint32_t)lba, (uint16_t)count))
        return -EIO;
    memory::memcpy((uint8_t*)buf, m->dma_buf, count * m->block_size);
    return 0;
}

static sint64_t msc_write(blkdev* bdev, uint64_t lba, uint32_t count, const void* buf)
{
    msc_dev* m = (msc_dev*)bdev->priv;
    if (m->gone)
        return -EIO;
    if (!request_fits(m, lba, count))
        return -EINVAL;
    memory::memcpy(m->dma_buf, (const uint8_t*)buf, count * m->block_size);
    return scsi_read_write_10(m, SCSI_WRITE_10, (uint32_t)lba, (uint16_t)count) ? 0 : -EIO;
}

static sint64_t msc_flush(blkdev* bdev)
{
    msc_dev* m = (msc_dev*)bdev->priv;
    if (m->gone)
        return -EIO;
    return scsi_synchronize_cache(m) ? 0 : -EIO;
}

static blkdev_ops msc_ops = { msc_read, msc_write, msc_flush };

// The device as a whole disk. False when its geometry is unusable.
static bool register_disk(msc_dev* m, uint8_t index)
{
    uint64_t sectors = (uint64_t)m->last_lba + 1;
    if (m->last_lba == MAX_LBA32_SECTORS || sectors > MAX_LBA32_SECTORS)
    {
        uart::printf("msc: >= 2 TiB (last_lba=%u) - READ(16) not supported yet, skipped\n",
                     m->last_lba);
        return false;
    }
    if (m->block_size == 0 || (m->block_size & (m->block_size - 1)) != 0 ||
        m->block_size < 512 || m->block_size > 4096)
    {
        uart::printf("msc: bogus sector size %u, skipped\n", m->block_size);
        return false;
    }

    blkdev d;
    memory::memset((uint8_t*)&d, 0, sizeof(d));
    d.name[0] = 'u'; d.name[1] = 's'; d.name[2] = 'b';
    if (index >= 10)
    {
        d.name[3] = (char)('0' + index / 10);
        d.name[4] = (char)('0' + index % 10);
    }
    else
        d.name[3] = (char)('0' + index);
    d.sector_size        = m->block_size;
    d.sector_count       = sectors;
    d.max_sectors_per_io = USB_MAX_XFER_BYTES / m->block_size;
    d.ops                = &msc_ops;
    d.priv               = m;

    if (block::register_dev(&d) != 0)
        return false;
    uart::printf("msc: %s: %s %s, %uB x %u (%u MB)\n", d.name, m->dev->vendor, m->dev->product,
                 d.sector_size, (uint32_t)sectors,
                 (uint32_t)(sectors * d.sector_size / (1024 * 1024)));
    return true;
}

static bool msc_probe(usb_device* dev, const usb_interface_descriptor* iface)
{
    if (iface->bInterfaceClass != USB_CLASS_MASS_STORAGE || iface->bInterfaceSubClass != USB_SUBCLASS_SCSI ||
        iface->bInterfaceProtocol != USB_PROTOCOL_BBB)
        return false;
    if (msc_count >= MAX_MSC_DEVS)
    {
        uart::printf("msc: too many devices\n");
        return false;
    }

    const usb_endpoint_descriptor* bulk_in = nullptr;
    const usb_endpoint_descriptor* bulk_out = nullptr;
    for (uint8_t i = 0;; i++)
    {
        const usb_endpoint_descriptor* ep = usb::interface_endpoint(dev, iface, i);
        if (!ep)
            break;
        if ((ep->bmAttributes & 0x03) != USB_EP_BULK)
            continue;
        if (ep->bEndpointAddress & 0x80)
            bulk_in = ep;
        else
            bulk_out = ep;
    }
    if (!bulk_in || !bulk_out)
        return false;

    if (!cbw)
    {
        cbw = (usb_cbw*)usb::dma_alloc(sizeof(usb_cbw));
        csw = (usb_csw*)usb::dma_alloc(sizeof(usb_csw));
    }

    msc_dev* m = &msc_devs[msc_count];
    memory::memset((uint8_t*)m, 0, sizeof(*m));
    m->dev = dev;

    if (usb::set_configuration(dev) != USB_OK)
        return false;
    const usb_endpoint_descriptor* descs[2] = { bulk_in, bulk_out };
    usb_endpoint* eps[2];
    if (usb::open_endpoints(dev, descs, 2, eps) != USB_OK)
        return false;
    m->in = eps[0];
    m->out = eps[1];

    scsi_inquiry(m);
    if (!scsi_test_unit_ready(m) || !scsi_read_capacity(m))
        return false;

    m->dma_buf = (uint8_t*)usb::dma_alloc(USB_MAX_XFER_BYTES);
    if (!m->dma_buf || !register_disk(m, msc_count))
        return false;

    msc_count++;
    return true;
}

static void msc_disconnect(usb_device* dev)
{
    for (uint8_t i = 0; i < msc_count; i++)
    {
        msc_dev* m = &msc_devs[i];
        if (m->gone || m->dev != dev)
            continue;
        uart::printf("msc: usb%u: device gone, its disk fails from now on\n", (uint32_t)i);
        m->gone = true;
        m->dev = nullptr;
        usb::dma_free(m->dma_buf);
        m->dma_buf = nullptr;
    }
}

extern const usb_class_driver msc_driver = { "msc", msc_probe, msc_disconnect, nullptr };
