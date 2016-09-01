/*
 * Allwinner q8 formfactor tablet hardware manager
 *
 * Copyright (C) 2016 Hans de Goede <hdegoede@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <asm/unaligned.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

/*
 * We can detect which touchscreen controller is used automatically,
 * but some controllers can be wired up differently depending on the
 * q8 PCB variant used, so they need different firmware files / settings.
 *
 * We allow the user to specify a firmware_variant to select a config
 * from a list of known configs. We also allow overriding each setting
 * individually.
 */

static int touchscreen_variant = -1;
module_param(touchscreen_variant, int, 0444);
MODULE_PARM_DESC(touchscreen_variant, "Touchscreen variant 0-x, -1 for auto");

static int touchscreen_width = -1;
module_param(touchscreen_width, int, 0444);
MODULE_PARM_DESC(touchscreen_width, "Touchscreen width, -1 for auto");

static int touchscreen_height = -1;
module_param(touchscreen_height, int, 0444);
MODULE_PARM_DESC(touchscreen_height, "Touchscreen height, -1 for auto");

static int touchscreen_invert_x = -1;
module_param(touchscreen_invert_x, int, 0444);
MODULE_PARM_DESC(touchscreen_invert_x, "Touchscreen invert x, -1 for auto");

static int touchscreen_invert_y = -1;
module_param(touchscreen_invert_y, int, 0444);
MODULE_PARM_DESC(touchscreen_invert_y, "Touchscreen invert y, -1 for auto");

static int touchscreen_swap_x_y = -1;
module_param(touchscreen_swap_x_y, int, 0444);
MODULE_PARM_DESC(touchscreen_swap_x_y, "Touchscreen swap x y, -1 for auto");

static char *touchscreen_fw_name;
module_param(touchscreen_fw_name, charp, 0444);
MODULE_PARM_DESC(touchscreen_fw_name, "Touchscreen firmware filename");

enum soc {
	a13,
	a23,
	a33,
};

#define TOUCHSCREEN_POWER_ON_DELAY	20
#define SILEAD_REG_ID			0xFC
#define EKTF2127_RESPONSE		0x52
#define EKTF2127_REQUEST		0x53
#define EKTF2127_WIDTH			0x63

enum {
	touchscreen_unknown,
	gsl1680_a082,
	gsl1680_b482,
	ektf2127,
	zet6251,
};

struct q8_hardwaremgr_device {
	int model;
	int addr;
	const char *compatible;
	bool delete_regulator;
};

struct q8_hardwaremgr_data {
	struct device *dev;
	enum soc soc;
	struct q8_hardwaremgr_device touchscreen;
	int touchscreen_variant;
	int touchscreen_width;
	int touchscreen_height;
	int touchscreen_invert_x;
	int touchscreen_invert_y;
	int touchscreen_swap_x_y;
	const char *touchscreen_fw_name;
};

typedef int (*bus_probe_func)(struct q8_hardwaremgr_data *data,
			      struct i2c_adapter *adap);
typedef int (*client_probe_func)(struct q8_hardwaremgr_data *data,
				 struct i2c_client *client);

static struct device_node *q8_hardware_mgr_apply_common(
	struct q8_hardwaremgr_device *dev, struct of_changeset *cset,
	const char *prefix)
{
	struct device_node *np;

	np = of_find_node_by_name(of_root, prefix);
	/* Never happens already checked in q8_hardwaremgr_do_probe() */
	if (WARN_ON(!np))
		return NULL;

	of_changeset_init(cset);
	of_changeset_add_property_u32(cset, np, "reg", dev->addr);
	of_changeset_add_property_string(cset, np, "compatible",
					 dev->compatible);
	of_changeset_update_property_string(cset, np, "status", "okay");

	if (dev->delete_regulator) {
		struct property *p;

		p = of_find_property(np, "vddio-supply", NULL);
		/* Never happens already checked in q8_hardwaremgr_do_probe() */
		if (WARN_ON(!p))
			return np;

		of_changeset_remove_property(cset, np, p);
	}

	return np; /* Allow the caller to make further changes */
}

static int q8_hardwaremgr_probe_client(struct q8_hardwaremgr_data *data,
				       struct q8_hardwaremgr_device *dev,
				       struct i2c_adapter *adap, u16 addr,
				       client_probe_func client_probe)
{
	struct i2c_client *client;
	int ret;

	client = i2c_new_dummy(adap, addr);
	if (!client)
		return -ENOMEM;

	/* ret will be one of 0: Success, -ETIMEDOUT: Bus stuck or -ENODEV */
	ret = client_probe(data, client);
	if (ret == 0)
		dev->addr = addr;

	i2c_unregister_device(client);

	return ret;
}

#define PROBE_CLIENT(dev, addr, probe) \
{ \
	int ret = q8_hardwaremgr_probe_client(data, dev, adap, addr, probe); \
	if (ret != -ENODEV) \
		return ret; \
}

static int q8_hardwaremgr_probe_silead(struct q8_hardwaremgr_data *data,
				       struct i2c_client *client)
{
	__le32 chip_id;
	int ret;

	ret = i2c_smbus_read_i2c_block_data(client, SILEAD_REG_ID,
					    sizeof(chip_id), (u8 *)&chip_id);
	if (ret != sizeof(chip_id))
		return ret == -ETIMEDOUT ? -ETIMEDOUT : -ENODEV;

	switch (le32_to_cpu(chip_id)) {
	case 0xa0820000:
		data->touchscreen.compatible = "silead,gsl1680";
		data->touchscreen.model = gsl1680_a082;
		dev_info(data->dev, "Silead touchscreen ID: 0xa0820000\n");
		return 0;
	case 0xb4820000:
		data->touchscreen.compatible = "silead,gsl1680";
		data->touchscreen.model = gsl1680_b482;
		dev_info(data->dev, "Silead touchscreen ID: 0xb4820000\n");
		return 0;
	default:
		dev_warn(data->dev, "Silead? touchscreen with unknown ID: 0x%08x\n",
			 le32_to_cpu(chip_id));
	}

	return -ENODEV;
}

static int q8_hardwaremgr_probe_ektf2127(struct q8_hardwaremgr_data *data,
					 struct i2c_client *client)
{
	unsigned char buff[4];
	int ret;

	/* Read hello, ignore data, depends on initial power state */
	ret = i2c_master_recv(client, buff, 4);
	if (ret != 4)
		return ret == -ETIMEDOUT ? -ETIMEDOUT : -ENODEV;

	/* Request width */
	buff[0] = EKTF2127_REQUEST;
	buff[1] = EKTF2127_WIDTH;
	buff[2] = 0x00;
	buff[3] = 0x00;
	ret = i2c_master_send(client, buff, 4);
	if (ret != 4)
		return ret == -ETIMEDOUT ? -ETIMEDOUT : -ENODEV;

	msleep(20);

	/* Read response */
	ret = i2c_master_recv(client, buff, 4);
	if (ret != 4)
		return ret == -ETIMEDOUT ? -ETIMEDOUT : -ENODEV;

	if (buff[0] == EKTF2127_RESPONSE && buff[1] == EKTF2127_WIDTH) {
		data->touchscreen.compatible = "elan,ektf2127";
		data->touchscreen.model = ektf2127;
		return 0;
	}

	return -ENODEV;
}

static int q8_hardwaremgr_probe_zet6251(struct q8_hardwaremgr_data *data,
					struct i2c_client *client)
{
	unsigned char buff[4];
	int ret;

	/*
	 * We only do a simple read finger data packet test, because some
	 * versions require firmware to be loaded. If no firmware is loaded
	 * the buffer will be filed with 0xff, so we ignore the contents.
	 */
	ret = i2c_master_recv(client, buff, 24);
	if (ret != 24)
		return ret == -ETIMEDOUT ? -ETIMEDOUT : -ENODEV;

	data->touchscreen.compatible = "zeitec,zet6251";
	data->touchscreen.model = zet6251;
	return 0;
}

static int q8_hardwaremgr_probe_touchscreen(struct q8_hardwaremgr_data *data,
					    struct i2c_adapter *adap)
{
	msleep(TOUCHSCREEN_POWER_ON_DELAY);

	PROBE_CLIENT(&data->touchscreen, 0x40, q8_hardwaremgr_probe_silead);
	PROBE_CLIENT(&data->touchscreen, 0x15, q8_hardwaremgr_probe_ektf2127);
	PROBE_CLIENT(&data->touchscreen, 0x76, q8_hardwaremgr_probe_zet6251);

	return -ENODEV;
}

static void q8_hardwaremgr_apply_gsl1680_a082_variant(
	struct q8_hardwaremgr_data *data)
{
	if (touchscreen_variant != -1)
		data->touchscreen_variant = touchscreen_variant;

	switch (data->touchscreen_variant) {
	default:
		dev_warn(data->dev, "Error unknown touchscreen_variant %d using 0\n",
			 touchscreen_variant);
		/* Fall through */
	case 0:
		data->touchscreen_width = 1024;
		data->touchscreen_height = 600;
		data->touchscreen_fw_name = "gsl1680-a082-q8-700.fw";
		break;
	case 1:
		data->touchscreen_width = 480;
		data->touchscreen_height = 800;
		data->touchscreen_swap_x_y = 1;
		data->touchscreen_fw_name = "gsl1680-a082-q8-a70.fw";
		break;
	}
}

static void q8_hardwaremgr_apply_gsl1680_b482_variant(
	struct q8_hardwaremgr_data *data)
{
	if (touchscreen_variant != -1)
		data->touchscreen_variant = touchscreen_variant;

	switch (data->touchscreen_variant) {
	default:
		dev_warn(data->dev, "Error unknown touchscreen_variant %d using 0\n",
			 touchscreen_variant);
		/* Fall through */
	case 0:
		data->touchscreen_width = 960;
		data->touchscreen_height = 640;
		data->touchscreen_fw_name = "gsl1680-b482-q8-d702.fw";
		break;
	case 1:
		data->touchscreen_width = 960;
		data->touchscreen_height = 640;
		data->touchscreen_fw_name = "gsl1680-b482-q8-a70.fw";
		break;
	}
}

static void q8_hardwaremgr_issue_gsl1680_warning(
	struct q8_hardwaremgr_data *data)
{
	dev_warn(data->dev, "gsl1680 touchscreen may require kernel cmdline parameters to function properly\n");
	dev_warn(data->dev, "Try q8_hardwaremgr.touchscreen_invert_x=%d if x coordinates are inverted\n",
		 !data->touchscreen_invert_x);
	dev_warn(data->dev, "Try q8_hardwaremgr.touchscreen_variant=%d if coordinates are all over the place\n",
		 !data->touchscreen_variant);

#define	show(x) \
	dev_info(data->dev, #x " %d (%s)\n", data->x, \
		 (x == -1) ? "auto" : "user supplied")

	show(touchscreen_variant);
	show(touchscreen_width);
	show(touchscreen_height);
	show(touchscreen_invert_x);
	show(touchscreen_invert_y);
	show(touchscreen_swap_x_y);
	dev_info(data->dev, "touchscreen_fw_name %s (%s)\n",
		 data->touchscreen_fw_name,
		 (touchscreen_fw_name == NULL) ? "auto" : "user supplied");
#undef show
}

static void q8_hardwaremgr_apply_touchscreen(struct q8_hardwaremgr_data *data)
{
	struct of_changeset cset;
	struct device_node *np;

	switch (data->touchscreen.model) {
	case touchscreen_unknown:
		return;
	case gsl1680_a082:
		q8_hardwaremgr_apply_gsl1680_a082_variant(data);
		break;
	case gsl1680_b482:
		q8_hardwaremgr_apply_gsl1680_b482_variant(data);
		break;
	case ektf2127:
	case zet6251:
		/* These have only 1 variant */
		break;
	}

	if (touchscreen_width != -1)
		data->touchscreen_width = touchscreen_width;

	if (touchscreen_height != -1)
		data->touchscreen_height = touchscreen_height;

	if (touchscreen_invert_x != -1)
		data->touchscreen_invert_x = touchscreen_invert_x;

	if (touchscreen_invert_y != -1)
		data->touchscreen_invert_y = touchscreen_invert_y;

	if (touchscreen_swap_x_y != -1)
		data->touchscreen_swap_x_y = touchscreen_swap_x_y;

	if (touchscreen_fw_name)
		data->touchscreen_fw_name = touchscreen_fw_name;

	if (data->touchscreen.model == gsl1680_a082 ||
	    data->touchscreen.model == gsl1680_b482)
		q8_hardwaremgr_issue_gsl1680_warning(data);

	np = q8_hardware_mgr_apply_common(&data->touchscreen, &cset,
					  "touchscreen");
	if (!np)
		return;

	if (data->touchscreen_width)
		of_changeset_add_property_u32(&cset, np, "touchscreen-size-x",
					      data->touchscreen_width);
	if (data->touchscreen_height)
		of_changeset_add_property_u32(&cset, np, "touchscreen-size-y",
					      data->touchscreen_height);
	if (data->touchscreen_invert_x)
		of_changeset_add_property_bool(&cset, np,
					       "touchscreen-inverted-x");
	if (data->touchscreen_invert_y)
		of_changeset_add_property_bool(&cset, np,
					       "touchscreen-inverted-y");
	if (data->touchscreen_swap_x_y)
		of_changeset_add_property_bool(&cset, np,
					       "touchscreen-swapped-x-y");
	if (data->touchscreen_fw_name)
		of_changeset_add_property_string(&cset, np, "firmware-name",
						 data->touchscreen_fw_name);

	of_changeset_apply(&cset);

	of_node_put(np);
}

static int q8_hardwaremgr_do_probe(struct q8_hardwaremgr_data *data,
				   struct q8_hardwaremgr_device *dev,
				   const char *prefix, bus_probe_func func)
{
	struct device_node *np;
	struct i2c_adapter *adap;
	struct regulator *reg;
	struct gpio_desc *gpio;
	int ret = 0;

	np = of_find_node_by_name(of_root, prefix);
	if (!np) {
		dev_err(data->dev, "Error %s node is missing\n", prefix);
		return -EINVAL;
	}

	/*
	 * Patch the dt_node into our device since there is no device for
	 * the probed hw yet (status = disabled) .
	 */
	data->dev->of_node = np;

	ret = pinctrl_bind_pins(data->dev);
	if (ret)
		goto put_node;

	adap = of_get_i2c_adapter_by_node(np->parent);
	if (!adap) {
		ret = -EPROBE_DEFER;
		goto put_pins;
	}

	reg = regulator_get_optional(data->dev, "vddio");
	if (IS_ERR(reg)) {
		ret = PTR_ERR(reg);
		if (ret == -EPROBE_DEFER)
			goto put_adapter;
		reg = NULL;
	}

	gpio = fwnode_get_named_gpiod(&np->fwnode, "power-gpios");
	if (IS_ERR(gpio)) {
		ret = PTR_ERR(gpio);
		if (ret == -EPROBE_DEFER)
			goto put_reg;
		gpio = NULL;
	}

	/* First try with only the power gpio driven high */
	if (gpio) {
		ret = gpiod_direction_output(gpio, 1);
		if (ret)
			goto put_gpio;
	}

	dev_info(data->dev, "Probing %s without a regulator\n", prefix);
	ret = func(data, adap);
	if (ret != 0 && reg) {
		/* Second try, also enable the regulator */
		ret = regulator_enable(reg);
		if (ret)
			goto restore_gpio;

		dev_info(data->dev, "Probing %s with a regulator\n", prefix);
		ret = func(data, adap);

		regulator_disable(reg);
	} else if (reg)
		dev->delete_regulator = true; /* Regulator not needed */

	if (ret == 0)
		dev_info(data->dev, "Found %s at 0x%02x\n",
			 dev->compatible, dev->addr);
	else
		ret = 0; /* Not finding a device is not an error */

restore_gpio:
	if (gpio)
		gpiod_direction_output(gpio, 0);
put_gpio:
	if (gpio)
		gpiod_put(gpio);
put_reg:
	if (reg)
		regulator_put(reg);
put_adapter:
	i2c_put_adapter(adap);

put_pins:
	/* Undo our manual pinctrl_bind_pins() */
	if (data->dev->pins) {
		devm_pinctrl_put(data->dev->pins->p);
		devm_kfree(data->dev, data->dev->pins);
		data->dev->pins = NULL;
	}

put_node:
	data->dev->of_node = NULL;
	of_node_put(np);

	return ret;
}

static int q8_hardwaremgr_probe(struct platform_device *pdev)
{
	struct q8_hardwaremgr_data *data;
	int ret = 0;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = &pdev->dev;
	data->soc = (long)pdev->dev.platform_data;

	ret = q8_hardwaremgr_do_probe(data, &data->touchscreen, "touchscreen",
				      q8_hardwaremgr_probe_touchscreen);
	if (ret)
		goto error;

	q8_hardwaremgr_apply_touchscreen(data);

error:
	kfree(data);

	return ret;
}

static struct platform_driver q8_hardwaremgr_driver = {
	.driver = {
		.name	= "q8-hwmgr",
	},
	.probe	= q8_hardwaremgr_probe,
};

static int __init q8_hardwaremgr_init(void)
{
	struct platform_device *pdev;
	enum soc soc;
	int ret;

	if (of_machine_is_compatible("allwinner,q8-a13"))
		soc = a13;
	else if (of_machine_is_compatible("allwinner,q8-a23"))
		soc = a23;
	else if (of_machine_is_compatible("allwinner,q8-a33"))
		soc = a33;
	else
		return 0;

	pdev = platform_device_alloc("q8-hwmgr", 0);
	if (!pdev)
		return -ENOMEM;

	pdev->dev.platform_data = (void *)(long)soc;

	ret = platform_device_add(pdev);
	if (ret)
		return ret;

	return platform_driver_register(&q8_hardwaremgr_driver);
}

device_initcall(q8_hardwaremgr_init);

MODULE_DESCRIPTION("Allwinner q8 formfactor tablet hardware manager");
MODULE_AUTHOR("Hans de Goede <hdegoede@redhat.com>");
MODULE_LICENSE("GPL");
