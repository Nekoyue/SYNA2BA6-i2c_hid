// SPDX-License-Identifier: GPL-2.0+
/*
 * Minimal DMI quirk hooks for this standalone SYNA2BA6 build.
 *
 * The upstream i2c-hid core calls these hooks when CONFIG_DMI is enabled.
 * This project does not carry unrelated descriptor overrides or HID quirks, so
 * each hook returns the default "no override" value.
 */

#include <linux/types.h>

#include "i2c-hid.h"

struct i2c_hid_desc *i2c_hid_get_dmi_i2c_hid_desc_override(uint8_t *i2c_name)
{
	return NULL;
}

char *i2c_hid_get_dmi_hid_report_desc_override(uint8_t *i2c_name,
					       unsigned int *size)
{
	return NULL;
}

u32 i2c_hid_get_dmi_quirks(const u16 vendor, const u16 product)
{
	return 0;
}
