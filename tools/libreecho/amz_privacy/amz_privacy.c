// SPDX-License-Identifier: GPL-2.0-only
/*
 * amz_privacy: the MT8163 kernel interface LibreEcho reads for the hardware
 * microphone privacy latch, on top of mainline's input switch.
 *
 * On lidar the latch is a gpio-keys switch (EV_SW, SW_MUTE_DEVICE). This
 * module follows that switch with an input handler and exposes it where
 * LibreEcho looks:
 *   /sys/devices/platform/amz_privacy/privacy_state   0 or 1 (read only)
 *   /sys/devices/platform/amz_privacy/mute_lamp       accepted and kept
 */
#include <linux/input.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

static struct platform_device *pdev;
static int privacy_state;
static int mute_lamp;

static ssize_t privacy_state_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(privacy_state));
}
static DEVICE_ATTR_RO(privacy_state);

static ssize_t mute_lamp_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(mute_lamp));
}

static ssize_t mute_lamp_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	int v, ret = kstrtoint(buf, 0, &v);

	if (ret)
		return ret;
	WRITE_ONCE(mute_lamp, !!v);
	return count;
}
static DEVICE_ATTR_RW(mute_lamp);

static struct attribute *amz_privacy_attrs[] = {
	&dev_attr_privacy_state.attr,
	&dev_attr_mute_lamp.attr,
	NULL,
};
ATTRIBUTE_GROUPS(amz_privacy);

static void amz_privacy_event(struct input_handle *handle, unsigned int type,
			      unsigned int code, int value)
{
	if (type == EV_SW && code == SW_MUTE_DEVICE)
		WRITE_ONCE(privacy_state, !!value);
}

static int amz_privacy_connect(struct input_handler *handler,
			       struct input_dev *dev,
			       const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;
	handle->dev = dev;
	handle->handler = handler;
	handle->name = "amz_privacy";
	ret = input_register_handle(handle);
	if (ret)
		goto err_free;
	ret = input_open_device(handle);
	if (ret)
		goto err_unregister;
	WRITE_ONCE(privacy_state, test_bit(SW_MUTE_DEVICE, dev->sw));
	return 0;

err_unregister:
	input_unregister_handle(handle);
err_free:
	kfree(handle);
	return ret;
}

static void amz_privacy_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id amz_privacy_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_SWBIT,
		.evbit = { BIT_MASK(EV_SW) },
		.swbit = { [BIT_WORD(SW_MUTE_DEVICE)] = BIT_MASK(SW_MUTE_DEVICE) },
	},
	{ }
};

static struct input_handler amz_privacy_handler = {
	.event = amz_privacy_event,
	.connect = amz_privacy_connect,
	.disconnect = amz_privacy_disconnect,
	.name = "amz_privacy",
	.id_table = amz_privacy_ids,
};

static int __init amz_privacy_init(void)
{
	struct platform_device_info info = {
		.name = "amz_privacy",
		.id = PLATFORM_DEVID_NONE,
	};
	int ret;

	pdev = platform_device_register_full(&info);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);
	ret = device_add_groups(&pdev->dev, amz_privacy_groups);
	if (ret)
		goto err_pdev;
	ret = input_register_handler(&amz_privacy_handler);
	if (ret)
		goto err_groups;
	return 0;

err_groups:
	device_remove_groups(&pdev->dev, amz_privacy_groups);
err_pdev:
	platform_device_unregister(pdev);
	return ret;
}

static void __exit amz_privacy_exit(void)
{
	input_unregister_handler(&amz_privacy_handler);
	device_remove_groups(&pdev->dev, amz_privacy_groups);
	platform_device_unregister(pdev);
}

module_init(amz_privacy_init);
module_exit(amz_privacy_exit);
MODULE_DESCRIPTION("LibreEcho privacy-latch interface on top of SW_MUTE_DEVICE");
MODULE_LICENSE("GPL");
