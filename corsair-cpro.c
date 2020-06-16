// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * corsair-cpro.c - Linux driver for Corsair Commander Pro
 * Copyright (C) 2020 Marius Zachmann <mail@mariuszachmann.de>
 */

#include <linux/kernel.h>
#include <linux/hwmon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/usb.h>

#define USB_VENDOR_ID_CORSAIR               0x1b1c
#define USB_PRODUCT_ID_CORSAIR_COMMANDERPRO 0x0c10
#define USB_PRODUCT_ID_CORSAIR_1000D	    0x1d00

#define OUT_BUFFER_SIZE	63
#define IN_BUFFER_SIZE	16
#define LABEL_LENGTH	12

#define CTL_GET_TMP_CNCT 0x10 /*
			       * returns in bytes 1-4 for each temp sensor:
			       * 0 not connected
			       * 1 connected
			       */
#define CTL_GET_TMP	 0x11 /*
			       * send: byte 1 is channel, rest zero
			       * rcv:  returns temp for channel in bytes 1 and 2
			       * returns 17 in byte 0 if no sensor is connected
			       */
#define CTL_GET_VOLT	 0x12 /*
			       * send: byte 1 is rail number: 0 = 12v, 1 = 5v, 2 = 3.3v
			       * rcv:  returns volt in bytes 1,2
			       */
#define CTL_GET_FAN_CNCT 0x20 /*
			       * returns in bytes 1-6 for each fan:
			       * 0 not connected
			       * 1 3pin
			       * 2 4pin
			       */
#define CTL_GET_FAN_RPM	 0x21 /*
			       * send: byte 1 is channel, rest zero
			       * rcv:  returns rpm in bytes 1,2
			       */
#define CTL_SET_FAN_FPWM 0x23 /*
			       * set fixed pwm
			       * send: byte 1 is fan number
			       * send: byte 2 is percentage from 0 - 100
			       */

struct ccp_device {
	struct usb_device *udev;
	struct mutex mutex; /* whenever buffer is used, lock before send_usb_cmd */
	u8 *buffer;
	int pwm[6];
	char fan_label[6][LABEL_LENGTH];
	int temp_cnct[4];
	char temp_label[4][LABEL_LENGTH];
};

/* send command, check for error in response, response in ccp->buffer */
static int send_usb_cmd(struct ccp_device *ccp, u8 command, u8 byte1, u8 byte2)
{
	int actual_length;
	int ret;

	memset(ccp->buffer, 0x00, OUT_BUFFER_SIZE);
	ccp->buffer[0] = command;
	ccp->buffer[1] = byte1;
	ccp->buffer[2] = byte2;

	ret = usb_bulk_msg(ccp->udev, usb_sndintpipe(ccp->udev, 2), ccp->buffer, OUT_BUFFER_SIZE,
			   &actual_length, 1000);
	if (ret) {
		dev_err(&ccp->udev->dev, "usb_bulk_msg send failed: %d", ret);
		return ret;
	}

	/* response needs to be received every time */
	ret = usb_bulk_msg(ccp->udev, usb_rcvintpipe(ccp->udev, 1), ccp->buffer, IN_BUFFER_SIZE,
			   &actual_length, 1000);
	if (ret) {
		dev_err(&ccp->udev->dev, "usb_bulk_msg receive failed: %d", ret);
		return ret;
	}

	/* first byte of response is error code */
	if (ccp->buffer[0] != 0x00) {
		dev_err(&ccp->udev->dev, "device response error: %d", ccp->buffer[0]);
		return -EIO;
	}

	return 0;
}

/* for commands, which return just a number depending on a channel: */
static int get_data(struct ccp_device *ccp, int command, int channel, long *val)
{
	int ret;

	mutex_lock(&ccp->mutex);

	ret = send_usb_cmd(ccp, command, channel, 0);
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

	ret = send_usb_cmd(ccp, CTL_SET_FAN_FPWM, channel, val);

	mutex_unlock(&ccp->mutex);
	return ret;
}

/* read fan connection status and set labels */
static int get_fan_cnct(struct ccp_device *ccp)
{
	int channel;
	int mode;
	int ret;

	mutex_lock(&ccp->mutex);

	ret = send_usb_cmd(ccp, CTL_GET_FAN_CNCT, 0, 0);
	if (ret)
		goto exit;

	for (channel = 0; channel < 6; channel++) {
		mode = ccp->buffer[channel + 1];

		switch (mode) {
		case 0:
			scnprintf(ccp->fan_label[channel], LABEL_LENGTH,
				  "fan%d nc", channel + 1);
			break;
		case 1:
			scnprintf(ccp->fan_label[channel], LABEL_LENGTH,
				  "fan%d 3pin", channel + 1);
			break;
		case 2:
			scnprintf(ccp->fan_label[channel], LABEL_LENGTH,
				  "fan%d 4pin", channel + 1);
			break;
		default:
			scnprintf(ccp->fan_label[channel], LABEL_LENGTH,
				  "fan%d other", channel + 1);
			break;
		}
	}

exit:
	mutex_unlock(&ccp->mutex);
	return ret;
}

/* read temp sensor connection status and set labels */
static int get_temp_cnct(struct ccp_device *ccp)
{
	int channel;
	int mode;
	int ret;

	mutex_lock(&ccp->mutex);

	ret = send_usb_cmd(ccp, CTL_GET_TMP_CNCT, 0, 0);
	if (ret)
		goto exit;

	for (channel = 0; channel < 4; channel++) {
		mode = ccp->buffer[channel + 1];
		ccp->temp_cnct[channel] = mode;

		switch (mode) {
		case 0:
			scnprintf(ccp->temp_label[channel], LABEL_LENGTH,
				  "temp%d nc", channel + 1);
			break;
		case 1:
			scnprintf(ccp->temp_label[channel], LABEL_LENGTH,
				  "temp%d", channel + 1);
			break;
		default:
			scnprintf(ccp->temp_label[channel], LABEL_LENGTH,
				  "temp%d other", channel + 1);
			break;
		}
	}

exit:
	mutex_unlock(&ccp->mutex);
	return ret;
}

static int get_temp(struct ccp_device *ccp, int channel, long *val)
{
	int ret;

	if (ccp->temp_cnct[channel] != 1)
		return -ENODATA;

	ret = get_data(ccp, CTL_GET_TMP, channel, val);
	*val = *val * 10;

	return ret;
}

static int ccp_read_string(struct device *dev, enum hwmon_sensor_types type,
			   u32 attr, int channel, const char **str)
{
	struct ccp_device *ccp = dev_get_drvdata(dev);
	int ret = 0;

	switch (type) {
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_label:
			*str = ccp->fan_label[channel];
			break;
		default:
			ret = -EOPNOTSUPP;
			break;
		}
		break;
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_label:
			*str = ccp->temp_label[channel];
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
	struct ccp_device *ccp = dev_get_drvdata(dev);
	int ret = 0;

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
			ret = get_data(ccp, CTL_GET_FAN_RPM, channel, val);
			break;
		default:
			ret = -EOPNOTSUPP;
			break;
		}
		break;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			/* how to read pwm values from the device is currently unknown */
			/* driver returns last set value or 0		               */
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
			ret = get_data(ccp, CTL_GET_VOLT, channel, val);
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
	struct ccp_device *ccp = dev_get_drvdata(dev);
	int ret = 0;

	switch (type) {
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
		case hwmon_temp_label:
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
	.read_string = ccp_read_string,
	.write = ccp_write,
};

static const struct hwmon_channel_info *ccp_info[] = {
	HWMON_CHANNEL_INFO(chip,
			   HWMON_C_REGISTER_TZ | HWMON_C_UPDATE_INTERVAL),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL
			   ),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL
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

static int ccp_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct device *hwmon_dev;
	struct ccp_device *ccp;
	int ret;

	ccp = devm_kzalloc(&intf->dev, sizeof(struct ccp_device), GFP_KERNEL);
	if (!ccp)
		return -ENOMEM;

	ccp->buffer = devm_kmalloc(&intf->dev, OUT_BUFFER_SIZE, GFP_KERNEL);
	if (!ccp->buffer)
		return -ENOMEM;

	mutex_init(&ccp->mutex);

	ccp->udev = interface_to_usbdev(intf);

	/* temp and fan connection status only updates, when device is powered on */
	ret = get_temp_cnct(ccp);
	if (ret)
		return ret;

	ret = get_fan_cnct(ccp);
	if (ret)
		return ret;

	hwmon_dev = devm_hwmon_device_register_with_info(&intf->dev, "corsaircpro", ccp,
							 &ccp_chip_info, 0);

	return PTR_ERR_OR_ZERO(hwmon_dev);
}

void ccp_disconnect(struct usb_interface *intf)
{
}

static const struct usb_device_id ccp_devices[] = {
	{ USB_DEVICE(USB_VENDOR_ID_CORSAIR, USB_PRODUCT_ID_CORSAIR_COMMANDERPRO) },
	{ USB_DEVICE(USB_VENDOR_ID_CORSAIR, USB_PRODUCT_ID_CORSAIR_1000D) },
	{ }
};

static struct usb_driver ccp_driver = {
	.name = "corsair-cpro",
	.probe = ccp_probe,
	.disconnect = ccp_disconnect,
	.id_table = ccp_devices
};

MODULE_DEVICE_TABLE(usb, ccp_devices);
MODULE_LICENSE("GPL");

module_usb_driver(ccp_driver);
