// SPDX-License-Identifier: GPL-2.0
/* Experimental ACPI integration for the MateBook E 2023 S5K3L6 sensor. */
#include <linux/acpi.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/unaligned.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

#include "matebook-sensor-tables.h"

#define MB_CODE MEDIA_BUS_FMT_SGRBG10_1X10
#define MB_LLP 4896
#define MB_FLL 3253

struct mb_mode {
	u32 width, height;
	struct v4l2_rect crop;
	const struct matebook_reg *regs;
	unsigned int count;
};

static const struct mb_mode modes[] = {
	{ 2072, 1552, { 40, 16, 4144, 3104 }, matebook_s5k3l6_binned,
	  ARRAY_SIZE(matebook_s5k3l6_binned) },
	{ 4208, 3120, { 8, 8, 4208, 3120 }, matebook_s5k3l6_full,
	  ARRAY_SIZE(matebook_s5k3l6_full) },
};
static const s64 link_frequencies[] = { 569600000 };

struct mb_sensor {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler controls;
	struct v4l2_ctrl *exposure, *vblank, *hblank;
	struct mutex mutex;
	struct clk *clock;
	struct regulator *supply;
	struct gpio_desc *reset;
	const struct mb_mode *mode;
	bool streaming;
};

static struct mb_sensor *to_mb(struct v4l2_subdev *sd)
{
	return container_of(sd, struct mb_sensor, sd);
}

static int mb_write(struct mb_sensor *sensor, u16 reg, u16 val, u8 size)
{
	struct i2c_client *client = v4l2_get_subdevdata(&sensor->sd);
	u8 buffer[4];
	int ret;

	put_unaligned_be16(reg, buffer);
	if (size == 1)
		buffer[2] = val;
	else
		put_unaligned_be16(val, buffer + 2);
	ret = i2c_master_send(client, buffer, size + 2);
	return ret == size + 2 ? 0 : (ret < 0 ? ret : -EIO);
}

static int mb_write_table(struct mb_sensor *sensor,
			  const struct matebook_reg *regs, unsigned int count)
{
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = mb_write(sensor, regs[i].address, regs[i].value, regs[i].size);
		if (ret)
			return ret;
		if (regs[i].delay)
			usleep_range(regs[i].delay, regs[i].delay + 100);
	}
	return 0;
}

static int mb_power_on(struct device *dev)
{
	struct mb_sensor *sensor = to_mb(i2c_get_clientdata(to_i2c_client(dev)));
	int ret;

	ret = regulator_enable(sensor->supply);
	if (ret)
		return ret;
	ret = clk_prepare_enable(sensor->clock);
	if (ret) {
		regulator_disable(sensor->supply);
		return ret;
	}
	usleep_range(1000, 1500);
	gpiod_set_value_cansleep(sensor->reset, 0);
	usleep_range(10000, 11000);
	return 0;
}

static int mb_power_off(struct device *dev)
{
	struct mb_sensor *sensor = to_mb(i2c_get_clientdata(to_i2c_client(dev)));

	gpiod_set_value_cansleep(sensor->reset, 1);
	clk_disable_unprepare(sensor->clock);
	return regulator_disable(sensor->supply);
}

static int mb_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mb_sensor *sensor = container_of(ctrl->handler, struct mb_sensor, controls);
	struct i2c_client *client = v4l2_get_subdevdata(&sensor->sd);
	struct device *dev = &client->dev;
	int ret;

	if (ctrl->id == V4L2_CID_VBLANK)
		__v4l2_ctrl_modify_range(sensor->exposure, 4,
			sensor->mode->height + ctrl->val - 8, 1,
			min_t(int, 1600, sensor->mode->height + ctrl->val - 8));
	ret = pm_runtime_get_if_in_use(dev);
	if (ret <= 0)
		return ret < 0 && ret != -EAGAIN ? ret : 0;
	ret = 0;
	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = mb_write(sensor, 0x0202, ctrl->val, 2);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = mb_write(sensor, 0x0204, ctrl->val, 2);
		break;
	case V4L2_CID_VBLANK:
		ret = mb_write(sensor, 0x0340, sensor->mode->height + ctrl->val, 2);
		break;
	}
	pm_runtime_put(dev);
	return ret;
}

static const struct v4l2_ctrl_ops mb_ctrl_ops = { .s_ctrl = mb_set_ctrl };

static int mb_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct mb_sensor *sensor = to_mb(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret = 0;

	mutex_lock(&sensor->mutex);
	if (sensor->streaming == !!enable)
		goto out;
	if (!enable) {
		ret = mb_write(sensor, 0x0100, 0, 2);
		usleep_range(35000, 36000);
		pm_runtime_put(&client->dev);
		sensor->streaming = false;
		goto out;
	}
	ret = pm_runtime_resume_and_get(&client->dev);
	if (ret)
		goto out;
	ret = mb_write(sensor, 0x0100, 0, 2);
	if (!ret)
		ret = mb_write(sensor, 0x0000, 0x0060, 2);
	if (!ret)
		ret = mb_write(sensor, 0x0000, 0x30c6, 2);
	usleep_range(10000, 11000);
	if (!ret)
		ret = mb_write(sensor, 0x0a02, 0x3400, 2);
	if (!ret)
		ret = mb_write(sensor, 0x3242, 0x2020, 2);
	if (!ret)
		ret = mb_write(sensor, 0x0114, 0x0330, 2);
	if (!ret)
		ret = mb_write(sensor, 0x0110, 0x0002, 2);
	if (!ret)
		ret = mb_write(sensor, 0x0300, 0x0006, 2);
	if (!ret)
		ret = mb_write_table(sensor, matebook_s5k3l6_init,
				     ARRAY_SIZE(matebook_s5k3l6_init));
	if (!ret)
		ret = mb_write_table(sensor, sensor->mode->regs, sensor->mode->count);
	if (!ret)
		ret = __v4l2_ctrl_handler_setup(&sensor->controls);
	if (!ret)
		ret = mb_write(sensor, 0x3c1e, 0x0100, 2);
	if (!ret)
		ret = mb_write(sensor, 0x0100, 0x0100, 2);
	if (!ret)
		ret = mb_write(sensor, 0x3c1e, 0, 2);
	if (ret) {
		mb_write(sensor, 0x0100, 0, 2);
		pm_runtime_put(&client->dev);
	} else {
		sensor->streaming = true;
	}
out:
	mutex_unlock(&sensor->mutex);
	return ret;
}

static void mb_format(struct v4l2_mbus_framefmt *fmt, const struct mb_mode *mode)
{
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->code = MB_CODE;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
}

static int mb_get_format(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			 struct v4l2_subdev_format *format)
{
	struct mb_sensor *sensor = to_mb(sd);

	if (format->pad)
		return -EINVAL;
	mutex_lock(&sensor->mutex);
	if (format->which == V4L2_SUBDEV_FORMAT_TRY)
		format->format = *v4l2_subdev_state_get_format(state, 0);
	else
		mb_format(&format->format, sensor->mode);
	mutex_unlock(&sensor->mutex);
	return 0;
}

static int mb_set_format(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			 struct v4l2_subdev_format *format)
{
	struct mb_sensor *sensor = to_mb(sd);
	const struct mb_mode *mode;

	if (format->pad)
		return -EINVAL;
	mode = v4l2_find_nearest_size(modes, ARRAY_SIZE(modes), width, height,
				     format->format.width, format->format.height);
	mb_format(&format->format, mode);
	mutex_lock(&sensor->mutex);
	if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
		*v4l2_subdev_state_get_format(state, 0) = format->format;
		*v4l2_subdev_state_get_crop(state, 0) = mode->crop;
	} else if (sensor->streaming) {
		mutex_unlock(&sensor->mutex);
		return -EBUSY;
	} else {
		sensor->mode = mode;
		__v4l2_ctrl_modify_range(sensor->hblank, MB_LLP - mode->width,
					 MB_LLP - mode->width, 1, MB_LLP - mode->width);
		__v4l2_ctrl_modify_range(sensor->vblank, MB_FLL - mode->height,
					 0xffff - mode->height, 1, MB_FLL - mode->height);
		__v4l2_ctrl_s_ctrl(sensor->vblank, MB_FLL - mode->height);
	}
	mutex_unlock(&sensor->mutex);
	return 0;
}

static int mb_enum_code(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad || code->index)
		return -EINVAL;
	code->code = MB_CODE;
	return 0;
}

static int mb_enum_size(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			struct v4l2_subdev_frame_size_enum *size)
{
	if (size->pad || size->index >= ARRAY_SIZE(modes) || size->code != MB_CODE)
		return -EINVAL;
	size->min_width = size->max_width = modes[size->index].width;
	size->min_height = size->max_height = modes[size->index].height;
	return 0;
}

static int mb_selection(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			struct v4l2_subdev_selection *selection)
{
	struct mb_sensor *sensor = to_mb(sd);

	if (selection->pad)
		return -EINVAL;
	switch (selection->target) {
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		selection->r = (struct v4l2_rect) { 0, 0, 4224, 3136 };
		break;
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
		selection->r = sensor->mode->crop;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int mb_init_state(struct v4l2_subdev *sd, struct v4l2_subdev_state *state)
{
	mb_format(v4l2_subdev_state_get_format(state, 0), &modes[0]);
	*v4l2_subdev_state_get_crop(state, 0) = modes[0].crop;
	return 0;
}

static const struct v4l2_subdev_video_ops video_ops = { .s_stream = mb_set_stream };
static const struct v4l2_subdev_pad_ops pad_ops = {
	.enum_mbus_code = mb_enum_code, .enum_frame_size = mb_enum_size,
	.get_fmt = mb_get_format, .set_fmt = mb_set_format, .get_selection = mb_selection,
};
static const struct v4l2_subdev_ops subdev_ops = { .video = &video_ops, .pad = &pad_ops };
static const struct v4l2_subdev_internal_ops internal_ops = { .init_state = mb_init_state };
static const struct media_entity_operations entity_ops = { .link_validate = v4l2_subdev_link_validate };

static int mb_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct mb_sensor *sensor;
	struct v4l2_fwnode_device_properties props;
	u8 address[2] = { 0, 0 }, value[2];
	struct i2c_msg messages[] = {
		{ .addr = client->addr, .len = 2, .buf = address },
		{ .addr = client->addr, .flags = I2C_M_RD, .len = 2, .buf = value },
	};
	struct v4l2_ctrl *ctrl;
	int ret;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;
	sensor->mode = &modes[0];
	sensor->clock = devm_clk_get(dev, NULL);
	if (IS_ERR(sensor->clock))
		return dev_err_probe(dev, PTR_ERR(sensor->clock), "clock unavailable\n");
	sensor->supply = devm_regulator_get(dev, "avdd");
	if (IS_ERR(sensor->supply))
		return dev_err_probe(dev, PTR_ERR(sensor->supply), "supply unavailable\n");
	sensor->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset))
		return dev_err_probe(dev, PTR_ERR(sensor->reset), "reset unavailable\n");
	mutex_init(&sensor->mutex);
	v4l2_i2c_subdev_init(&sensor->sd, client, &subdev_ops);
	sensor->sd.internal_ops = &internal_ops;
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sensor->sd.entity.ops = &entity_ops;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = mb_power_on(dev);
	if (ret)
		goto destroy_mutex;
	ret = i2c_transfer(client->adapter, messages, ARRAY_SIZE(messages));
	if (ret != ARRAY_SIZE(messages) || get_unaligned_be16(value) != 0x30c6) {
		dev_err(dev, "S5K3L6 identification failed (%d, %04x)\n", ret,
			ret == ARRAY_SIZE(messages) ? get_unaligned_be16(value) : 0);
		ret = -ENODEV;
		goto power_off;
	}
	dev_info(dev, "S5K3L6 30c6, input clock %lu\n", clk_get_rate(sensor->clock));
	ret = v4l2_ctrl_handler_init(&sensor->controls, 8);
	if (ret)
		goto power_off;
	sensor->controls.lock = &sensor->mutex;
	ctrl = v4l2_ctrl_new_int_menu(&sensor->controls, NULL, V4L2_CID_LINK_FREQ,
				     0, 0, link_frequencies);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	v4l2_ctrl_new_std(&sensor->controls, NULL, V4L2_CID_PIXEL_RATE,
			  477866666, 477866666, 1, 477866666);
	sensor->hblank = v4l2_ctrl_new_std(&sensor->controls, NULL, V4L2_CID_HBLANK,
		MB_LLP - sensor->mode->width, MB_LLP - sensor->mode->width, 1,
		MB_LLP - sensor->mode->width);
	if (sensor->hblank)
		sensor->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	sensor->vblank = v4l2_ctrl_new_std(&sensor->controls, &mb_ctrl_ops, V4L2_CID_VBLANK,
		MB_FLL - sensor->mode->height, 0xffff - sensor->mode->height, 1,
		MB_FLL - sensor->mode->height);
	sensor->exposure = v4l2_ctrl_new_std(&sensor->controls, &mb_ctrl_ops,
		V4L2_CID_EXPOSURE, 4, MB_FLL - 8, 1, 1600);
	v4l2_ctrl_new_std(&sensor->controls, &mb_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  32, 512, 1, 32);
	ret = sensor->controls.error;
	if (!ret)
		ret = v4l2_fwnode_device_parse(dev, &props);
	if (!ret)
		ret = v4l2_ctrl_new_fwnode_properties(&sensor->controls, NULL, &props);
	if (ret)
		goto free_controls;
	sensor->sd.ctrl_handler = &sensor->controls;
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret)
		goto free_controls;
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	ret = v4l2_async_register_subdev_sensor(&sensor->sd);
	if (ret)
		goto free_entity;
	pm_runtime_idle(dev);
	return 0;
free_entity:
	pm_runtime_disable(dev);
	media_entity_cleanup(&sensor->sd.entity);
free_controls:
	v4l2_ctrl_handler_free(&sensor->controls);
power_off:
	mb_power_off(dev);
destroy_mutex:
	mutex_destroy(&sensor->mutex);
	return ret;
}

static void mb_remove(struct i2c_client *client)
{
	struct mb_sensor *sensor = to_mb(i2c_get_clientdata(client));

	v4l2_async_unregister_subdev(&sensor->sd);
	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		mb_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
	media_entity_cleanup(&sensor->sd.entity);
	v4l2_ctrl_handler_free(&sensor->controls);
	mutex_destroy(&sensor->mutex);
}

static const struct dev_pm_ops pm_ops = {
	SET_RUNTIME_PM_OPS(mb_power_off, mb_power_on, NULL)
};
static const struct acpi_device_id acpi_ids[] = { { "SSE5364" }, {} };
MODULE_DEVICE_TABLE(acpi, acpi_ids);
static struct i2c_driver mb_driver = {
	.driver = { .name = "s5k3l6", .acpi_match_table = acpi_ids, .pm = &pm_ops },
	.probe = mb_probe, .remove = mb_remove,
};
module_i2c_driver(mb_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Experimental MateBook E 2023 S5K3L6 camera driver");
