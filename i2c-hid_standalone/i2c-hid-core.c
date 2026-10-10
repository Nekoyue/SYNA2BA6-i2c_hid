/*
 * HID over I2C protocol implementation
 *
 * Copyright (c) 2012 Benjamin Tissoires <benjamin.tissoires@gmail.com>
 * Copyright (c) 2012 Ecole Nationale de l'Aviation Civile, France
 * Copyright (c) 2012 Red Hat, Inc
 *
 * This code is partly based on "USB HID support for Linux":
 *
 *  Copyright (c) 1999 Andreas Gal
 *  Copyright (c) 2000-2005 Vojtech Pavlik <vojtech@suse.cz>
 *  Copyright (c) 2005 Michael Haboustak <mike-@cinci.rr.com> for Concept2, Inc
 *  Copyright (c) 2007-2008 Oliver Neukum
 *  Copyright (c) 2006-2010 Jiri Kosina
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive for
 * more details.
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/pm.h>
#include <linux/pm_wakeirq.h>
#include <linux/device.h>
#include <linux/wait.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/hid.h>
#include <linux/mutex.h>
#include <linux/unaligned.h>

#include "i2c-hid.h"

#define I2C_VENDOR_ID_SYNAPTICS			0x06cb
#define I2C_PRODUCT_ID_SYNAPTICS_SYNA2BA6	0xcf00

/* quirks to control the device */
#define I2C_HID_QUIRK_SYNA_RESYNC		BIT(0)

/* Command opcodes */
#define I2C_HID_OPCODE_RESET			0x01
#define I2C_HID_OPCODE_GET_REPORT		0x02
#define I2C_HID_OPCODE_SET_REPORT		0x03
#define I2C_HID_OPCODE_GET_IDLE			0x04
#define I2C_HID_OPCODE_SET_IDLE			0x05
#define I2C_HID_OPCODE_GET_PROTOCOL		0x06
#define I2C_HID_OPCODE_SET_PROTOCOL		0x07
#define I2C_HID_OPCODE_SET_POWER		0x08

/* flags */
#define I2C_HID_STARTED		0
#define I2C_HID_RESET_PENDING	1

#define I2C_HID_PWR_ON		0x00
#define I2C_HID_PWR_SLEEP	0x01

#define i2c_hid_dbg(ihid, ...) dev_dbg(&(ihid)->client->dev, __VA_ARGS__)

struct i2c_hid_desc {
	__le16 wHIDDescLength;
	__le16 bcdVersion;
	__le16 wReportDescLength;
	__le16 wReportDescRegister;
	__le16 wInputRegister;
	__le16 wMaxInputLength;
	__le16 wOutputRegister;
	__le16 wMaxOutputLength;
	__le16 wCommandRegister;
	__le16 wDataRegister;
	__le16 wVendorID;
	__le16 wProductID;
	__le16 wVersionID;
	__le32 reserved;
} __packed;

/* The main device structure */
struct i2c_hid {
	struct i2c_client	*client;	/* i2c client */
	struct hid_device	*hid;	/* pointer to corresponding HID dev */
	struct i2c_hid_desc hdesc;		/* the HID Descriptor */
	__le16			wHIDDescRegister; /* location of the i2c
						   * register of the HID
						   * descriptor. */
	unsigned int		bufsize;	/* i2c buffer size */
	u8			*inbuf;		/* Input buffer */
	u8			*rawbuf;	/* Raw Input buffer */
	u8			*cmdbuf;	/* Command buffer */

	unsigned long		flags;		/* device flags */
	unsigned long		quirks;		/* Various quirks */

	wait_queue_head_t	wait;		/* For waiting the interrupt */

	struct mutex		cmd_lock;	/* protects cmdbuf and rawbuf */
	struct mutex		reset_lock;

	struct i2chid_ops	*ops;

	/* I2C_HID_QUIRK_SYNA_RESYNC state */
	struct work_struct	syna_resync_work;
	struct list_head	syna_resync_node;	/* on syna_resync_devices */
	unsigned long		last_input_jiffies;	/* for idle detection */
};

/*
 * i2c_hid_lookup_quirk: return any quirks associated with a I2C HID device
 * @idVendor: the 16-bit vendor ID
 * @idProduct: the 16-bit product ID
 *
 * Returns: a u32 quirks value.
 */
static u32 i2c_hid_lookup_quirk(const u16 idVendor, const u16 idProduct)
{
	/*
	 * SYNA2BA6:00 06CB:CF00 can end up in a degraded state after idle or
	 * system sleep where pointer movement still works but multitouch
	 * gestures stop working. See the "Synaptics SYNA2BA6 resync" section
	 * below for the recovery path.
	 */
	if (idVendor == I2C_VENDOR_ID_SYNAPTICS &&
	    idProduct == I2C_PRODUCT_ID_SYNAPTICS_SYNA2BA6)
		return I2C_HID_QUIRK_SYNA_RESYNC;

	return 0;
}

static int i2c_hid_probe_address(struct i2c_hid *ihid)
{
	int ret;

	/*
	 * Some STM-based devices need 400µs after a rising clock edge to wake
	 * from deep sleep, in which case the first read will fail. Try after a
	 * short sleep to see if the device came alive on the bus. Certain
	 * Weida Tech devices also need this.
	 */
	ret = i2c_smbus_read_byte(ihid->client);
	if (ret < 0) {
		usleep_range(400, 500);
		ret = i2c_smbus_read_byte(ihid->client);
	}
	return ret < 0 ? ret : 0;
}

static int i2c_hid_xfer(struct i2c_hid *ihid,
			u8 *send_buf, int send_len, u8 *recv_buf, int recv_len)
{
	struct i2c_client *client = ihid->client;
	struct i2c_msg msgs[2] = { 0 };
	int n = 0;
	int ret;

	if (send_len) {
		i2c_hid_dbg(ihid, "%s: cmd=%*ph\n",
			    __func__, send_len, send_buf);

		msgs[n].addr = client->addr;
		msgs[n].flags = (client->flags & I2C_M_TEN) | I2C_M_DMA_SAFE;
		msgs[n].len = send_len;
		msgs[n].buf = send_buf;
		n++;
	}

	if (recv_len) {
		msgs[n].addr = client->addr;
		msgs[n].flags = (client->flags & I2C_M_TEN) |
				I2C_M_RD | I2C_M_DMA_SAFE;
		msgs[n].len = recv_len;
		msgs[n].buf = recv_buf;
		n++;
	}

	ret = i2c_transfer(client->adapter, msgs, n);

	if (ret != n)
		return ret < 0 ? ret : -EIO;

	return 0;
}

static int i2c_hid_read_register(struct i2c_hid *ihid, __le16 reg,
				 void *buf, size_t len)
{
	guard(mutex)(&ihid->cmd_lock);

	*(__le16 *)ihid->cmdbuf = reg;

	return i2c_hid_xfer(ihid, ihid->cmdbuf, sizeof(__le16), buf, len);
}

static size_t i2c_hid_encode_command(u8 *buf, u8 opcode,
				     int report_type, int report_id)
{
	size_t length = 0;

	if (report_id < 0x0F) {
		buf[length++] = report_type << 4 | report_id;
		buf[length++] = opcode;
	} else {
		buf[length++] = report_type << 4 | 0x0F;
		buf[length++] = opcode;
		buf[length++] = report_id;
	}

	return length;
}

static int i2c_hid_get_report(struct i2c_hid *ihid,
			      u8 report_type, u8 report_id,
			      u8 *recv_buf, size_t recv_len)
{
	size_t length = 0;
	size_t ret_count;
	int error;

	i2c_hid_dbg(ihid, "%s\n", __func__);

	guard(mutex)(&ihid->cmd_lock);

	/* Command register goes first */
	*(__le16 *)ihid->cmdbuf = ihid->hdesc.wCommandRegister;
	length += sizeof(__le16);
	/* Next is GET_REPORT command */
	length += i2c_hid_encode_command(ihid->cmdbuf + length,
					 I2C_HID_OPCODE_GET_REPORT,
					 report_type, report_id);
	/*
	 * Device will send report data through data register. Because
	 * command can be either 2 or 3 bytes destination for the data
	 * register may be not aligned.
	 */
	put_unaligned_le16(le16_to_cpu(ihid->hdesc.wDataRegister),
			   ihid->cmdbuf + length);
	length += sizeof(__le16);

	/*
	 * In addition to report data device will supply data length
	 * in the first 2 bytes of the response, so adjust the read length.
	 */
	recv_len = min(recv_len, ihid->bufsize - sizeof(__le16));
	error = i2c_hid_xfer(ihid, ihid->cmdbuf, length,
			     ihid->rawbuf, recv_len + sizeof(__le16));
	if (error) {
		dev_err(&ihid->client->dev,
			"failed to get a report from device: %d\n", error);
		return error;
	}

	/* The buffer is sufficiently aligned */
	ret_count = le16_to_cpup((__le16 *)ihid->rawbuf);

	/* Check for empty report response */
	if (ret_count <= sizeof(__le16))
		return 0;

	recv_len = min(recv_len, ret_count - sizeof(__le16));
	memcpy(recv_buf, ihid->rawbuf + sizeof(__le16), recv_len);

	if (report_id && recv_len != 0 && recv_buf[0] != report_id) {
		dev_err(&ihid->client->dev,
			"device returned incorrect report (%d vs %d expected)\n",
			recv_buf[0], report_id);
		return -EINVAL;
	}

	return recv_len;
}

static size_t i2c_hid_format_report(u8 *buf, int report_id,
				    const u8 *data, size_t size)
{
	size_t length = sizeof(__le16); /* reserve space to store size */

	if (report_id)
		buf[length++] = report_id;

	memcpy(buf + length, data, size);
	length += size;

	/* Store overall size in the beginning of the buffer */
	put_unaligned_le16(length, buf);

	return length;
}

/**
 * i2c_hid_set_or_send_report: forward an incoming report to the device
 * @ihid: the i2c hid device
 * @report_type: 0x03 for HID_FEATURE_REPORT ; 0x02 for HID_OUTPUT_REPORT
 * @report_id: the report ID
 * @buf: the actual data to transfer, without the report ID
 * @data_len: size of buf
 * @do_set: true: use SET_REPORT HID command, false: send plain OUTPUT report
 */
static int i2c_hid_set_or_send_report(struct i2c_hid *ihid,
				      u8 report_type, u8 report_id,
				      const u8 *buf, size_t data_len,
				      bool do_set)
{
	size_t length = 0;
	int error;

	i2c_hid_dbg(ihid, "%s\n", __func__);

	if (data_len > ihid->bufsize)
		return -EINVAL;

	if (!do_set && le16_to_cpu(ihid->hdesc.wMaxOutputLength) == 0)
		return -ENOSYS;

	guard(mutex)(&ihid->cmd_lock);

	if (do_set) {
		/* Command register goes first */
		*(__le16 *)ihid->cmdbuf = ihid->hdesc.wCommandRegister;
		length += sizeof(__le16);
		/* Next is SET_REPORT command */
		length += i2c_hid_encode_command(ihid->cmdbuf + length,
						 I2C_HID_OPCODE_SET_REPORT,
						 report_type, report_id);
		/*
		 * Report data will go into the data register. Because
		 * command can be either 2 or 3 bytes destination for
		 * the data register may be not aligned.
		*/
		put_unaligned_le16(le16_to_cpu(ihid->hdesc.wDataRegister),
				   ihid->cmdbuf + length);
		length += sizeof(__le16);
	} else {
		/*
		 * With simple "send report" all data goes into the output
		 * register.
		 */
		*(__le16 *)ihid->cmdbuf = ihid->hdesc.wOutputRegister;
		length += sizeof(__le16);
	}

	length += i2c_hid_format_report(ihid->cmdbuf + length,
					report_id, buf, data_len);

	error = i2c_hid_xfer(ihid, ihid->cmdbuf, length, NULL, 0);
	if (error) {
		dev_err(&ihid->client->dev,
			"failed to set a report to device: %d\n", error);
		return error;
	}

	return data_len;
}

static int i2c_hid_set_power_command(struct i2c_hid *ihid, int power_state)
{
	size_t length;

	guard(mutex)(&ihid->cmd_lock);

	/* SET_POWER uses command register */
	*(__le16 *)ihid->cmdbuf = ihid->hdesc.wCommandRegister;
	length = sizeof(__le16);

	/* Now the command itself */
	length += i2c_hid_encode_command(ihid->cmdbuf + length,
					 I2C_HID_OPCODE_SET_POWER,
					 0, power_state);

	return i2c_hid_xfer(ihid, ihid->cmdbuf, length, NULL, 0);
}

static int i2c_hid_set_power(struct i2c_hid *ihid, int power_state)
{
	int ret;

	i2c_hid_dbg(ihid, "%s\n", __func__);

	/*
	 * Some STM-based devices need 400µs after a rising clock edge to wake
	 * from deep sleep, in which case the first request will fail due to
	 * the address not being acknowledged. Try after a short sleep to see
	 * if the device came alive on the bus. Certain Weida Tech devices also
	 * need this.
	 */
	ret = i2c_hid_set_power_command(ihid, power_state);
	if (ret && power_state == I2C_HID_PWR_ON) {
		usleep_range(400, 500);
		ret = i2c_hid_set_power_command(ihid, I2C_HID_PWR_ON);
	}

	if (ret)
		dev_err(&ihid->client->dev,
			"failed to change power setting.\n");

	/*
	 * The HID over I2C specification states that if a DEVICE needs time
	 * after the PWR_ON request, it should utilise CLOCK stretching.
	 * However, it has been observed that the Windows driver provides a
	 * 1ms sleep between the PWR_ON and RESET requests.
	 * According to Goodix Windows even waits 60 ms after (other?)
	 * PWR_ON requests. Testing has confirmed that several devices
	 * will not work properly without a delay after a PWR_ON request.
	 */
	if (!ret && power_state == I2C_HID_PWR_ON)
		msleep(60);

	return ret;
}

static int i2c_hid_start_hwreset(struct i2c_hid *ihid)
{
	size_t length = 0;
	int ret;

	i2c_hid_dbg(ihid, "%s\n", __func__);

	/*
	 * This prevents sending feature reports while the device is
	 * being reset. Otherwise we may lose the reset complete
	 * interrupt.
	 */
	lockdep_assert_held(&ihid->reset_lock);

	ret = i2c_hid_set_power(ihid, I2C_HID_PWR_ON);
	if (ret)
		return ret;

	scoped_guard(mutex, &ihid->cmd_lock) {
		/* Prepare reset command. Command register goes first. */
		*(__le16 *)ihid->cmdbuf = ihid->hdesc.wCommandRegister;
		length += sizeof(__le16);
		/* Next is RESET command itself */
		length += i2c_hid_encode_command(ihid->cmdbuf + length,
						 I2C_HID_OPCODE_RESET, 0, 0);

		set_bit(I2C_HID_RESET_PENDING, &ihid->flags);

		ret = i2c_hid_xfer(ihid, ihid->cmdbuf, length, NULL, 0);
		if (ret) {
			dev_err(&ihid->client->dev,
				"failed to reset device: %d\n", ret);
			break;
		}

		return 0;
	}

	/* Clean up if sending reset command failed */
	clear_bit(I2C_HID_RESET_PENDING, &ihid->flags);
	i2c_hid_set_power(ihid, I2C_HID_PWR_SLEEP);
	return ret;
}

static int i2c_hid_finish_hwreset(struct i2c_hid *ihid)
{
	int ret = 0;

	i2c_hid_dbg(ihid, "%s: waiting...\n", __func__);

	if (!wait_event_timeout(ihid->wait,
				!test_bit(I2C_HID_RESET_PENDING, &ihid->flags),
				msecs_to_jiffies(1000))) {
		dev_warn(&ihid->client->dev, "device did not ack reset within 1000 ms\n");
		clear_bit(I2C_HID_RESET_PENDING, &ihid->flags);
	}
	i2c_hid_dbg(ihid, "%s: finished.\n", __func__);

	ret = i2c_hid_set_power(ihid, I2C_HID_PWR_ON);

	return ret;
}

/*
 * Synaptics SYNA2BA6 resync (I2C_HID_QUIRK_SYNA_RESYNC)
 *
 * The resync re-sends SET_POWER(ON) and then runs the HID driver's
 * reset_resume() hook, which for hid-multitouch releases all contacts and
 * re-sends the Input Mode / Surface & Button switch feature reports. It is
 * the same sequence as i2c_hid_core_resume(), run outside the PM callbacks.
 *
 * Triggers (all funnel into i2c_hid_queue_syna_resync()):
 *  - idle:   first input report after syna_resync_after_idle_ms of silence
 *  - resume: PM .complete phase, if syna_resync_after_resume is set
 *  - manual: writing Y to the syna_resync_now parameter
 */
static LIST_HEAD(syna_resync_devices);
static DEFINE_MUTEX(syna_resync_devices_lock);

static unsigned int syna_resync_after_idle_ms = 30000;
module_param(syna_resync_after_idle_ms, uint, 0644);
MODULE_PARM_DESC(syna_resync_after_idle_ms,
		 "Queue a SYNA2BA6 resync on first input after this idle time in ms, 0 disables");

static bool syna_resync_after_resume = true;
module_param(syna_resync_after_resume, bool, 0644);
MODULE_PARM_DESC(syna_resync_after_resume,
		 "Queue a SYNA2BA6 resync after system resume");

static bool syna_resync_now;

static void i2c_hid_syna_resync_work(struct work_struct *work)
{
	struct i2c_hid *ihid = container_of(work, struct i2c_hid,
					    syna_resync_work);
	struct device *dev = &ihid->client->dev;
	int ret;

	dev_info(dev, "resyncing Synaptics device\n");

	mutex_lock(&ihid->reset_lock);
	ret = i2c_hid_set_power(ihid, I2C_HID_PWR_ON);
	mutex_unlock(&ihid->reset_lock);

	if (ret) {
		dev_warn(dev, "Synaptics power on failed: %d\n", ret);
		return;
	}

	ret = hid_driver_reset_resume(ihid->hid);
	if (ret)
		dev_warn(dev, "Synaptics reset-resume failed: %d\n", ret);
}

static void i2c_hid_queue_syna_resync(struct i2c_hid *ihid)
{
	if (!(ihid->quirks & I2C_HID_QUIRK_SYNA_RESYNC))
		return;

	schedule_work(&ihid->syna_resync_work);
}

static void i2c_hid_syna_resume_resync(struct i2c_hid *ihid)
{
	if (!syna_resync_after_resume)
		return;

	if (!(ihid->quirks & I2C_HID_QUIRK_SYNA_RESYNC))
		return;

	dev_info(&ihid->client->dev,
		 "system resume complete, queuing Synaptics resync\n");
	i2c_hid_queue_syna_resync(ihid);
}

/* Called for every input report; updates the idle timestamp. */
static void i2c_hid_syna_idle_resync(struct i2c_hid *ihid)
{
	unsigned long idle_jiffies;
	unsigned long now = jiffies;
	unsigned int actual_idle_ms;

	if (!(ihid->quirks & I2C_HID_QUIRK_SYNA_RESYNC))
		goto update;

	if (!syna_resync_after_idle_ms)
		goto update;

	idle_jiffies = msecs_to_jiffies(syna_resync_after_idle_ms);
	if (time_after(now, ihid->last_input_jiffies + idle_jiffies)) {
		actual_idle_ms = jiffies_to_msecs(now - ihid->last_input_jiffies);
		dev_info(&ihid->client->dev,
			 "input after %u ms idle, threshold %u ms, queuing Synaptics resync\n",
			 actual_idle_ms, syna_resync_after_idle_ms);
		i2c_hid_queue_syna_resync(ihid);
	}

update:
	ihid->last_input_jiffies = now;
}

static int param_set_syna_resync_now(const char *val,
				     const struct kernel_param *kp)
{
	struct i2c_hid *ihid;
	bool trigger;
	int ret;

	ret = kstrtobool(val, &trigger);
	if (ret)
		return ret;

	if (!trigger)
		return 0;

	mutex_lock(&syna_resync_devices_lock);
	list_for_each_entry(ihid, &syna_resync_devices, syna_resync_node) {
		dev_info(&ihid->client->dev,
			 "manual Synaptics resync requested\n");
		i2c_hid_queue_syna_resync(ihid);
	}
	mutex_unlock(&syna_resync_devices_lock);

	return 0;
}

static const struct kernel_param_ops param_ops_syna_resync_now = {
	.set = param_set_syna_resync_now,
	.get = param_get_bool,
};

module_param_cb(syna_resync_now, &param_ops_syna_resync_now,
		&syna_resync_now, 0200);
MODULE_PARM_DESC(syna_resync_now,
		 "Write Y/1/true to manually queue a SYNA2BA6 resync");

static void i2c_hid_get_input(struct i2c_hid *ihid)
{
	u16 size = le16_to_cpu(ihid->hdesc.wMaxInputLength);
	u16 ret_size;
	int ret;

	if (size > ihid->bufsize)
		size = ihid->bufsize;

	ret = i2c_master_recv(ihid->client, ihid->inbuf, size);
	if (ret != size) {
		if (ret < 0)
			return;

		dev_err(&ihid->client->dev, "%s: got %d data instead of %d\n",
			__func__, ret, size);
		return;
	}

	/* Receiving buffer is properly aligned */
	ret_size = le16_to_cpup((__le16 *)ihid->inbuf);
	if (!ret_size) {
		/* host or device initiated RESET completed */
		if (test_and_clear_bit(I2C_HID_RESET_PENDING, &ihid->flags))
			wake_up(&ihid->wait);
		return;
	}

	if (ret_size > size || ret_size < sizeof(__le16)) {
		dev_err(&ihid->client->dev,
			"%s: incomplete report (%d/%d)\n",
			__func__, size, ret_size);
		return;
	}

	i2c_hid_dbg(ihid, "input: %*ph\n", ret_size, ihid->inbuf);

	if (test_bit(I2C_HID_STARTED, &ihid->flags)) {
		if (ihid->hid->group != HID_GROUP_RMI)
			pm_wakeup_event(&ihid->client->dev, 0);

		hid_safe_input_report(ihid->hid, HID_INPUT_REPORT,
				      ihid->inbuf + sizeof(__le16),
				      ihid->bufsize - sizeof(__le16),
				      ret_size - sizeof(__le16), 1);
		i2c_hid_syna_idle_resync(ihid);
	}

	return;
}

static irqreturn_t i2c_hid_irq(int irq, void *dev_id)
{
	struct i2c_hid *ihid = dev_id;

	i2c_hid_get_input(ihid);

	return IRQ_HANDLED;
}

static int i2c_hid_get_report_length(struct hid_report *report)
{
	return ((report->size - 1) >> 3) + 1 +
		report->device->report_enum[report->type].numbered + 2;
}

/*
 * Traverse the supplied list of reports and find the longest
 */
static void i2c_hid_find_max_report(struct hid_device *hid, unsigned int type,
		unsigned int *max)
{
	struct hid_report *report;
	unsigned int size;

	/* We should not rely on wMaxInputLength, as some devices may set it to
	 * a wrong length. */
	list_for_each_entry(report, &hid->report_enum[type].report_list, list) {
		size = i2c_hid_get_report_length(report);
		if (*max < size)
			*max = size;
	}
}

static void i2c_hid_free_buffers(struct i2c_hid *ihid)
{
	kfree(ihid->inbuf);
	kfree(ihid->rawbuf);
	kfree(ihid->cmdbuf);
	ihid->inbuf = NULL;
	ihid->rawbuf = NULL;
	ihid->cmdbuf = NULL;
	ihid->bufsize = 0;
}

static int i2c_hid_alloc_buffers(struct i2c_hid *ihid, size_t report_size)
{
	/*
	 * The worst case is computed from the set_report command with a
	 * reportID > 15 and the maximum report length.
	 */
	int cmd_len = sizeof(__le16) +	/* command register */
		      sizeof(u8) +	/* encoded report type/ID */
		      sizeof(u8) +	/* opcode */
		      sizeof(u8) +	/* optional 3rd byte report ID */
		      sizeof(__le16) +	/* data register */
		      sizeof(__le16) +	/* report data size */
		      sizeof(u8) +	/* report ID if numbered report */
		      report_size;

	ihid->inbuf = kzalloc(report_size, GFP_KERNEL);
	ihid->rawbuf = kzalloc(report_size, GFP_KERNEL);
	ihid->cmdbuf = kzalloc(cmd_len, GFP_KERNEL);

	if (!ihid->inbuf || !ihid->rawbuf || !ihid->cmdbuf) {
		i2c_hid_free_buffers(ihid);
		return -ENOMEM;
	}

	ihid->bufsize = report_size;

	return 0;
}

static int i2c_hid_get_raw_report(struct hid_device *hid,
				  u8 report_type, u8 report_id,
				  u8 *buf, size_t count)
{
	struct i2c_client *client = hid->driver_data;
	struct i2c_hid *ihid = i2c_get_clientdata(client);
	int ret_count;

	if (report_type == HID_OUTPUT_REPORT)
		return -EINVAL;

	/*
	 * In case of unnumbered reports the response from the device will
	 * not have the report ID that the upper layers expect, so we need
	 * to stash it the buffer ourselves and adjust the data size.
	 */
	if (!report_id) {
		buf[0] = 0;
		buf++;
		count--;
	}

	ret_count = i2c_hid_get_report(ihid,
			report_type == HID_FEATURE_REPORT ? 0x03 : 0x01,
			report_id, buf, count);

	if (ret_count > 0 && !report_id)
		ret_count++;

	return ret_count;
}

static int i2c_hid_output_raw_report(struct hid_device *hid, u8 report_type,
				     const u8 *buf, size_t count, bool do_set)
{
	struct i2c_client *client = hid->driver_data;
	struct i2c_hid *ihid = i2c_get_clientdata(client);
	int report_id = buf[0];
	int ret;

	if (report_type == HID_INPUT_REPORT)
		return -EINVAL;

	mutex_lock(&ihid->reset_lock);

	/*
	 * Note that both numbered and unnumbered reports passed here
	 * are supposed to have report ID stored in the 1st byte of the
	 * buffer, so we strip it off unconditionally before passing payload
	 * to i2c_hid_set_or_send_report which takes care of encoding
	 * everything properly.
	 */
	ret = i2c_hid_set_or_send_report(ihid,
				report_type == HID_FEATURE_REPORT ? 0x03 : 0x02,
				report_id, buf + 1, count - 1, do_set);

	if (ret >= 0)
		ret++; /* add report_id to the number of transferred bytes */

	mutex_unlock(&ihid->reset_lock);

	return ret;
}

static int i2c_hid_output_report(struct hid_device *hid, u8 *buf, size_t count)
{
	return i2c_hid_output_raw_report(hid, HID_OUTPUT_REPORT, buf, count,
					 false);
}

static int i2c_hid_raw_request(struct hid_device *hid, unsigned char reportnum,
			       __u8 *buf, size_t len, unsigned char rtype,
			       int reqtype)
{
	switch (reqtype) {
	case HID_REQ_GET_REPORT:
		return i2c_hid_get_raw_report(hid, rtype, reportnum, buf, len);
	case HID_REQ_SET_REPORT:
		if (buf[0] != reportnum)
			return -EINVAL;
		return i2c_hid_output_raw_report(hid, rtype, buf, len, true);
	default:
		return -EIO;
	}
}

static int i2c_hid_parse(struct hid_device *hid)
{
	struct i2c_client *client = hid->driver_data;
	struct i2c_hid *ihid = i2c_get_clientdata(client);
	struct i2c_hid_desc *hdesc = &ihid->hdesc;
	char *rdesc;
	unsigned int rsize;
	int ret;
	int tries = 3;

	i2c_hid_dbg(ihid, "entering %s\n", __func__);

	rsize = le16_to_cpu(hdesc->wReportDescLength);
	if (!rsize || rsize > HID_MAX_DESCRIPTOR_SIZE) {
		dbg_hid("weird size of report descriptor (%u)\n", rsize);
		return -EINVAL;
	}

	mutex_lock(&ihid->reset_lock);
	do {
		ret = i2c_hid_start_hwreset(ihid);
		if (ret == 0)
			ret = i2c_hid_finish_hwreset(ihid);
		else
			msleep(1000);
	} while (tries-- > 0 && ret);
	mutex_unlock(&ihid->reset_lock);

	if (ret)
		return ret;

	rdesc = kzalloc(rsize, GFP_KERNEL);
	if (!rdesc)
		return -ENOMEM;

	i2c_hid_dbg(ihid, "asking HID report descriptor\n");

	ret = i2c_hid_read_register(ihid,
				    ihid->hdesc.wReportDescRegister,
				    rdesc, rsize);
	if (ret) {
		hid_err(hid, "reading report descriptor failed\n");
		goto out;
	}

	i2c_hid_dbg(ihid, "Report Descriptor: %*ph\n", rsize, rdesc);

	ret = hid_parse_report(hid, rdesc, rsize);
	if (ret)
		dbg_hid("parsing report descriptor failed\n");

out:
	kfree(rdesc);

	return ret;
}

static int i2c_hid_start(struct hid_device *hid)
{
	struct i2c_client *client = hid->driver_data;
	struct i2c_hid *ihid = i2c_get_clientdata(client);
	int ret;
	unsigned int bufsize = HID_MIN_BUFFER_SIZE;

	i2c_hid_find_max_report(hid, HID_INPUT_REPORT, &bufsize);
	i2c_hid_find_max_report(hid, HID_OUTPUT_REPORT, &bufsize);
	i2c_hid_find_max_report(hid, HID_FEATURE_REPORT, &bufsize);

	if (bufsize > ihid->bufsize) {
		disable_irq(client->irq);
		i2c_hid_free_buffers(ihid);

		ret = i2c_hid_alloc_buffers(ihid, bufsize);
		enable_irq(client->irq);

		if (ret)
			return ret;
	}

	return 0;
}

static void i2c_hid_stop(struct hid_device *hid)
{
	hid->claimed = 0;
}

static int i2c_hid_open(struct hid_device *hid)
{
	struct i2c_client *client = hid->driver_data;
	struct i2c_hid *ihid = i2c_get_clientdata(client);

	set_bit(I2C_HID_STARTED, &ihid->flags);
	return 0;
}

static void i2c_hid_close(struct hid_device *hid)
{
	struct i2c_client *client = hid->driver_data;
	struct i2c_hid *ihid = i2c_get_clientdata(client);

	clear_bit(I2C_HID_STARTED, &ihid->flags);
}

static const struct hid_ll_driver i2c_hid_ll_driver = {
	.parse = i2c_hid_parse,
	.start = i2c_hid_start,
	.stop = i2c_hid_stop,
	.open = i2c_hid_open,
	.close = i2c_hid_close,
	.output_report = i2c_hid_output_report,
	.raw_request = i2c_hid_raw_request,
};

static int i2c_hid_init_irq(struct i2c_client *client)
{
	struct i2c_hid *ihid = i2c_get_clientdata(client);
	unsigned long irqflags = 0;
	int ret;

	i2c_hid_dbg(ihid, "Requesting IRQ: %d\n", client->irq);

	if (!irq_get_trigger_type(client->irq))
		irqflags = IRQF_TRIGGER_LOW;

	ret = request_threaded_irq(client->irq, NULL, i2c_hid_irq,
				   irqflags | IRQF_ONESHOT | IRQF_NO_AUTOEN,
				   client->name, ihid);
	if (ret < 0) {
		dev_warn(&client->dev,
			"Could not register for %s interrupt, irq = %d,"
			" ret = %d\n",
			client->name, client->irq, ret);

		return ret;
	}

	return 0;
}

static int i2c_hid_fetch_hid_descriptor(struct i2c_hid *ihid)
{
	struct i2c_hid_desc *hdesc = &ihid->hdesc;
	unsigned int dsize;
	int error;

	/* i2c hid fetch using a fixed descriptor size (30 bytes) */
	i2c_hid_dbg(ihid, "Fetching the HID descriptor\n");
	error = i2c_hid_read_register(ihid,
				      ihid->wHIDDescRegister,
				      &ihid->hdesc,
				      sizeof(ihid->hdesc));
	if (error) {
		dev_err(&ihid->client->dev,
			"failed to fetch HID descriptor: %d\n",
			error);
		return -ENODEV;
	}

	/* Validate the length of HID descriptor, the 4 first bytes:
	 * bytes 0-1 -> length
	 * bytes 2-3 -> bcdVersion (has to be 1.00) */
	/* check bcdVersion == 1.0 */
	if (le16_to_cpu(hdesc->bcdVersion) != 0x0100) {
		dev_err(&ihid->client->dev,
			"unexpected HID descriptor bcdVersion (0x%04hx)\n",
			le16_to_cpu(hdesc->bcdVersion));
		return -ENODEV;
	}

	/* Descriptor length should be 30 bytes as per the specification */
	dsize = le16_to_cpu(hdesc->wHIDDescLength);
	if (dsize != sizeof(struct i2c_hid_desc)) {
		dev_err(&ihid->client->dev,
			"weird size of HID descriptor (%u)\n", dsize);
		return -ENODEV;
	}
	i2c_hid_dbg(ihid, "HID Descriptor: %*ph\n", dsize, &ihid->hdesc);
	return 0;
}

static void i2c_hid_core_shutdown_tail(struct i2c_hid *ihid)
{
	if (!ihid->ops->shutdown_tail)
		return;

	ihid->ops->shutdown_tail(ihid->ops);
}

static void i2c_hid_core_restore_sequence(struct i2c_hid *ihid)
{
	if (!ihid->ops->restore_sequence)
		return;

	ihid->ops->restore_sequence(ihid->ops);
}

static int i2c_hid_core_suspend(struct i2c_hid *ihid)
{
	struct i2c_client *client = ihid->client;
	struct hid_device *hid = ihid->hid;
	int ret;

	cancel_work_sync(&ihid->syna_resync_work);

	ret = hid_driver_suspend(hid, PMSG_SUSPEND);
	if (ret < 0)
		return ret;

	i2c_hid_set_power(ihid, I2C_HID_PWR_SLEEP);

	disable_irq(client->irq);

	return 0;
}

static int i2c_hid_core_resume(struct i2c_hid *ihid)
{
	struct hid_device *hid = ihid->hid;
	int ret;

	enable_irq(ihid->client->irq);

	ret = i2c_hid_set_power(ihid, I2C_HID_PWR_ON);
	if (ret)
		return ret;

	ret = hid_driver_reset_resume(hid);
	if (ret)
		return ret;

	return 0;
}

/*
 * Check that the device exists and parse the HID descriptor.
 */
static int __i2c_hid_core_probe(struct i2c_hid *ihid)
{
	struct i2c_client *client = ihid->client;
	struct hid_device *hid = ihid->hid;
	int ret;

	ret = i2c_hid_probe_address(ihid);
	if (ret < 0) {
		i2c_hid_dbg(ihid, "nothing at this address: %d\n", ret);
		return -ENXIO;
	}

	ret = i2c_hid_fetch_hid_descriptor(ihid);
	if (ret < 0) {
		dev_err(&client->dev,
			"Failed to fetch the HID Descriptor\n");
		return ret;
	}

	hid->version = le16_to_cpu(ihid->hdesc.bcdVersion);
	hid->vendor = le16_to_cpu(ihid->hdesc.wVendorID);
	hid->product = le16_to_cpu(ihid->hdesc.wProductID);

	snprintf(hid->name, sizeof(hid->name), "%s %04X:%04X",
		 client->name, (u16)hid->vendor, (u16)hid->product);
	strscpy(hid->phys, dev_name(&client->dev), sizeof(hid->phys));

	ihid->quirks = i2c_hid_lookup_quirk(hid->vendor, hid->product);

	return 0;
}

static int i2c_hid_core_register_hid(struct i2c_hid *ihid)
{
	struct i2c_client *client = ihid->client;
	struct hid_device *hid = ihid->hid;
	int ret;

	enable_irq(client->irq);

	ret = hid_add_device(hid);
	if (ret) {
		if (ret != -ENODEV)
			hid_err(client, "can't add hid device: %d\n", ret);
		disable_irq(client->irq);
		return ret;
	}

	return 0;
}

int i2c_hid_core_probe(struct i2c_client *client, struct i2chid_ops *ops,
		       u16 hid_descriptor_address)
{
	int ret;
	struct i2c_hid *ihid;
	struct hid_device *hid;

	dbg_hid("HID probe called for i2c 0x%02x\n", client->addr);

	if (!client->irq) {
		dev_err(&client->dev,
			"HID over i2c has not been provided an Int IRQ\n");
		return -EINVAL;
	}

	if (client->irq < 0) {
		if (client->irq != -EPROBE_DEFER)
			dev_err(&client->dev,
				"HID over i2c doesn't have a valid IRQ\n");
		return client->irq;
	}

	ihid = devm_kzalloc(&client->dev, sizeof(*ihid), GFP_KERNEL);
	if (!ihid)
		return -ENOMEM;

	i2c_set_clientdata(client, ihid);

	ihid->ops = ops;
	ihid->client = client;
	ihid->wHIDDescRegister = cpu_to_le16(hid_descriptor_address);
	ihid->last_input_jiffies = jiffies;

	init_waitqueue_head(&ihid->wait);
	mutex_init(&ihid->cmd_lock);
	mutex_init(&ihid->reset_lock);
	INIT_WORK(&ihid->syna_resync_work, i2c_hid_syna_resync_work);
	INIT_LIST_HEAD(&ihid->syna_resync_node);

	/* we need to allocate the command buffer without knowing the maximum
	 * size of the reports. Let's use HID_MIN_BUFFER_SIZE, then we do the
	 * real computation later. */
	ret = i2c_hid_alloc_buffers(ihid, HID_MIN_BUFFER_SIZE);
	if (ret < 0)
		return ret;
	device_enable_async_suspend(&client->dev);

	hid = hid_allocate_device();
	if (IS_ERR(hid)) {
		ret = PTR_ERR(hid);
		goto err_free_buffers;
	}

	ihid->hid = hid;

	hid->driver_data = client;
	hid->ll_driver = &i2c_hid_ll_driver;
	hid->dev.parent = &client->dev;
	hid->bus = BUS_I2C;

	ret = __i2c_hid_core_probe(ihid);
	if (ret < 0)
		goto err_destroy_device;

	ret = i2c_hid_init_irq(client);
	if (ret < 0)
		goto err_destroy_device;

	ret = i2c_hid_core_register_hid(ihid);
	if (ret)
		goto err_free_irq;

	if (ihid->quirks & I2C_HID_QUIRK_SYNA_RESYNC) {
		mutex_lock(&syna_resync_devices_lock);
		list_add_tail(&ihid->syna_resync_node, &syna_resync_devices);
		mutex_unlock(&syna_resync_devices_lock);
	}

	return 0;

err_free_irq:
	free_irq(client->irq, ihid);
err_destroy_device:
	hid_destroy_device(hid);
err_free_buffers:
	i2c_hid_free_buffers(ihid);

	return ret;
}
EXPORT_SYMBOL_GPL(i2c_hid_core_probe);

void i2c_hid_core_remove(struct i2c_client *client)
{
	struct i2c_hid *ihid = i2c_get_clientdata(client);
	struct hid_device *hid;

	if (ihid->quirks & I2C_HID_QUIRK_SYNA_RESYNC) {
		mutex_lock(&syna_resync_devices_lock);
		list_del_init(&ihid->syna_resync_node);
		mutex_unlock(&syna_resync_devices_lock);
	}

	i2c_hid_core_suspend(ihid);

	hid = ihid->hid;
	hid_destroy_device(hid);

	free_irq(client->irq, ihid);

	if (ihid->bufsize)
		i2c_hid_free_buffers(ihid);
}
EXPORT_SYMBOL_GPL(i2c_hid_core_remove);

void i2c_hid_core_shutdown(struct i2c_client *client)
{
	struct i2c_hid *ihid = i2c_get_clientdata(client);

	i2c_hid_set_power(ihid, I2C_HID_PWR_SLEEP);
	free_irq(client->irq, ihid);

	i2c_hid_core_shutdown_tail(ihid);
}
EXPORT_SYMBOL_GPL(i2c_hid_core_shutdown);

static int i2c_hid_core_pm_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct i2c_hid *ihid = i2c_get_clientdata(client);

	return i2c_hid_core_suspend(ihid);
}

static int i2c_hid_core_pm_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct i2c_hid *ihid = i2c_get_clientdata(client);

	return i2c_hid_core_resume(ihid);
}

static int i2c_hid_core_pm_restore(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct i2c_hid *ihid = i2c_get_clientdata(client);

	i2c_hid_core_restore_sequence(ihid);

	return i2c_hid_core_resume(ihid);
}

static void i2c_hid_core_pm_complete(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct i2c_hid *ihid = i2c_get_clientdata(client);

	i2c_hid_syna_resume_resync(ihid);
}

const struct dev_pm_ops i2c_hid_core_pm = {
	.suspend = pm_sleep_ptr(i2c_hid_core_pm_suspend),
	.resume = pm_sleep_ptr(i2c_hid_core_pm_resume),
	.freeze = pm_sleep_ptr(i2c_hid_core_pm_suspend),
	.thaw = pm_sleep_ptr(i2c_hid_core_pm_resume),
	.poweroff = pm_sleep_ptr(i2c_hid_core_pm_suspend),
	.restore = pm_sleep_ptr(i2c_hid_core_pm_restore),
	.complete = pm_sleep_ptr(i2c_hid_core_pm_complete),
};
EXPORT_SYMBOL_GPL(i2c_hid_core_pm);

MODULE_DESCRIPTION("HID over I2C core driver");
MODULE_AUTHOR("Benjamin Tissoires <benjamin.tissoires@gmail.com>");
MODULE_LICENSE("GPL");
