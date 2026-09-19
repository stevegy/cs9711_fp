// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * cs9711.c - Chipsailing CS9711 fingerprint USB character device driver.
 *
 * This is an out-of-tree kernel module that exposes the CS9711 sensor as a
 * character device /dev/cs9711 with ioctl-based scan control and read() for
 * the 8024-byte raw sensor frame (34x236).
 */

#include <linux/kernel.h>
#include <linux/module.h>
/* Provided by the Linux kernel headers; build this module through kbuild. */
#include <linux/usb.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <asm/uaccess.h>

#include "cs9711.h"

#define CS9711_DRIVER_NAME "cs9711"
#define CS9711_MINOR_BASE 0

/* Forward declaration for usb_class_driver used in probe/disconnect. */
static struct usb_class_driver cs9711_usb_class_driver;

/* Per-device state. */
struct cs9711_dev {
	struct usb_device *udev;
	struct usb_interface *interface;
	unsigned int ep_in;
	unsigned int ep_out;
	u8 frame[CS9711_FRAME_SIZE];
	struct mutex io_lock;
	bool disconnected;
	bool open_active;
};

static struct class *cs9711_class;

/* Build an 8-byte command. */
static void cs9711_build_cmd(u8 *buf, u8 type)
{
	memset(buf, 0, CS9711_CMD_LEN);
	buf[0] = 0xEA;
	buf[CS9711_CMD_LEN - 1] = 0xEA;
	buf[1] = type;
	buf[CS9711_CMD_LEN - 2] = type;
}

/* Send a command via bulk OUT. */
static int cs9711_send_cmd(struct cs9711_dev *dev, u8 type, int timeout)
{
	u8 cmd[CS9711_CMD_LEN];
	int tx;

	cs9711_build_cmd(cmd, type);
	return usb_bulk_msg(dev->udev,
			    usb_sndbulkpipe(dev->udev, dev->ep_out),
			    cmd, sizeof(cmd), &tx, timeout);
}

/* Read reply via bulk IN and verify against expected magic. */
static int cs9711_verify_reply(struct cs9711_dev *dev, u8 type, int timeout)
{
	u8 buf[CS9711_CMD_LEN];
	int rx;
	int rc;

	rc = usb_bulk_msg(dev->udev,
			  usb_rcvbulkpipe(dev->udev, dev->ep_in),
			  buf, sizeof(buf), &rx, timeout);
	if (rc) {
		dev_err(&dev->interface->dev, "reply recv (cmd %u): rc=%d rx=%d\n", type, rc, rx);
		return rc;
	}
	if (rx != CS9711_CMD_LEN) {
		dev_err(&dev->interface->dev, "reply short read (cmd %u): %d/%d\n", type, rx, CS9711_CMD_LEN);
		return -EPROTO;
	}
	if (memcmp(buf, cs9711_expected_reply, CS9711_CMD_LEN)) {
		dev_err(&dev->interface->dev, "reply mismatch (cmd %u)\n", type);
		return -EIO;
	}
	return 0;
}

/* Do an INIT handshake: send INIT, verify reply. */
static int cs9711_handshake_init(struct cs9711_dev *dev)
{
	int rc;

	rc = cs9711_send_cmd(dev, CMD_INIT, 300);
	if (rc) {
		dev_err(&dev->interface->dev, "INIT send failed: %d\n", rc);
		return rc;
	}
	return cs9711_verify_reply(dev, CMD_INIT, 300);
}

/* Do a RESET command and drain the reply. */
static int cs9711_reset_drain(struct cs9711_dev *dev)
{
	u8 buf[CS9711_CMD_LEN];
	int rx;
	int rc;

	rc = cs9711_send_cmd(dev, CMD_RESET, 300);
	if (rc) {
		dev_err(&dev->interface->dev, "RESET send failed: %d\n", rc);
		return rc;
	}
	/* Best-effort drain of the reply. */
	rc = usb_bulk_msg(dev->udev,
			  usb_rcvbulkpipe(dev->udev, dev->ep_in),
			  buf, sizeof(buf), &rx, 100);
	if (rc)
		dev_dbg(&dev->interface->dev, "RESET drain: rc=%d rx=%d\n", rc, rx);

	return 0;
}

/* Do a full SCAN: send SCAN cmd, read block1(8000), read block2(24), post-scan RESET+drain. */
static int cs9711_do_scan(struct cs9711_dev *dev)
{
	u8 cmd[CS9711_CMD_LEN];
	int tx, rx, rc;

	/* Send SCAN command. */
	cs9711_build_cmd(cmd, CMD_SCAN);
	rc = usb_bulk_msg(dev->udev,
			  usb_sndbulkpipe(dev->udev, dev->ep_out),
			  cmd, sizeof(cmd), &tx, 300);
	if (rc || tx != CS9711_CMD_LEN) {
		dev_err(&dev->interface->dev, "SCAN send failed: rc=%d tx=%d\n", rc, tx);
		return rc ? rc : -EPROTO;
	}

	/* Read block 1: 8000 bytes. */
	rc = usb_bulk_msg(dev->udev,
			  usb_rcvbulkpipe(dev->udev, dev->ep_in),
			  dev->frame, CS9711_BLOCK1, &rx, 30000); /* generous for finger capture */
	if (rc || rx != CS9711_BLOCK1) {
		dev_err(&dev->interface->dev, "SCAN block1 failed: rc=%d rx=%d\n", rc, rx);
		return rc ? rc : -EPROTO;
	}

	/* Read block 2: 24 bytes. */
	rc = usb_bulk_msg(dev->udev,
			  usb_rcvbulkpipe(dev->udev, dev->ep_in),
			  dev->frame + CS9711_BLOCK1, CS9711_BLOCK2, &rx, 300);
	if (rc || rx != CS9711_BLOCK2) {
		dev_err(&dev->interface->dev, "SCAN block2 failed: rc=%d rx=%d\n", rc, rx);
		return rc ? rc : -EPROTO;
	}

	/* Post-scan RESET + drain. */
	cs9711_reset_drain(dev);

	return 0;
}

/* USB probe callback. */
static int cs9711_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(interface);
	struct cs9711_dev *dev;
	int rc;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->udev = udev;
	dev->interface = interface;
	dev->ep_in = 0x81;
	dev->ep_out = 0x01;
	mutex_init(&dev->io_lock);
	dev->disconnected = false;
	dev->open_active = false;

	/* Claim interface. */
	rc = usb_set_interface(dev->udev, interface->altsetting[0].desc.bInterfaceNumber, 0);
	if (rc) {
		dev_err(&interface->dev, "set_interface failed: %d\n", rc);
		goto err_free;
	}

	/* Register as a minor-based char device. */
	rc = usb_register_dev(interface, &cs9711_usb_class_driver);
	if (rc) {
		dev_err(&interface->dev, "usb_register_dev failed: %d\n", rc);
		goto err_release;
	}

	/* Store in usb_interface. */
	usb_set_intfdata(interface, dev);

	dev_info(&interface->dev, "CS9711 fingerprint device probed (vid=%04x pid=%04x)\n",
		 le16_to_cpu(udev->descriptor.idVendor), le16_to_cpu(udev->descriptor.idProduct));

	return 0;

err_release:
	usb_set_interface(dev->udev, interface->altsetting[0].desc.bInterfaceNumber, 0);
err_free:
	kfree(dev);
	return rc;
}

/* USB disconnect callback. */
static void cs9711_disconnect(struct usb_interface *interface)
{
	struct cs9711_dev *dev = usb_get_intfdata(interface);

	if (!dev)
		return;

	mutex_lock(&dev->io_lock);
	dev->disconnected = true;
	mutex_unlock(&dev->io_lock);

	usb_deregister_dev(interface, &cs9711_usb_class_driver);

	/* Release interface. */
	usb_set_interface(dev->udev, interface->altsetting[0].desc.bInterfaceNumber, 0);

	usb_set_intfdata(interface, NULL);
	kfree(dev);

	dev_info(&interface->dev, "CS9711 device disconnected\n");
}

static const struct usb_device_id cs9711_id_table[] = {
	{ USB_DEVICE(CS9711_VID, CS9711_PID) },
	{ USB_DEVICE(0x2541, 0x0236) }, /* also supported per reference */
	{ }
};
MODULE_DEVICE_TABLE(usb, cs9711_id_table);

static struct usb_driver cs9711_driver = {
	.name = CS9711_DRIVER_NAME,
	.probe = cs9711_probe,
	.disconnect = cs9711_disconnect,
	.id_table = cs9711_id_table,
	.supports_autosuspend = 1,
};

/* File operations. */
static int cs9711_open(struct inode *inode, struct file *file)
{
	struct usb_interface *interface;
	struct cs9711_dev *dev;
	int rc;

	interface = usb_find_interface(&cs9711_driver, iminor(inode));
	if (!interface)
		return -ENODEV;

	dev = usb_get_intfdata(interface);
	if (!dev)
		return -ENODEV;

	mutex_lock(&dev->io_lock);
	if (dev->disconnected) {
		rc = -ENODEV;
		goto err_unlock;
	}
	if (dev->open_active) {
		rc = -EBUSY;
		goto err_unlock;
	}
	dev->open_active = true;
	mutex_unlock(&dev->io_lock);

	/* Do INIT handshake on open. */
	rc = cs9711_handshake_init(dev);
	if (rc) {
		mutex_lock(&dev->io_lock);
		dev->open_active = false;
		goto err_unlock;
	}

	file->private_data = dev;
	return 0;

err_unlock:
	mutex_unlock(&dev->io_lock);
	return rc;
}

static int cs9711_release(struct inode *inode, struct file *file)
{
	struct cs9711_dev *dev = file->private_data;

	if (!dev)
		return 0;

	mutex_lock(&dev->io_lock);
	dev->open_active = false;
	mutex_unlock(&dev->io_lock);

	/* Send RESET on release. */
	cs9711_reset_drain(dev);

	return 0;
}

static long cs9711_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct cs9711_dev *dev = file->private_data;
	int rc = 0;

	if (!dev || dev->disconnected)
		return -ENODEV;

	mutex_lock(&dev->io_lock);

	switch (cmd) {
	case CS9711_IOC_SCAN:
		rc = cs9711_do_scan(dev);
		break;
	case CS9711_IOC_RESET:
		rc = cs9711_reset_drain(dev);
		break;
	case CS9711_IOC_GETINFO: {
		struct cs9711_info info;

		info.width = CS9711_SENSOR_WIDTH;
		info.height = CS9711_SENSOR_HEIGHT;
		info.frame_size = CS9711_FRAME_SIZE;

		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			rc = -EFAULT;
		break;
	}
	default:
		rc = -ENOTTY;
		break;
	}

	mutex_unlock(&dev->io_lock);
	return rc;
}

static ssize_t cs9711_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct cs9711_dev *dev = file->private_data;
	int rc;

	if (!dev || dev->disconnected)
		return -ENODEV;

	if (*ppos != 0)
		return 0; /* only read from start */

	if (count > CS9711_FRAME_SIZE)
		count = CS9711_FRAME_SIZE;

	mutex_lock(&dev->io_lock);
	rc = copy_to_user(buf, dev->frame, count) ? -EFAULT : 0;
	mutex_unlock(&dev->io_lock);

	return rc ? rc : (ssize_t)count;
}

static const struct file_operations cs9711_fops = {
	.owner = THIS_MODULE,
	.open = cs9711_open,
	.release = cs9711_release,
	.unlocked_ioctl = cs9711_ioctl,
	.read = cs9711_read,
	.llseek = noop_llseek,
};

static struct usb_class_driver cs9711_usb_class_driver = {
	.name = "cs9711/%d",
	.fops = &cs9711_fops,
	.minor_base = CS9711_MINOR_BASE,
};

static int __init cs9711_init(void)
{
	int rc;

	cs9711_class = class_create("cs9711");
	if (IS_ERR(cs9711_class)) {
		pr_err("cs9711: class_create failed\n");
		return PTR_ERR(cs9711_class);
	}

	rc = usb_register(&cs9711_driver);
	if (rc) {
		pr_err("cs9711: usb_register failed: %d\n", rc);
		class_destroy(cs9711_class);
		return rc;
	}

	pr_info("cs9711: driver loaded, class registered\n");
	return 0;
}

static void __exit cs9711_exit(void)
{
	usb_deregister(&cs9711_driver);
	class_destroy(cs9711_class);
	pr_info("cs9711: driver unloaded\n");
}

module_init(cs9711_init);
module_exit(cs9711_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CS9711 Driver Project");
MODULE_DESCRIPTION("Chipsailing CS9711 Fingerprint USB Character Device Driver");
