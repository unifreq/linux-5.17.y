// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Titan Micro Electronics TM1628 LED controller
 *
 * Copyright (c) 2019 Andreas Färber
 */

#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/spi/spi.h>
#include <uapi/linux/map_to_7segment.h>

#define TM1628_CMD_DISPLAY_MODE		(0 << 6)
#define TM1628_DISPLAY_MODE_6_12	0x02
#define TM1628_DISPLAY_MODE_7_11	0x03

#define TM1628_CMD_DATA			(1 << 6)
#define TM1628_DATA_TEST_MODE		BIT(3)
#define TM1628_DATA_FIXED_ADDR		BIT(2)
#define TM1628_DATA_WRITE_DATA		0x00
#define TM1628_DATA_READ_DATA		0x02

#define TM1628_CMD_DISPLAY_CTRL		(2 << 6)
#define TM1628_DISPLAY_CTRL_DISPLAY_ON	BIT(3)

#define TM1628_CMD_SET_ADDRESS		(3 << 6)

#define TM1628_BRIGHTNESS_MAX		7

/* Physical limits, depending on the mode the chip may support less */
#define MAX_GRID_SIZE			7
#define MAX_SEGMENT_NUM			16

struct tm1628_led {
	struct led_classdev	leddev;
	struct tm1628		*ctrl;
	u32			grid;
	u32			seg;
};

struct tm1628 {
	struct spi_device		*spi;
	__le16				data[MAX_GRID_SIZE];
	struct mutex			disp_lock;
	char				text[MAX_GRID_SIZE + 1];
	u8				segment_mapping[7];
	u8				grid[MAX_GRID_SIZE];
	int				grid_size;
	struct tm1628_led		leds[];
};

/* Command 1: Display Mode Setting */
static int tm1628_set_display_mode(struct spi_device *spi, u8 grid_mode)
{
	u8 cmd = TM1628_CMD_DISPLAY_MODE | grid_mode;

	return spi_write(spi, &cmd, 1);
}

/* Command 3: Address Setting */
static int tm1628_set_address(struct spi_device *spi, u8 offset)
{
	u8 cmd = TM1628_CMD_SET_ADDRESS | (offset * sizeof(__le16));

	return spi_write(spi, &cmd, 1);
}

/* Command 2: Data Setting */
static int tm1628_write_data(struct spi_device *spi, unsigned int offset,
			     unsigned int len)
{
	struct tm1628 *s = spi_get_drvdata(spi);
	u8 cmd = TM1628_CMD_DATA | TM1628_DATA_WRITE_DATA;
	struct spi_transfer xfers[] = {
		{
			.tx_buf = &cmd,
			.len = 1,
		},
		{
			.tx_buf = (__force void *)(s->data + offset),
			.len = len * sizeof(__le16),
		},
	};

	if (offset + len > MAX_GRID_SIZE) {
		dev_err(&spi->dev, "Invalid data address offset %u len %u\n",
			offset, len);
		return -EINVAL;
	}

	tm1628_set_address(spi, offset);

	return spi_sync_transfer(spi, xfers, ARRAY_SIZE(xfers));
}

/* Command 4: Display Control */
static int tm1628_set_display_ctrl(struct spi_device *spi, bool on)
{
	u8 cmd = TM1628_CMD_DISPLAY_CTRL | TM1628_BRIGHTNESS_MAX;

	if (on)
		cmd |= TM1628_DISPLAY_CTRL_DISPLAY_ON;

	return spi_write(spi, &cmd, 1);
}

static int tm1628_show_text(struct tm1628 *s)
{
	static SEG7_CONVERSION_MAP(map_seg7, MAP_ASCII7SEG_ALPHANUM);
	int i, ret;

	int msg_len = strlen(s->text);

	mutex_lock(&s->disp_lock);

	for (i = 0; i < s->grid_size; i++) {
		int pos = s->grid[i] - 1;

		if (i < msg_len) {
			int char7_raw = map_to_seg7(&map_seg7, s->text[i]);
			int j, char7;

			for (j = 0, char7 = 0; j < 7; j++) {
				if (char7_raw & BIT(j))
					char7 |= BIT(s->segment_mapping[j] - 1);
			}

			s->data[pos] = cpu_to_le16(char7);
		} else {
			s->data[pos] = 0;
		}
	}

	ret = tm1628_write_data(s->spi, 0, s->grid_size);

	mutex_unlock(&s->disp_lock);

	return ret;
}

static int tm1628_led_set_brightness(struct led_classdev *led_cdev,
				     enum led_brightness brightness)
{
	struct tm1628_led *led = container_of(led_cdev, struct tm1628_led, leddev);
	struct tm1628 *s = led->ctrl;
	int offset, ret;
	__le16 bit;

	offset = led->grid - 1;
	bit = cpu_to_le16(BIT(led->seg - 1));

	mutex_lock(&s->disp_lock);

	if (brightness == LED_OFF)
		s->data[offset] &= ~bit;
	else
		s->data[offset] |= bit;

	ret = tm1628_write_data(s->spi, offset, 1);

	mutex_unlock(&s->disp_lock);

	return ret;
}

static enum led_brightness tm1628_led_get_brightness(struct led_classdev *led_cdev)
{
	struct tm1628_led *led = container_of(led_cdev, struct tm1628_led, leddev);
	struct tm1628 *s = led->ctrl;
	int offset;
	__le16 bit;
	bool on;

	offset = led->grid - 1;
	bit = cpu_to_le16(BIT(led->seg - 1));

	mutex_lock(&s->disp_lock);
	on = s->data[offset] & bit;
	mutex_unlock(&s->disp_lock);

	return on ? LED_ON : LED_OFF;
}

static int tm1628_register_led(struct tm1628 *s, struct fwnode_handle *node,
			       u32 grid, u32 seg, struct tm1628_led *led)
{
	struct device *dev = &s->spi->dev;
	struct led_init_data init_data = { .fwnode = node };

	led->ctrl = s;
	led->grid = grid;
	led->seg  = seg;
	led->leddev.max_brightness = LED_ON;
	led->leddev.brightness_set_blocking = tm1628_led_set_brightness;
	led->leddev.brightness_get = tm1628_led_get_brightness;

	return devm_led_classdev_register_ext(dev, &led->leddev, &init_data);
}

static ssize_t display_text_show(struct device *dev, struct device_attribute *attr,
				 char *buf)
{
	struct tm1628 *s = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", s->text);
}

static ssize_t display_text_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct tm1628 *s = dev_get_drvdata(dev);
	int ret, i;

	if (count > s->grid_size + 1) /* consider trailing newline */
		return -E2BIG;

	for (i = 0; i < count && isprint(buf[i]); i++)
		s->text[i] = buf[i];

	s->text[i] = '\0';

	ret = tm1628_show_text(s);
	if (ret < 0)
		return ret;

	return count;
}

static const DEVICE_ATTR_RW(display_text);

static int tm1628_spi_probe(struct spi_device *spi)
{
	struct fwnode_handle *child;
	unsigned int num_leds;
	struct tm1628 *s;
	int ret, i;

	num_leds = device_get_child_node_count(&spi->dev);

	s = devm_kzalloc(&spi->dev, struct_size(s, leds, num_leds), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	s->spi = spi;
	spi_set_drvdata(spi, s);

	mutex_init(&s->disp_lock);

	msleep(200); /* according to TM1628 datasheet */

	/* clear screen */
	ret = tm1628_write_data(spi, 0, MAX_GRID_SIZE);
	if (ret)
		return ret;
	/* Assume that subsequent SPI transfers will be ok if first was ok */

	/* For now we support 6x12 mode only. This should be sufficient for most use cases */
	tm1628_set_display_mode(spi, TM1628_DISPLAY_MODE_6_12);

	tm1628_set_display_ctrl(spi, true);

	if (!IS_REACHABLE(CONFIG_LEDS_CLASS))
		goto no_leds;

	num_leds = 0;

	device_for_each_child_node(&spi->dev, child) {
		u32 reg[2];

		ret = fwnode_property_read_u32_array(child, "reg", reg, 2);
		if (ret) {
			dev_err(&spi->dev, "Reading %s reg property failed (%d)\n",
				fwnode_get_name(child), ret);
			continue;
		}

		if (reg[0] == 0 || reg[0] > MAX_GRID_SIZE) {
			dev_err(&spi->dev, "Invalid grid %u at %s\n",
				reg[0], fwnode_get_name(child));
			continue;
		}

		if (reg[1] == 0 || reg[1] > MAX_SEGMENT_NUM) {
			dev_err(&spi->dev, "Invalid segment %u at %s\n",
				reg[1], fwnode_get_name(child));
			continue;
		}

		ret = tm1628_register_led(s, child, reg[0], reg[1], s->leds + num_leds);
		if (ret) {
			dev_err(&spi->dev, "Failed to register LED %s (%d)\n",
				fwnode_get_name(child), ret);
			continue;
		}
		num_leds++;
	}

no_leds:
	ret = device_property_count_u8(&spi->dev, "grid");
	if (ret < 1 || ret > MAX_GRID_SIZE) {
		dev_err(&spi->dev, "Invalid display length (%d)\n", ret);
		return -EINVAL;
	}

	s->grid_size = ret;

	ret = device_property_read_u8_array(&spi->dev, "grid", s->grid, s->grid_size);
	if (ret < 0)
		return ret;

	for (i = 0; i < s->grid_size; i++) {
		if (s->grid[i] < 1 || s->grid[i] > s->grid_size)
			return -EINVAL;
	}

	ret = device_property_read_u8_array(&spi->dev, "segment-mapping", s->segment_mapping, 7);
	if (ret < 0)
		return ret;

	for (i = 0; i < 7; i++) {
		if (s->segment_mapping[i] < 1 || s->segment_mapping[i] > MAX_SEGMENT_NUM)
			return -EINVAL;
	}

	ret = device_create_file(&spi->dev, &dev_attr_display_text);
	if (ret)
		return ret;

	dev_info(&spi->dev, "Configured display with %u digits and %u symbols\n",
		 s->grid_size, num_leds);

	return 0;
}

static int tm1628_spi_remove(struct spi_device *spi)
{
	device_remove_file(&spi->dev, &dev_attr_display_text);
	tm1628_set_display_ctrl(spi, false);
	return 0;
}

static void tm1628_spi_shutdown(struct spi_device *spi)
{
	tm1628_set_display_ctrl(spi, false);
}

static const struct of_device_id tm1628_spi_of_matches[] = {
	{ .compatible = "titanmec,tm1628" },
	{}
};
MODULE_DEVICE_TABLE(of, tm1628_spi_of_matches);

static const struct spi_device_id tm1628_spi_id_table[] = {
	{ "tm1628" },
	{},
};
MODULE_DEVICE_TABLE(spi, tm1628_spi_id_table);

static struct spi_driver tm1628_spi_driver = {
	.probe = tm1628_spi_probe,
	.remove = tm1628_spi_remove,
	.shutdown = tm1628_spi_shutdown,
	.id_table = tm1628_spi_id_table,

	.driver = {
		.name = "tm1628",
		.of_match_table = tm1628_spi_of_matches,
	},
};
module_spi_driver(tm1628_spi_driver);

MODULE_DESCRIPTION("TM1628 LED controller driver");
MODULE_AUTHOR("Andreas Färber");
MODULE_LICENSE("GPL");
