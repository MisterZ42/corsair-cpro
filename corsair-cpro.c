// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * corsair-cpro.c - Linux driver for Corsair Commander Pro
 * Copyright (C) 2020 Marius Zachmann <mail@mariuszachmann.de>
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>

#define	hid_to_usb_dev(hid_dev) \
	to_usb_device(hid_dev->dev.parent->parent)
#define USB_VENDOR_ID_CORSAIR               0x1b1c
#define USB_PRODUCT_ID_CORSAIR_COMMANDERPRO 0x0c10
#define USB_PRODUCT_ID_CORSAIR_1000D	    0x1d00

#define OUT_BUFFER_SIZE	63
#define IN_BUFFER_SIZE	16
#define LABEL_LENGTH	11

#define CTL_GET_TMP	 0x11  /* byte 1 is channel, rest zero              */
			       /* returns temp for channel in bytes 1 and 2 */
#define CTL_GET_VOLT	 0x12  /* byte 1 = rail number 12, 5, 3.3 */
			       /* returns volt in bytes 1,2       */
#define CTL_GET_FAN_CNCT 0x20  /* returns in bytes 1-6   */
			       /* 0 for no connect       */
			       /* 1 for 3pin, 2 for 4pin */
#define CTL_GET_FAN_RPM	 0x21  /* works like CTL_GET_TMP */
#define CTL_SET_FAN_FPWM 0x23  /* byte 1 is fan number              */
			       /* byte 2 is percentage from 0 - 100 */

struct ccp_device {
	struct hid_device *hdev;
	struct mutex mutex; /* whenever buffer is used and usb calls are made */
	u8 *buffer;
	int pwm[6];
	int fan_enable[6];
	char fan_label[6][LABEL_LENGTH];
};

/* send 63 byte buffer and receive response in same buffer */
static int send_usb_cmd(struct ccp_device *ccp)
{
	int ret;
	struct usb_device *udev = hid_to_usb_dev(ccp->hdev);
	int actual_length;

	ret = usb_bulk_msg(udev, usb_sndintpipe(udev, 2), ccp->buffer, OUT_BUFFER_SIZE,
			   &actual_length, 1000);
	if (ret) {
		hid_err(ccp->hdev, "usb_bulk_msg send failed: %d", ret);
		goto exit;
	}

	ret = usb_bulk_msg(udev, usb_rcvintpipe(udev, 1), ccp->buffer, IN_BUFFER_SIZE,
			   &actual_length, 1000);
	if (ret) {
		hid_err(ccp->hdev, "usb_bulk_msg receive failed: %d", ret);
		goto exit;
	}

exit:
	mutex_unlock(&ccp->mutex);
	return ret;
}

/* for commands, which return just a number depending on a channel: */
/* get_temp, get_volt, get_fan_rpm */
static int get_data(struct ccp_device *ccp, int command, int channel, long *val)
{
	int ret;

	mutex_lock(&ccp->mutex);

	memset(ccp->buffer, 0x00, OUT_BUFFER_SIZE);
	ccp->buffer[0] = command;
	ccp->buffer[1] = channel;
	ret = send_usb_cmd(ccp);
	if (ret)
		goto exit;

	*val = (ccp->buffer[1] << 8) + ccp->buffer[2];

exit:
	mutex_unlock(&ccp->mutex);
	return ret;
}

static int set_pwm(struct ccp_device *ccp, int channel, long val)
{
	int ret;

	if (val < 0 || val > 255)
		return -EINVAL;

	ccp->pwm[channel] = val;

	/* The Corsair Commander Pro uses values from 0-100 */
	val = DIV_ROUND_CLOSEST(val * 100, 255);

	mutex_lock(&ccp->mutex);

	memset(ccp->buffer, 0x00, OUT_BUFFER_SIZE);
	ccp->buffer[0] = CTL_SET_FAN_FPWM;
	ccp->buffer[1] = channel;
	ccp->buffer[2] = val;
	ret = send_usb_cmd(ccp);

	mutex_unlock(&ccp->mutex);
	return ret;
}

static int get_fan_mode_label(struct ccp_device *ccp, int channel)
{
	int ret;
	int mode;

	mutex_lock(&ccp->mutex);

	memset(ccp->buffer, 0x00, OUT_BUFFER_SIZE);
	ccp->buffer[0] = CTL_GET_FAN_CNCT;
	ret = send_usb_cmd(ccp);
	if (ret)
		goto exit;

	mode = ccp->buffer[channel + 1];

	switch (mode) {
	case 0:
		scnprintf(ccp->fan_label[channel], LABEL_LENGTH, "fan%d nc", channel + 1);
		break;
	case 1:
		scnprintf(ccp->fan_label[channel], LABEL_LENGTH, "fan%d 3pin", channel + 1);
		break;
	case 2:
		scnprintf(ccp->fan_label[channel], LABEL_LENGTH, "fan%d 4pin", channel + 1);
		break;
	default:
		scnprintf(ccp->fan_label[channel], LABEL_LENGTH, "fan%d other", channel + 1);
		break;
	}

exit:
	mutex_unlock(&ccp->mutex);
	return ret;
}

static int get_voltages(struct ccp_device *ccp, int channel, long *val)
{
	int ret;

	ret = get_data(ccp, CTL_GET_VOLT, channel, val);

	return ret;
}

static int get_temp(struct ccp_device *ccp, int channel, long *val)
{
	int ret;

	ret = get_data(ccp, CTL_GET_TMP, channel, val);
	*val = *val * 10;

	return ret;
}

static int get_rpm(struct ccp_device *ccp, int channel, long *val)
{
	int ret;

	if (!ccp->fan_enable[channel])
		return -ENODATA;

	ret = get_data(ccp, CTL_GET_FAN_RPM, channel, val);

	return ret;
}

static int ccp_read_string(struct device *dev, enum hwmon_sensor_types type,
			   u32 attr, int channel, const char **str)
{
	int ret;
	struct ccp_device *ccp = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_label:
			ret = get_fan_mode_label(ccp, channel);
			*str = ccp->fan_label[channel];
			break;
		default:
			ret = -EOPNOTSUPP;
			break;
		}
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	return ret;
}

static int ccp_read(struct device *dev, enum hwmon_sensor_types type,
		    u32 attr, int channel, long *val)
{
	int ret = 0;
	struct ccp_device *ccp = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
			ret = get_temp(ccp, channel, val);
			break;
		default:
			ret = -EOPNOTSUPP;
			break;
		}
		break;
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
			ret = get_rpm(ccp, channel, val);
			break;
		case hwmon_fan_enable:
			*val = ccp->fan_enable[channel];
			break;
		default:
			ret = -EOPNOTSUPP;
			break;
		}
		break;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			/* how to read pwm values from the device is unknown */
			/* driver returns last set value or 0		     */
			*val = ccp->pwm[channel];
			break;
		default:
			ret = -EOPNOTSUPP;
			break;
		}
		break;
	case hwmon_in:
		switch (attr) {
		case hwmon_in_input:
			ret = get_voltages(ccp, channel, val);
			break;
		default:
			ret = -EOPNOTSUPP;
			break;
		}
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	return ret;
};

static int ccp_write(struct device *dev, enum hwmon_sensor_types type,
		     u32 attr, int channel, long val)
{
	int ret = 0;
	struct ccp_device *ccp = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_enable:
			if (val == 0 || val == 1)
				ccp->fan_enable[channel] = val;
			else
				ret = -EINVAL;
			break;
		default:
			ret = -EOPNOTSUPP;
			break;
		}
		break;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			ret = set_pwm(ccp, channel, val);
			break;
		default:
			ret = -EOPNOTSUPP;
			break;
		}
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	return ret;
};

static umode_t ccp_is_visible(const void *data, enum hwmon_sensor_types type,
			      u32 attr, int channel)
{
	switch (type) {
	case hwmon_chip:
		switch (attr) {
		case hwmon_chip_update_interval:
			return 0644;
		default:
			break;
		}
		break;
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
			return 0444;
		default:
			break;
		}
		break;
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
			return 0444;
		case hwmon_fan_label:
			return 0444;
		case hwmon_fan_enable:
			return 0644;
		default:
			break;
		}
		break;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			return 0644;
		default:
			break;
		}
		break;
	case hwmon_in:
		switch (attr) {
		case hwmon_in_input:
			return 0444;
		default:
			break;
		}
		break;
	default:
		break;
	}

	return 0;
};

static const struct hwmon_ops ccp_hwmon_ops = {
	.is_visible = ccp_is_visible,
	.read = ccp_read,
	.write = ccp_write,
	.read_string = ccp_read_string,
};

static const struct hwmon_channel_info *ccp_info[] = {
	HWMON_CHANNEL_INFO(chip,
			   HWMON_C_REGISTER_TZ | HWMON_C_UPDATE_INTERVAL),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_MAX,
			   HWMON_T_INPUT | HWMON_T_MAX,
			   HWMON_T_INPUT | HWMON_T_MAX,
			   HWMON_T_INPUT | HWMON_T_MAX
			   ),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_ENABLE | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_ENABLE | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_ENABLE | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_ENABLE | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_ENABLE | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_ENABLE | HWMON_F_LABEL
			   ),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT,
			   HWMON_PWM_INPUT
			   ),
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT,
			   HWMON_I_INPUT
			   ),
	NULL
};

static const struct hwmon_chip_info ccp_chip_info = {
	.ops = &ccp_hwmon_ops,
	.info = ccp_info,

};

static int ccp_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct ccp_device *ccp;
	struct device *hwmon_dev;
	int ret = 0;

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "hid_parse failed\n");
		return ret;
	}

	ccp = devm_kzalloc(&hdev->dev, sizeof(struct ccp_device), GFP_KERNEL);
	if (!ccp)
		return -ENOMEM;

	ccp->buffer = devm_kmalloc(&hdev->dev, OUT_BUFFER_SIZE, GFP_KERNEL);
	if (!ccp->buffer)
		return -ENOMEM;

	mutex_init(&ccp->mutex);

	ccp->fan_enable[0] = 1;
	ccp->fan_enable[1] = 1;
	ccp->fan_enable[2] = 1;
	ccp->fan_enable[3] = 1;
	ccp->fan_enable[4] = 1;
	ccp->fan_enable[5] = 1;
	ccp->hdev = hdev;

	hwmon_dev = devm_hwmon_device_register_with_info(&hdev->dev, "corsaircpro", ccp,
							 &ccp_chip_info, 0);

	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct hid_device_id ccp_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR,
			 USB_PRODUCT_ID_CORSAIR_COMMANDERPRO) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR,
			 USB_PRODUCT_ID_CORSAIR_1000D) },
	{ }
};

static struct hid_driver ccp_driver = {
	.name = "corsair-cpro",
	.id_table = ccp_devices,
	.probe = ccp_probe
};

MODULE_DEVICE_TABLE(hid, ccp_devices);
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: hid");

module_hid_driver(ccp_driver);
