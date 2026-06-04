/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Minimal HID IDs used by this standalone i2c-hid build.
 *
 * This project intentionally carries only the Synaptics I2C HID device that
 * needs the local reset-on-resume quirk.
 */

#ifndef HID_IDS_H_FILE
#define HID_IDS_H_FILE

#define I2C_VENDOR_ID_SYNAPTICS		0x06cb
#define I2C_PRODUCT_ID_SYNAPTICS_SYNA2BA6	0xcf00

#endif
