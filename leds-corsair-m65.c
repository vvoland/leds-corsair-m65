#include <linux/completion.h>
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
	const char* name;
	int zone_id;
	u32 default_color;
};

static const struct corsair_mouse_led_zone corsair_m65_zones[] = {
	{ .name = "wheel", .zone_id = 1, .default_color = 0x00FFFF },
	{ .name = "logo",  .zone_id = 2, .default_color = 0xFFFF00 },
	{ .name = "dpi",   .zone_id = 3, .default_color = 0x00FF00 },
};

struct corsair_m65_led {
	struct led_classdev cdev;
	const struct corsair_mouse_led_zone *zone;
	bool removed;
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

#define CORSAIR_M65_LEDS_COUNT	  3
struct corsair_m65_data {
	struct hid_device* hdev;
	struct corsair_m65_led leds[CORSAIR_M65_LEDS_COUNT];
	const struct corsair_mouse_led_zone *zones;

	/* 
	 * All zones data (even those that didn't change) must be sent in each command
	 * So keep the whole command and mutate it
	 */
	struct corsair_zones_request *color_cmd;
};

#define CORSAIR_CMD_BUFFER_SIZE		      65

#define CORSAIR_CMD_WRITE                     0x07
#define CORSAIR_CMD_PROPERTY_SUBMIT_COLOR     0x22

/*
 * Allocate and initialize command that sets the zone colors
 * After this, only zones_count and zones members have to be mutated
 */
static struct corsair_zones_request* corsair_request_alloc_colors(struct hid_device *hdev, const struct corsair_mouse_led_zone zones[], size_t zones_sizeof)
{
	struct corsair_zones_request *cmd = devm_kzalloc(&hdev->dev, sizeof(struct corsair_zones_request), GFP_KERNEL);
	int i;

	if (!cmd)
		return NULL;

	cmd->report_num = 0;
	cmd->cmd = CORSAIR_CMD_WRITE;
	cmd->subcmd = CORSAIR_CMD_PROPERTY_SUBMIT_COLOR;
	cmd->zones_count = zones_sizeof / sizeof(struct corsair_mouse_led_zone);
	if (cmd->zones_count >= CORSAIR_MOUSE_MAX_ZONES_COUNT)
	{
		dev_err(&hdev->dev, "zones_count too big: %u", cmd->zones_count);
		return NULL;
	}

	for (i = 0; i < cmd->zones_count; ++i)
	{
		u32 defcolor = zones[i].default_color;
		cmd->zones[i].r = (defcolor >> 16) & 0xFF;
		cmd->zones[i].g = (defcolor >> 8)  & 0xFF;
		cmd->zones[i].b = (defcolor >> 0)  & 0xFF;
		cmd->zones[i].zone = zones[i].zone_id;
	}

	// Add last empty zone
	//cmd->zones[cmd->zones_count].zone = cmd->zones_count + 1;
	//cmd->zones_count++;

	cmd->_unknown = 0;

	return cmd;
}

static int corsair_m65_set_color(struct corsair_m65_data *data, const struct corsair_mouse_led_zone *zone, u8 red, u8 green, u8 blue)
{
	struct corsair_zone_data *zone_data;
	u32 zone_idx = zone - data->zones;

	if (zone_idx >= data->color_cmd->zones_count)
	{
		printk(KERN_ERR "Invalid zone index %u", zone_idx);
		return -EINVAL;
	}

	zone_data = &data->color_cmd->zones[zone_idx];
	if (zone_data->zone != zone->zone_id)
	{
		printk(KERN_ERR "Zone id mismatched (%u != %u)", zone_data->zone, zone->zone_id);
		return -EINVAL;
	}

	// If try to set the same color
	if (zone_data->r == red && zone_data->g == green && zone_data->b == blue)
	{
		return -EEXIST;
	}

	zone_data->r = red;
	zone_data->g = green;
	zone_data->b = blue;

	return 0;
}

static int corsair_m65_submit_color(struct hid_device *hdev, struct corsair_m65_data *priv)
{
	int ret;

#if 0
	int i;
	for (i = 0; i < priv->color_cmd->zones_count; ++i)
	{
		u8 id = priv->color_cmd->zones[i].zone;
		u8 r = priv->color_cmd->zones[i].r;
		u8 g = priv->color_cmd->zones[i].g;
		u8 b = priv->color_cmd->zones[i].b;
		printk(KERN_DEBUG "Zone idx: %u, id: %u, red: %X, green: %X, blue: %X", i, id, r, g, b);
	}
#endif

	//ret = hid_hw_raw_request(hdev, priv->color_cmd->cmd, (u8*)(priv->color_cmd), CORSAIR_CMD_BUFFER_SIZE, HID_OUTPUT_REPORT, HID_REQ_SET_REPORT);

	ret = hid_hw_output_report(hdev, (u8*)priv->color_cmd, CORSAIR_CMD_BUFFER_SIZE);

	if (ret < 0)
	{
		hid_err(hdev, "Failed to output report, err %d", ret);
		return ret;
	}

	return 0;
}

static int corsair_m65_led_set(struct led_classdev *cdev, enum led_brightness brightness)
{
	int ret;
	u32 value = (u32)brightness;

	u8 red   = (value >> 16) & 0xFF;
	u8 green = (value >> 8)  & 0xFF;
	u8 blue  = (value >> 0)  & 0xFF;

	struct corsair_m65_data *priv;
	struct corsair_m65_led *led = container_of(cdev, struct corsair_m65_led, cdev);
	struct hid_device *hdev = to_hid_device(cdev->dev->parent);

	if (!hdev)
	{
		dev_err(cdev->dev, "Could not get a hid_device for led %s", cdev->name);
		return -ENODEV;
	}

	priv = hid_get_drvdata(hdev);

	ret = corsair_m65_set_color(priv, led->zone, red, green, blue);

	switch (ret)
	{
		// Don't submit data if the color is the same
		case -EEXIST: 
			ret = 0;
			break;
		case 0:
			ret = corsair_m65_submit_color(hdev, priv);
			break;
		default:
			ret = -ENODEV;
	}

	return ret;
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
	priv->color_cmd = corsair_request_alloc_colors(hdev, corsair_m65_zones, sizeof(corsair_m65_zones));
	if (!priv->color_cmd)
		return -ENOMEM;

	for (i = 0; i < CORSAIR_M65_LEDS_COUNT; ++i) {
		int ret;
		struct corsair_m65_led *led = &priv->leds[i];
		const struct corsair_mouse_led_zone *zone = &priv->zones[i];

		char *name = devm_kasprintf(&hdev->dev, GFP_KERNEL, "corsair_m65::%s", zone->name);
		if (!name)
		{
			hid_err(hdev, "Could not allocate memory for zone name");
			return -ENOMEM;
		}

		led->zone = zone;
		led->cdev.name = name;
		led->cdev.max_brightness = 0xFFFFFF;
		led->cdev.brightness_set_blocking = corsair_m65_led_set;
		led->cdev.dev = &hdev->dev;

		printk("dev %s\n", name);
		ret = devm_led_classdev_register(&hdev->dev, &led->cdev);
		if (ret < 0)
		{
			led->removed = true;
			hid_err(hdev, "Could not register led %s", name);
			return ret;
		}
	}

	return corsair_m65_submit_color(hdev, priv);
}

static int corsair_m65_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct corsair_m65_data *priv;
	int ret;

	ret = hid_parse(hdev);
	if (ret)
	{
		hid_err(hdev, "Failed to parse hid device");
		return ret;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret)
	{
		hid_err(hdev, "Failed to start hid device");
		return ret;
	}

	if (corsair_m65_is_control_interface(hdev))
	{
		priv = devm_kzalloc(&hdev->dev, sizeof(struct corsair_m65_data), GFP_KERNEL);
		if (!priv)
		{
			ret = -ENOMEM;
			goto stop;
		}

		priv->hdev = hdev;
		hid_set_drvdata(hdev, priv);

		ret = hid_hw_open(hdev);

		if (ret)
		{
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
	.name		= "corsair-m65-leds",
	.id_table	= corsair_m65_idtable,
	.probe		= corsair_m65_probe,
	.remove		= corsair_m65_remove,
};
module_hid_driver(corsair_m65_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Paweł Gronowski <me@woland.xyz>");
MODULE_DESCRIPTION("Linux driver for Corsair M65 leds");

// vim: ts=8 noet sw=8 :
