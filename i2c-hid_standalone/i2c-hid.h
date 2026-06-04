/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef I2C_HID_H
#define I2C_HID_H

#include <linux/i2c.h>

/**
 * struct i2chid_ops - Ops provided to the core.
 *
 * @shutdown_tail: called at the end of shutdown.
 * @restore_sequence: hibernation restore sequence.
 */
struct i2chid_ops {
	void (*shutdown_tail)(struct i2chid_ops *ops);
	void (*restore_sequence)(struct i2chid_ops *ops);
};

int i2c_hid_core_probe(struct i2c_client *client, struct i2chid_ops *ops,
		       u16 hid_descriptor_address);
void i2c_hid_core_remove(struct i2c_client *client);

void i2c_hid_core_shutdown(struct i2c_client *client);

extern const struct dev_pm_ops i2c_hid_core_pm;

#endif
