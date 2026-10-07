/*
 * P4OS - what the RF app needs from under librtlsdr: the board's own handle
 * of the open device, to stream from it (port/libusb.h).
 */
#pragma once

#include <stdint.h>
#include "aos_hal.h"
#include "../rtlsdr/rtl-sdr.h"

struct rfusb_device_handle;
struct rfusb_device_handle *rtlsdr_p4os_usb_handle(rtlsdr_dev_t *dev);  /* librtlsdr.c, at its end */
aos_usb_raw_t *rfusb_raw(struct rfusb_device_handle *h);
uint8_t rfusb_addr(struct rfusb_device_handle *h);
