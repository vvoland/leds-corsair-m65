// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/errno.h>
#include <linux/hid.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/usb.h>

#define USB_VENDOR_ID_CORSAIR               0x1b1c
#define USB_DEVICE_ID_CORSAIR_M65_PRO_RGB   0x1b2e

struct corsair_mouse_led_zone {
	const char *name;
	int zone_id;
	u32 default_color;
};

#define CORSAIR_M65_LEDS_COUNT	  3
static const struct corsair_mouse_led_zone corsair_m65_zones[] = {
	{ .name = "wheel", .zone_id = 1, .default_color = 0x00FFFF },
	{ .name = "logo",  .zone_id = 2, .default_color = 0xFFFF00 },
	{ .name = "dpi",   .zone_id = 3, .default_color = 0x00FF00 },
};

struct corsair_zone_data {
	u8 zone;
	u8 r, g, b;
};

#define CORSAIR_MOUSE_MAX_ZONES_COUNT   15
struct corsair_zones_request {
	u8 report_num;
	u8 cmd;
	u8 subcmd;
	u8 zones_count;
	u8 _unknown;
	struct corsair_zone_data zones[CORSAIR_MOUSE_MAX_ZONES_COUNT];
} __packed;


struct corsair_m65_led {
	struct led_classdev cdev;
	struct corsair_zone_data *zone_data;
	bool removed;
};

struct corsair_m65_data {
	struct hid_device *hdev;
	struct corsair_m65_led leds[CORSAIR_M65_LEDS_COUNT];
	const struct corsair_mouse_led_zone *zones;

	/*
	 * All zones data (even those that didn't change) must be sent in each command
	 * So store the whole command and mutate it
	 */
	struct corsair_zones_request *color_cmd;
};

#define CORSAIR_CMD_BUFFER_SIZE               65

#define CORSAIR_CMD_WRITE                     0x07
#define CORSAIR_CMD_PROPERTY_SUBMIT_COLOR     0x22

/*
 * Allocate and initialize command that sets the zone colors
 * After this, only zones_count and zones members have to be mutated
 */
static struct corsair_zones_request *corsair_alloc_command(
		struct hid_device *hdev,
		const struct corsair_mouse_led_zone zones[],
		size_t zones_sizeof)
{
	struct corsair_zones_request *cmd = devm_kzalloc(&hdev->dev, sizeof(struct corsair_zones_request), GFP_KERNEL);
	int i;

	if (!cmd)
		return NULL;

	cmd->report_num = 0;
	cmd->cmd = CORSAIR_CMD_WRITE;
	cmd->subcmd = CORSAIR_CMD_PROPERTY_SUBMIT_COLOR;
	cmd->zones_count = zones_sizeof / sizeof(struct corsair_mouse_led_zone);
	if (cmd->zones_count >= CORSAIR_MOUSE_MAX_ZONES_COUNT) {
		dev_err(&hdev->dev, "zones_count too big: %u", cmd->zones_count);
		return NULL;
	}

	for (i = 0; i < cmd->zones_count; ++i) {
		u32 defcolor = zones[i].default_color;

		cmd->zones[i].r = (defcolor >> 16) & 0xFF;
		cmd->zones[i].g = (defcolor >> 8)  & 0xFF;
		cmd->zones[i].b = (defcolor >> 0)  & 0xFF;
		cmd->zones[i].zone = zones[i].zone_id;
	}

	cmd->_unknown = 0;

	return cmd;
}

static struct corsair_zone_data *corsair_m65_get_cmd_zone_data(struct corsair_m65_data *data, const struct corsair_mouse_led_zone *zone)
{
	struct corsair_zone_data *zone_data;
	u32 zone_idx = zone - data->zones;

	if (zone_idx >= data->color_cmd->zones_count)
		return NULL;

	zone_data = &data->color_cmd->zones[zone_idx];
	if (zone_data->zone != zone->zone_id)
		return NULL;

	return zone_data;
}

static int corsair_m65_submit_color(struct hid_device *hdev)
{
	struct corsair_m65_data *priv = hid_get_drvdata(hdev);
	int ret = hid_hw_output_report(hdev, (u8 *)priv->color_cmd, CORSAIR_CMD_BUFFER_SIZE);

	if (ret < 0) {
		hid_err(hdev, "Failed to output report, err %d", ret);
		return ret;
	}

	return 0;
}

static int corsair_m65_led_set(struct led_classdev *cdev, enum led_brightness brightness)
{
	struct corsair_m65_led *led = container_of(cdev, struct corsair_m65_led, cdev);
	struct hid_device *hdev = to_hid_device(cdev->dev->parent);
	struct corsair_zone_data *zone_data = led->zone_data;

	u32 value = (u32)brightness;
	u8 r = (value >> 16) & 0xFF;
	u8 g = (value >> 8)  & 0xFF;
	u8 b = (value >> 0)  & 0xFF;

	// Don't submit command if the color didn't change
	if (zone_data->r == r && zone_data->g == g && zone_data->b == b)
		return 0;

	zone_data->r = r;
	zone_data->g = g;
	zone_data->b = b;

	return corsair_m65_submit_color(hdev);
}

static bool corsair_m65_is_control_interface(struct hid_device *hdev)
{
	struct usb_interface *intf = to_usb_interface(hdev->dev.parent);

	return intf->cur_altsetting->desc.bInterfaceNumber == 1;
}

static int corsair_m65_init(struct corsair_m65_data *priv)
{
	int i;
	struct hid_device *hdev = priv->hdev;

	priv->zones = corsair_m65_zones;
	priv->color_cmd = corsair_alloc_command(hdev, corsair_m65_zones, sizeof(corsair_m65_zones));
	if (!priv->color_cmd)
		return -ENOMEM;

	for (i = 0; i < CORSAIR_M65_LEDS_COUNT; ++i) {
		int ret;
		char *name;
		struct corsair_m65_led *led = &priv->leds[i];
		const struct corsair_mouse_led_zone *mouse_zone = &priv->zones[i];
		struct corsair_zone_data *zone_data = corsair_m65_get_cmd_zone_data(priv, mouse_zone);

		if (!zone_data) {
			hid_err(hdev, "Could not get zone data in command for zone %s", mouse_zone->name);
			continue;
		}

		name = devm_kasprintf(&hdev->dev, GFP_KERNEL, "corsair_m65::%s", mouse_zone->name);
		if (!name) {
			hid_err(hdev, "Could not allocate memory for zone name");
			return -ENOMEM;
		}

		led->zone_data = zone_data;
		led->cdev.name = name;
		led->cdev.max_brightness = 0xFFFFFF;
		led->cdev.brightness_set_blocking = corsair_m65_led_set;
		led->cdev.dev = &hdev->dev;

		hid_info(hdev, "Registering mouse led %s", name);
		ret = devm_led_classdev_register(&hdev->dev, &led->cdev);
		if (ret < 0) {
			led->removed = true;
			hid_err(hdev, "Could not register led %s", name);
			return ret;
		}
	}

	return corsair_m65_submit_color(hdev);
}

static int corsair_m65_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct corsair_m65_data *priv;
	int ret;

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "Failed to parse hid device");
		return ret;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret) {
		hid_err(hdev, "Failed to start hid device");
		return ret;
	}

	if (corsair_m65_is_control_interface(hdev)) {
		priv = devm_kzalloc(&hdev->dev, sizeof(struct corsair_m65_data), GFP_KERNEL);
		if (!priv) {
			ret = -ENOMEM;
			goto stop;
		}

		priv->hdev = hdev;
		hid_set_drvdata(hdev, priv);

		ret = hid_hw_open(hdev);

		if (ret) {
			hid_err(hdev, "Failed to open hid device");
			goto stop;
		}

		ret = corsair_m65_init(priv);
		if (ret < 0)
			goto close_and_stop;
	}

	return 0;
close_and_stop:
	hid_hw_close(hdev);
stop:
	hid_hw_stop(hdev);
	return ret;
}

static void corsair_m65_remove(struct hid_device *hdev)
{
	hid_hw_stop(hdev);
}

static const struct hid_device_id corsair_m65_idtable[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR, USB_DEVICE_ID_CORSAIR_M65_PRO_RGB) },
	{ },
};
MODULE_DEVICE_TABLE(hid, corsair_m65_idtable);

static struct hid_driver corsair_m65_driver = {
	.name           = "corsair-m65-leds",
	.id_table       = corsair_m65_idtable,
	.probe          = corsair_m65_probe,
	.remove         = corsair_m65_remove,
};
module_hid_driver(corsair_m65_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Paweł Gronowski <me@woland.xyz>");
MODULE_DESCRIPTION("Linux driver for Corsair M65 leds");

// vim: ts=8 noet sw=8 :
