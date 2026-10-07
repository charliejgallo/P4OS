/*
 * P4OS - the part of libusb that librtlsdr uses, on the board's raw USB
 * (aos_hal_usb_raw_*, docs/USB.md "Raw devices"). port/rfusb.c is the other
 * half.
 *
 * Every name is mapped to an rfusb_ one, so the simulator - which links the
 * Mac's real libusb for its own raw USB - never mixes the two. The device
 * list is the host's (aos_hal_usb_devices), control transfers are the
 * HAL's, and the async half (libusb_submit_transfer and the rest) answers
 * "not supported": the app streams through aos_hal_usb_raw_stream_*, which
 * keeps the transfers in flight itself, and never calls
 * rtlsdr_read_async().
 */
#ifndef RF_LIBUSB_SHIM_H
#define RF_LIBUSB_SHIM_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/time.h>

#define LIBUSB_CALL
#define LIBUSB_API_VERSION 0x01000100

#define libusb_context              rfusb_context
#define libusb_device               rfusb_device
#define libusb_device_handle        rfusb_device_handle
#define libusb_device_descriptor    rfusb_device_descriptor
#define libusb_transfer             rfusb_transfer
#define libusb_transfer_cb_fn       rfusb_transfer_cb_fn
#define libusb_init                 rfusb_init
#define libusb_exit                 rfusb_exit
#define libusb_get_device_list      rfusb_get_device_list
#define libusb_free_device_list     rfusb_free_device_list
#define libusb_get_device_descriptor rfusb_get_device_descriptor
#define libusb_get_device           rfusb_get_device
#define libusb_open                 rfusb_open
#define libusb_close                rfusb_close
#define libusb_claim_interface      rfusb_claim_interface
#define libusb_release_interface    rfusb_release_interface
#define libusb_kernel_driver_active rfusb_kernel_driver_active
#define libusb_detach_kernel_driver rfusb_detach_kernel_driver
#define libusb_attach_kernel_driver rfusb_attach_kernel_driver
#define libusb_reset_device         rfusb_reset_device
#define libusb_control_transfer     rfusb_control_transfer
#define libusb_bulk_transfer        rfusb_bulk_transfer
#define libusb_get_string_descriptor_ascii rfusb_get_string_descriptor_ascii
#define libusb_alloc_transfer       rfusb_alloc_transfer
#define libusb_free_transfer        rfusb_free_transfer
#define libusb_submit_transfer      rfusb_submit_transfer
#define libusb_cancel_transfer      rfusb_cancel_transfer
#define libusb_handle_events_timeout_completed rfusb_handle_events_timeout_completed
#define libusb_dev_mem_alloc        rfusb_dev_mem_alloc
#define libusb_dev_mem_free         rfusb_dev_mem_free
#define libusb_fill_bulk_transfer   rfusb_fill_bulk_transfer

typedef struct rfusb_context rfusb_context;
typedef struct rfusb_device rfusb_device;
typedef struct rfusb_device_handle rfusb_device_handle;

enum {
    LIBUSB_SUCCESS = 0, LIBUSB_ERROR_IO = -1, LIBUSB_ERROR_INVALID_PARAM = -2, LIBUSB_ERROR_ACCESS = -3,
    LIBUSB_ERROR_NO_DEVICE = -4, LIBUSB_ERROR_NOT_FOUND = -5, LIBUSB_ERROR_BUSY = -6, LIBUSB_ERROR_TIMEOUT = -7,
    LIBUSB_ERROR_OVERFLOW = -8, LIBUSB_ERROR_PIPE = -9, LIBUSB_ERROR_INTERRUPTED = -10,
    LIBUSB_ERROR_NO_MEM = -11, LIBUSB_ERROR_NOT_SUPPORTED = -12, LIBUSB_ERROR_OTHER = -99,
};
enum { LIBUSB_ENDPOINT_IN = 0x80, LIBUSB_ENDPOINT_OUT = 0x00 };
enum { LIBUSB_REQUEST_TYPE_STANDARD = 0x00, LIBUSB_REQUEST_TYPE_CLASS = 0x20, LIBUSB_REQUEST_TYPE_VENDOR = 0x40 };
enum libusb_transfer_status {
    LIBUSB_TRANSFER_COMPLETED, LIBUSB_TRANSFER_ERROR, LIBUSB_TRANSFER_TIMED_OUT, LIBUSB_TRANSFER_CANCELLED,
    LIBUSB_TRANSFER_STALL, LIBUSB_TRANSFER_NO_DEVICE, LIBUSB_TRANSFER_OVERFLOW,
};

struct rfusb_device_descriptor {
    uint8_t  bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t  iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
};

struct rfusb_transfer;
typedef void (*rfusb_transfer_cb_fn)(struct rfusb_transfer *transfer);
struct rfusb_transfer {
    rfusb_device_handle *dev_handle;
    uint8_t flags;
    unsigned char endpoint, type;
    unsigned int timeout;
    enum libusb_transfer_status status;
    int length, actual_length;
    rfusb_transfer_cb_fn callback;
    void *user_data;
    unsigned char *buffer;
};

int  rfusb_init(rfusb_context **ctx);
void rfusb_exit(rfusb_context *ctx);
ssize_t rfusb_get_device_list(rfusb_context *ctx, rfusb_device ***list);
void rfusb_free_device_list(rfusb_device **list, int unref);
int  rfusb_get_device_descriptor(rfusb_device *dev, struct rfusb_device_descriptor *desc);
rfusb_device *rfusb_get_device(rfusb_device_handle *h);
int  rfusb_open(rfusb_device *dev, rfusb_device_handle **h);
void rfusb_close(rfusb_device_handle *h);
int  rfusb_claim_interface(rfusb_device_handle *h, int intf);
int  rfusb_release_interface(rfusb_device_handle *h, int intf);
int  rfusb_kernel_driver_active(rfusb_device_handle *h, int intf);
int  rfusb_detach_kernel_driver(rfusb_device_handle *h, int intf);
int  rfusb_attach_kernel_driver(rfusb_device_handle *h, int intf);
int  rfusb_reset_device(rfusb_device_handle *h);
int  rfusb_control_transfer(rfusb_device_handle *h, uint8_t type, uint8_t req, uint16_t value, uint16_t index,
                            unsigned char *data, uint16_t len, unsigned int timeout);
int  rfusb_bulk_transfer(rfusb_device_handle *h, unsigned char ep, unsigned char *data, int len, int *got,
                         unsigned int timeout);
int  rfusb_get_string_descriptor_ascii(rfusb_device_handle *h, uint8_t index, unsigned char *data, int len);
struct rfusb_transfer *rfusb_alloc_transfer(int iso_packets);
void rfusb_free_transfer(struct rfusb_transfer *t);
int  rfusb_submit_transfer(struct rfusb_transfer *t);
int  rfusb_cancel_transfer(struct rfusb_transfer *t);
int  rfusb_handle_events_timeout_completed(rfusb_context *ctx, struct timeval *tv, int *completed);
unsigned char *rfusb_dev_mem_alloc(rfusb_device_handle *h, size_t len);
int  rfusb_dev_mem_free(rfusb_device_handle *h, unsigned char *buf, size_t len);

static inline void rfusb_fill_bulk_transfer(struct rfusb_transfer *t, rfusb_device_handle *h, unsigned char ep,
                                            unsigned char *buf, int len, rfusb_transfer_cb_fn cb, void *user,
                                            unsigned int timeout)
{
    t->dev_handle = h;
    t->endpoint = ep;
    t->type = 2;
    t->timeout = timeout;
    t->buffer = buf;
    t->length = len;
    t->user_data = user;
    t->callback = cb;
}

#endif
