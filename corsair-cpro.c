#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/hwmon.h>
#include <linux/spinlock.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>

MODULE_LICENSE("GPL v2");

#define USB_VENDOR_ID_CORSAIR    0x1b1c
#define USB_VENDOR_ID_CORSAIR_CP 0x0c10

#define OUT_BUFFER_SIZE 63
#define IN_BUFFER_SIZE 16
#define MAX_BUFFER_SIZE 63

#define CTL_GET_TMP_CFG  0x10  // bytes 1-4 show connection of corresponding sensor
#define CTL_GET_TMP      0x11  // byte 1 should be tmp-sensor, rest zero
                               // returns temp for channel in bytes 1 and 2
#define CTL_GET_FAN_RPM  0x21  // works exactly like CTL_GET_TMP
#define CTL_SET_FAN_FPWM 0x23  // byte 1 is fan number
                               // byte 2 is percentage from 0 - 100


struct ccp_device {
        struct usb_device *udev;
        struct device *hwmondev;
        spinlock_t lock;
        int temp[4];
        int pwm[6];
	int fan_mode[6];
	int fan_enable[6];

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
                        HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_ENABLE,
                        HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_ENABLE,
                        HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_ENABLE,
                        HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_ENABLE,
                        HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_ENABLE,
                        HWMON_F_INPUT | HWMON_F_LABEL | HWMON_F_ENABLE
                        ),
        HWMON_CHANNEL_INFO(pwm,
                        HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
                        HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
                        HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
                        HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
                        HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
                        HWMON_PWM_INPUT | HWMON_PWM_ENABLE
                        ),
        NULL
};

static void dump_data(u8* data, int length)
{
        int i;
        for (i = 0; i < length; i ++) {
                printk(KERN_ALERT "%x - ", data[i]);
        }
        printk(KERN_ALERT "\n");
        return;
}

static int get_usb_data(struct ccp_device *ccp, u8* buffer)
{
        int ret;
        int actual_length;

        spin_lock(&(ccp->lock));

        ret = usb_bulk_msg(ccp->udev,
                        usb_sndintpipe(ccp->udev, 2),
                        buffer,
                        OUT_BUFFER_SIZE,
                        &actual_length,
                        1000);

        if(ret) {
                printk(KERN_ALERT "send usb %d ", ret);
                goto exit;
        }

        ret = usb_bulk_msg(ccp->udev,
                        usb_rcvintpipe(ccp->udev, 1),
                        buffer,
                        IN_BUFFER_SIZE,
                        &actual_length,
                        1000);

        if(ret) {
                printk(KERN_ALERT "rcv usb %d ", ret);
                goto exit;
        }
exit:
        spin_unlock(&(ccp->lock));
        return 0;
}

static int set_pwm(struct ccp_device *ccp, int channel, long val)
{
        int ret;
        int actual_length;
        u8 *buffer;

        ret = 0;

        if(val > 255) {
                return -EINVAL;
        }
        // The Corsair Commander Pro uses values from 0-100
        // so I need to convert it.

        val = val << 8;
        val = val / 255;
        val = val * 100;
        val = val >> 8;
//        printk(KERN_ALERT "val2 = %d\n", val);
	ccp->pwm[channel] = val;

        buffer = kzalloc(OUT_BUFFER_SIZE, GFP_KERNEL);
        if (buffer == 0) {
		dev_err(ccp->hwmondev, "Out of memory\n");
                return -ENOMEM;
        }

        buffer[0] = CTL_SET_FAN_FPWM;
        buffer[1] = channel;
        buffer[2] = val;

        ret = usb_bulk_msg(ccp->udev,
                        usb_sndintpipe(ccp->udev, 2),
                        buffer,
                        OUT_BUFFER_SIZE,
                        &actual_length,
                        1000);

        if(ret) {
                printk(KERN_ALERT "send usb %d ", ret);
                goto exit;
        }
        return 0;

exit:
        kfree(buffer);
        return ret <= 0 ? ret : -EIO;

}

static int get_fan_mode(struct ccp_device *ccp, int channel, const char** mode_desc)
{
        int ret;
        int actual_length;
	int mode;
        u8 *buffer;
        ret = 0;

        buffer = kzalloc(MAX_BUFFER_SIZE, GFP_KERNEL);
        if (buffer == 0) {
		dev_err(ccp->hwmondev, "Out of memory\n");
                return -ENOMEM;
        }

        buffer[0] = 0x20;

        ret = get_usb_data(ccp, buffer);
	mode = buffer[channel+1];
	switch(mode) {
	case 0:
		*mode_desc = "Auto/Disconnect";
		break;
	case 1:
		*mode_desc = "3 Pin";
		break;
	case 2:
		*mode_desc = "4 Pin";
		break;
	default:
		printk(KERN_ALERT "Mode Description for %d not implemented", mode);
		break;
	}

exit:
        kfree(buffer);
        return ret <= 0 ? ret : -EIO;
}

static int get_temp_or_rpm(struct ccp_device *ccp, int ctlrequest, int channel, long *val)
{
        int ret;
        int actual_length;
        u8 *buffer;
        ret = 0;

        buffer = kzalloc(MAX_BUFFER_SIZE, GFP_KERNEL);
        if (buffer == 0) {
		dev_err(ccp->hwmondev, "Out of memory\n");
                return -ENOMEM;
        }

        buffer[0] = ctlrequest;
        buffer[1] = channel;

        get_usb_data(ccp, buffer);


        *val = (buffer[1] << 8) + buffer[2];
exit:
        kfree(buffer);
        return ret <= 0 ? ret : -EIO;
}

static umode_t ccp_is_visible(const void *data, enum hwmon_sensor_types type,
                          u32 attr, int channel)
{
        switch (type) {
        case hwmon_chip:
                switch (attr) {
                case hwmon_chip_update_interval:
                        return 0644;
                }
                break;
        case hwmon_temp:
                switch (attr) {
                case hwmon_temp_input:
                        return 0444;
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
		}
                break;
        case hwmon_pwm:
                switch (attr) {
                case hwmon_pwm_input:
                        return 0644;
		case hwmon_pwm_enable:
			return 0644;
                }
                break;
        default:
                break;
        }
        return 0;
};

static int ccp_read_string(struct device *dev,
			   enum hwmon_sensor_types type,
			   u32 attr, int channel, const char **str)
{
	switch (type) {
	case hwmon_fan:
		switch (channel) {
		case 0:
			*str = "Fan1";
			return 0;
		case 1:
			*str = "Fan2";
			return 0;
		case 2:
			*str = "Fan3";
			return 0;
		case 3:
			*str = "Fan4";
			return 0;
		case 4:
			*str = "Fan5";
			return 0;
		case 5:
			*str = "Fan6";
			return 0;
		default:
			return -EINVAL;
		}
	case hwmon_temp:
//		sprintf(str, "Temp%d", channel);
		return 0;
	default:
		return -EINVAL;
	}
}

static int ccp_read(struct device* dev, enum hwmon_sensor_types type,
                    u32 attr, int channel, long *val)
{
        int err = 0;
        struct ccp_device *ccp;
//	char fanLabel[5];

        ccp = dev_get_drvdata(dev);
        switch(type) {
        case hwmon_temp:
                switch(attr) {
                case hwmon_temp_input:
                        get_temp_or_rpm(ccp, CTL_GET_TMP, channel, val);
                        *val = *val * 10;
                        break;
                default:
                        err = -EINVAL;
                        break;
                }
                break;
        case hwmon_fan:
                switch(attr) {
                case hwmon_fan_input:
			if(ccp->fan_enable[channel] == 1) {
	                        get_temp_or_rpm(ccp, CTL_GET_FAN_RPM, channel, val);
			} else {
				return -ENODATA;
			}
                        break;
		case hwmon_fan_enable:
			*val = ccp->fan_enable[channel];
			break;
                default:
                        err = -EINVAL;
                        break;
                }
                break;
	case hwmon_pwm:
		switch(attr) {
		case hwmon_pwm_input:
			*val = ccp->pwm[channel];
			break;
		case hwmon_pwm_enable:
			// automatic configuration not yet supported
			// need more info about device
			*val = 1;
			break;
		default:
			err = -EINVAL;
		}
        default:
                err = -EINVAL;
        }
        return err;
};

static int ccp_write(struct device* dev, enum hwmon_sensor_types type,
                     u32 attr, int channel, long val)
{
        int err = 0;
        struct ccp_device *ccp;

        ccp = dev_get_drvdata(dev);

        switch(type) {
	case hwmon_fan:
		switch(attr) {
		case hwmon_fan_enable:
			ccp->fan_enable[channel] = val;
			break;
		default:
			err = -EINVAL;
			break;
		}
		break;
        case hwmon_pwm:
                switch(attr) {
                case hwmon_pwm_input:
                        set_pwm(ccp, channel, val);
                        break;
                default:
                        err = -EINVAL;
                        break;
                }
                break;
        default:
                err = -EINVAL;
                break;
        }
        return err;
};

static const struct hwmon_ops ccp_hwmon_ops = {
        .is_visible = ccp_is_visible,
        .read = ccp_read,
	.read_string = ccp_read_string,
        .write = ccp_write,
};

static const struct hwmon_chip_info ccp_chip_info = {
        .ops = &ccp_hwmon_ops,
        .info = ccp_info,

};


static int ccp_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
        struct ccp_device *ccp;
	struct usb_device *udev = interface_to_usbdev(intf);
	struct device* hwmondev;
	int retval = -ENOMEM;

        printk(KERN_ALERT "ccp_probe\n");

	ccp = kzalloc(sizeof(struct ccp_device), GFP_KERNEL);

	if (ccp == NULL) {
		dev_err(&intf->dev, "Out of memory\n");
		goto error_mem;
	}

        spin_lock_init(&(ccp->lock));

	ccp->fan_enable[0] = 1;
	ccp->fan_enable[1] = 1;
	ccp->fan_enable[2] = 1;
	ccp->fan_enable[3] = 1;
	ccp->fan_enable[4] = 1;
	ccp->fan_enable[5] = 1;

	ccp->udev = usb_get_dev(udev);

	usb_set_intfdata (intf, ccp);

        ccp->hwmondev = devm_hwmon_device_register_with_info(&(udev->dev), // udev->dev
				"corsaircpro",
				ccp,
				&ccp_chip_info,
				0);

        return 0;

/*error:
	usb_set_intfdata (intf, NULL);
	usb_put_dev(ccp->udev);
	kfree(ccp);*/
error_mem:
	return retval;

}

static void ccp_disconnect(struct usb_interface *intf)
{
        struct ccp_device *ccp;

        ccp = usb_get_intfdata (intf);

        hwmon_device_unregister(ccp->hwmondev);

	usb_set_intfdata (intf, NULL);

	usb_put_dev(ccp->udev);

	kfree(ccp);

        printk(KERN_ALERT "ccp_disconnect\n");
}

static const struct usb_device_id ccp_devices[] = {
        { USB_DEVICE(USB_VENDOR_ID_CORSAIR, USB_VENDOR_ID_CORSAIR_CP) },
        { }
};


static struct usb_driver ccp_driver = {
        .name = "corsair-cpro",
        .id_table = ccp_devices,
        .probe = ccp_probe,
        .disconnect = ccp_disconnect,
};

static int __init ccp_init(void)
{
        int result;

        printk(KERN_ALERT "ccp_init\n");

        result = usb_register(&ccp_driver);
        if (result)
                printk(KERN_ALERT "usb_register failed, Error number %d", result);

        return result;
}

static void __exit ccp_exit(void)
{
        usb_deregister(&ccp_driver);
}

module_init(ccp_init);
module_exit(ccp_exit);
