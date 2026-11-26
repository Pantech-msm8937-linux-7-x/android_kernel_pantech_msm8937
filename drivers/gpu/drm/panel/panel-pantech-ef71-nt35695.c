// SPDX-License-Identifier: GPL-2.0-only
/*
 * Panels of the Pantech VEGA IRON 2: the Novatek NT35695 1080p command mode
 * panel of the EF71S and the FocalTech FT8716 1080p video mode panel of the
 * EF71AS.
 *
 * Init sequences, timings and DCS backlight from the EF71 msm-3.18 panel
 * descriptions (qcom,mdss_dsi_nt35695_1080p_cmd_ef71 and
 * qcom,mdss_dsi_ft8716_1080p_video_ef71). Both models share one device tree:
 * as on msm-3.18, the panel is the one the bootloader names in
 * mdss_mdp.panel.
 *
 * Copyright (c) 2026, hiru <rlawoehd187@naver.com>
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

#include <video/mipi_display.h>

#define NT35695_BL_MAX	216
#define FT8716_BL_MAX	250

struct ef71_nt35695;

struct ef71_panel_desc {
	/* in the mdss_mdp.panel name the bootloader passes */
	const char *name;
	const struct drm_display_mode *mode;
	unsigned long mode_flags;
	int bl_max;
	bool bl_long_write;
	void (*reset)(struct ef71_nt35695 *ctx);
	int (*on)(struct ef71_nt35695 *ctx);
	int (*off)(struct ef71_nt35695 *ctx);
	/*
	 * Video mode: the off commands go out while the video still runs,
	 * the DSI host sends commands between frames only
	 */
	bool off_in_disable;
	/*
	 * Display off for a touch controller waiting for a wake gesture
	 * (keep_awake), or NULL when the panel may sleep as usual then: it
	 * only has to stay out of reset.
	 */
	int (*off_awake)(struct ef71_nt35695 *ctx);
};

struct ef71_nt35695 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset_gpio;
	const struct ef71_panel_desc *desc;
};

static inline struct ef71_nt35695 *to_ef71_nt35695(struct drm_panel *panel)
{
	return container_of_const(panel, struct ef71_nt35695, panel);
}

static void ef71_nt35695_reset(struct ef71_nt35695 *ctx)
{
	/* msm-3.18: <1 20>, <0 1>, <1 20> on the active low reset line */
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(20);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(20);
}

static int ef71_nt35695_on(struct ef71_nt35695 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xba, 0x03);
	/* Tearing effect on VBlank only */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x35, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x44, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbb, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x10);
	/* CABC: user interface image */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x55, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x23);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x01, 0x84);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfb, 0x01);
	/* Backlight PWM at 40 kHz */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x08, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x46, 0x45);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x10);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 110);
	/*
	 * Out of the idle mode (8 colours) double tap to wake leaves it in,
	 * which the reset does not clear (see ef71_nt35695_off_awake())
	 */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_EXIT_IDLE_MODE);
	/* Brightness control and backlight on */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x10);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 48);

	return dsi_ctx.accum_err;
}

static int ef71_nt35695_off(struct ef71_nt35695 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	ctx->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 150);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 150);

	return dsi_ctx.accum_err;
}

/*
 * Display off and idle mode, without the sleep in of the off commands: the
 * mXT641T stops sensing as soon as the panel sleeps, so this is what the
 * panel gets while the touch waits for a double tap (keep_awake). Idle mode,
 * 8 colours, lowers what the awake panel draws; the next prepare resets and
 * initialises the panel as usual, leaving idle mode explicitly.
 */
static int ef71_nt35695_off_awake(struct ef71_nt35695 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	ctx->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_ENTER_IDLE_MODE);

	return dsi_ctx.accum_err;
}

static int ef71_nt35695_prepare(struct drm_panel *panel)
{
	struct ef71_nt35695 *ctx = to_ef71_nt35695(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ctx->desc->reset(ctx);

	ret = ctx->desc->on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		return ret;
	}

	return 0;
}

static int ef71_nt35695_disable(struct drm_panel *panel)
{
	struct ef71_nt35695 *ctx = to_ef71_nt35695(panel);
	int ret;

	if (!ctx->desc->off_in_disable)
		return 0;

	ret = ctx->desc->off(ctx);
	if (ret < 0)
		dev_err(&ctx->dsi->dev, "Failed to un-initialize panel: %d\n",
			ret);

	return 0;
}

static int ef71_nt35695_unprepare(struct drm_panel *panel)
{
	struct ef71_nt35695 *ctx = to_ef71_nt35695(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret = 0;

	if (panel->keep_awake && ctx->desc->off_awake)
		ret = ctx->desc->off_awake(ctx);
	else if (!ctx->desc->off_in_disable)
		ret = ctx->desc->off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	/* out of reset, so it keeps driving the touch sensing */
	if (!panel->keep_awake)
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);

	return 0;
}

static const struct drm_display_mode ef71_nt35695_mode = {
	.clock = (1080 + 180 + 10 + 142) * (1920 + 10 + 2 + 4) * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 180,
	.hsync_end = 1080 + 180 + 10,
	.htotal = 1080 + 180 + 10 + 142,
	.vdisplay = 1920,
	.vsync_start = 1920 + 10,
	.vsync_end = 1920 + 10 + 2,
	.vtotal = 1920 + 10 + 2 + 4,
	.width_mm = 59,
	.height_mm = 104,
	.type = DRM_MODE_TYPE_DRIVER,
};

/* FT8716 */
static void ef71_ft8716_reset(struct ef71_nt35695 *ctx)
{
	/* msm-3.18: <1 20>, <0 2>, <1 20>, then 15 ms, on the active low line */
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(20);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(2000, 3000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(20);
	msleep(15);
}

static int ef71_ft8716_on(struct ef71_nt35695 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);
	/* Tearing effect on VBlank only */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_SET_TEAR_ON, 0x00);
	mipi_dsi_msleep(&dsi_ctx, 160);
	/* Brightness control, dimming and backlight on */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x2c);
	/* CABC: user interface image */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_POWER_SAVE, 0x01);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 48);

	return dsi_ctx.accum_err;
}

static int ef71_ft8716_off(struct ef71_nt35695 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	ctx->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	return dsi_ctx.accum_err;
}

static const struct drm_display_mode ef71_ft8716_mode = {
	.clock = (1080 + 32 + 4 + 32) * (1920 + 16 + 2 + 16) * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 32,
	.hsync_end = 1080 + 32 + 4,
	.htotal = 1080 + 32 + 4 + 32,
	.vdisplay = 1920,
	.vsync_start = 1920 + 16,
	.vsync_end = 1920 + 16 + 2,
	.vtotal = 1920 + 16 + 2 + 16,
	.type = DRM_MODE_TYPE_DRIVER,
};

static const struct ef71_panel_desc ef71_nt35695_desc = {
	.name = "nt35695",
	.mode = &ef71_nt35695_mode,
	.mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS,
	.bl_max = NT35695_BL_MAX,
	.reset = ef71_nt35695_reset,
	.on = ef71_nt35695_on,
	.off = ef71_nt35695_off,
	/* the mXT641T stops sensing as soon as this panel sleeps */
	.off_awake = ef71_nt35695_off_awake,
};

static const struct ef71_panel_desc ef71_ft8716_desc = {
	.name = "ft8716",
	.mode = &ef71_ft8716_mode,
	.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
		      MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
		      MIPI_DSI_CLOCK_NON_CONTINUOUS,
	.bl_max = FT8716_BL_MAX,
	.bl_long_write = true,
	.reset = ef71_ft8716_reset,
	.on = ef71_ft8716_on,
	.off = ef71_ft8716_off,
	.off_in_disable = true,
	/* in-cell touch: it may sleep, only its reset and supplies stay up */
};

static int ef71_nt35695_get_modes(struct drm_panel *panel,
				  struct drm_connector *connector)
{
	struct ef71_nt35695 *ctx = to_ef71_nt35695(panel);

	return drm_connector_helper_get_modes_fixed(connector, ctx->desc->mode);
}

static const struct drm_panel_funcs ef71_nt35695_panel_funcs = {
	.prepare = ef71_nt35695_prepare,
	.disable = ef71_nt35695_disable,
	.unprepare = ef71_nt35695_unprepare,
	.get_modes = ef71_nt35695_get_modes,
};

static int ef71_nt35695_bl_update_status(struct backlight_device *bl)
{
	struct ef71_nt35695 *ctx = bl_get_data(bl);
	struct mipi_dsi_device *dsi = ctx->dsi;
	u8 brightness = backlight_get_brightness(bl);
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	if (ctx->desc->bl_long_write) {
		/* msm-3.18: qcom,mdss-dsi-bklt-long-write_type */
		const u8 buf[] = { MIPI_DCS_SET_DISPLAY_BRIGHTNESS, brightness };
		struct mipi_dsi_msg msg = {
			.channel = dsi->channel,
			.type = MIPI_DSI_DCS_LONG_WRITE,
			.tx_buf = buf,
			.tx_len = sizeof(buf),
		};

		/* a DCS long write, which the DCS helpers do not send for one byte */
		ret = dsi->host->ops->transfer(dsi->host, &msg);
	} else {
		/* One parameter byte, not the 16 bit form the DCS helper sends */
		ret = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
					 &brightness, sizeof(brightness));
	}

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return ret < 0 ? ret : 0;
}

static const struct backlight_ops ef71_nt35695_bl_ops = {
	.update_status = ef71_nt35695_bl_update_status,
};

static struct backlight_device *
ef71_nt35695_create_backlight(struct ef71_nt35695 *ctx)
{
	struct device *dev = &ctx->dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 150,
		.max_brightness = ctx->desc->bl_max,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, ctx,
					      &ef71_nt35695_bl_ops, &props);
}

/*
 * The panel fitted: the one whose name is in the mdss_mdp.panel the
 * bootloader passes, the NT35695 of the EF71S otherwise.
 */
static const struct ef71_panel_desc *ef71_panel_fitted(struct device *dev)
{
	const struct ef71_panel_desc *desc = of_device_get_match_data(dev);
	const char *bootargs;

	if (desc)
		return desc;

	if (of_chosen &&
	    !of_property_read_string(of_chosen, "bootargs", &bootargs) &&
	    strstr(bootargs, ef71_ft8716_desc.name))
		return &ef71_ft8716_desc;

	return &ef71_nt35695_desc;
}

static int ef71_nt35695_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct ef71_nt35695 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct ef71_nt35695, panel,
				   &ef71_nt35695_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	ctx->desc = ef71_panel_fitted(dev);
	mipi_dsi_set_drvdata(dsi, ctx);
	dev_info(dev, "%s panel\n", ctx->desc->name);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = ctx->desc->mode_flags;

	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = ef71_nt35695_create_backlight(ctx);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	return 0;
}

static void ef71_nt35695_remove(struct mipi_dsi_device *dsi)
{
	struct ef71_nt35695 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id ef71_nt35695_of_match[] = {
	/* the one the bootloader names */
	{ .compatible = "pantech,ef71-panel" },
	{ .compatible = "pantech,ef71-nt35695", .data = &ef71_nt35695_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ef71_nt35695_of_match);

static struct mipi_dsi_driver ef71_nt35695_driver = {
	.probe = ef71_nt35695_probe,
	.remove = ef71_nt35695_remove,
	.driver = {
		.name = "panel-pantech-ef71-nt35695",
		.of_match_table = ef71_nt35695_of_match,
	},
};
module_mipi_dsi_driver(ef71_nt35695_driver);

MODULE_DESCRIPTION("DRM driver for the Pantech EF71 NT35695 and FT8716 panels");
MODULE_LICENSE("GPL");
