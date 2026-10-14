// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2025 iWave Global Pvt. Ltd.
 *
 */

/*#include <common.h>*/
#include <dm.h>
#include <dm/device_compat.h>
#include <mipi_dsi.h>
#include <panel.h>
#include <asm/gpio.h>
#include <linux/err.h>
#include <linux/delay.h>

#define CMD_TABLE_LEN 2
typedef u8 cmd_set_table[CMD_TABLE_LEN];

/* Write Manufacture Command Set Control */
#define WRMAUCCTR 0xFE

struct winstar_panel_priv {
	struct gpio_desc reset;
	unsigned int lanes;
	enum mipi_dsi_pixel_format format;
	unsigned long mode_flags;
};

struct rad_platform_data {
	int (*enable)(struct udevice *dev);
};

/* Manufacturer Command Set pages (CMD2) */
static const cmd_set_table mcs_winstar[] = {
        {0x80, 0x5B},
        {0x81, 0x47},
        {0x82, 0x84},
        {0x83, 0x88},
        {0x84, 0x88},
        {0x85, 0x23},
        {0x86, 0xB6},

};

static const struct display_timing default_timing = {
	.pixelclock.typ		= 51000000,
	.hactive.typ		= 1024,
	.hfront_porch.typ	= 160,
	.hback_porch.typ	= 10,
	.hsync_len.typ		= 2,
	.vactive.typ		= 600,
	.vfront_porch.typ	= 1,
	.vback_porch.typ	= 12,
	.vsync_len.typ		= 2,
	.flags = DISPLAY_FLAGS_HSYNC_LOW |
		 DISPLAY_FLAGS_VSYNC_LOW |
		 DISPLAY_FLAGS_DE_LOW |
		 DISPLAY_FLAGS_PIXDATA_NEGEDGE,
};


static u8 color_format_from_dsi_format(enum mipi_dsi_pixel_format format)
{
	switch (format) {
	case MIPI_DSI_FMT_RGB565:
		return 0x55;
	case MIPI_DSI_FMT_RGB666:
	case MIPI_DSI_FMT_RGB666_PACKED:
		return 0x66;
	case MIPI_DSI_FMT_RGB888:
		return 0x77;
	default:
		return 0x77; /* for backward compatibility */
	}
};

static int rad_panel_push_cmd_list(struct mipi_dsi_device *device,
				   const cmd_set_table *cmd_set,
				   size_t count)
{
	size_t i;
	const cmd_set_table *cmd;
	int ret = 0;

	for (i = 0; i < count; i++) {
		cmd = cmd_set++;
		ret = mipi_dsi_generic_write(device, cmd, CMD_TABLE_LEN);
		if (ret < 0)
			return ret;
	}

	return ret;
};

static int winstar_enable(struct udevice *dev)
{
	struct winstar_panel_priv *priv = dev_get_priv(dev);
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dev);
	struct mipi_dsi_device *dsi = plat->device;
	u8 color_format = color_format_from_dsi_format(priv->format);
	u16 brightness;
	int ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	ret = rad_panel_push_cmd_list(dsi, &mcs_winstar[0],
		sizeof(mcs_winstar) / CMD_TABLE_LEN);
	if (ret < 0) {
		printf("Failed to send MCS (%d)\n", ret);
		return -EIO;
	}

	/* Select User Command Set table (CMD1) */
	ret = mipi_dsi_generic_write(dsi, (u8[]){ WRMAUCCTR, 0x00 }, 2);
	if (ret < 0)
		return -EIO;

	/* Software reset */
	ret = mipi_dsi_dcs_soft_reset(dsi);
	if (ret < 0) {
		printf("Failed to do Software Reset (%d)\n", ret);
		return -EIO;
	}

	/* Wait 80ms for panel out of reset */
	mdelay(80);
#if 0
        /*IWG50M: MIPI DSI: Display Command Set to rotate the display*/
        ret = mipi_dsi_generic_write(dsi, (u8[]){ 0x36, 0xa2 }, 2);
        if (ret < 0) {
                printf("Failed to rotate display (%d)\n", ret);
                return -EIO;
        }
#endif

	/* Set DSI mode */
	ret = mipi_dsi_generic_write(dsi, (u8[]){ 0xC2, 0x08 }, 2);
	if (ret < 0) {
		printf("Failed to set DSI mode (%d)\n", ret);
		return -EIO;
	}

	/* Set tear ON */
	ret = mipi_dsi_dcs_set_tear_on(dsi, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	if (ret < 0) {
		printf("Failed to set tear ON (%d)\n", ret);
		return -EIO;
	}

	/* Set tear scanline */
	ret = mipi_dsi_dcs_set_tear_scanline(dsi, 0x380);
	if (ret < 0) {
		printf("Failed to set tear scanline (%d)\n", ret);
		return -EIO;
	}

	/* Set pixel format */
	ret = mipi_dsi_dcs_set_pixel_format(dsi, color_format);
	if (ret < 0) {
		printf("Failed to set pixel format (%d)\n", ret);
		return -EIO;
	}


	/* Set display brightness */
	brightness = 255; /* Max brightness */
	ret = mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, &brightness, 2);
	if (ret < 0) {
		printf("Failed to set display brightness (%d)\n",
				  ret);
		return -EIO;
	}

	/* Exit sleep mode */
	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0) {
		printf("Failed to exit sleep mode (%d)\n", ret);
		return -EIO;
	}

	mdelay(5);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0) {
		printf("Failed to set display ON (%d)\n", ret);
		return -EIO;
	}

	return 0;
}

static int winstar_panel_enable_backlight(struct udevice *dev)
{
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dev);
	struct rad_platform_data *data = (struct rad_platform_data *)dev_get_driver_data(dev);
	struct mipi_dsi_device *device = plat->device;
	int ret;

	ret = mipi_dsi_attach(device);
	if (ret < 0)
		return ret;

	return data->enable(dev);
}

static int winstar_panel_get_display_timing(struct udevice *dev,
					    struct display_timing *timings)
{
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dev);
	struct mipi_dsi_device *device = plat->device;
	struct winstar_panel_priv *priv = dev_get_priv(dev);

	memcpy(timings, &default_timing, sizeof(*timings));

	/* fill characteristics of DSI data link */
	if (device) {
		device->lanes = priv->lanes;
		device->format = priv->format;
		device->mode_flags = priv->mode_flags;
	}

	return 0;
}

static int winstar_panel_probe(struct udevice *dev)
{
	struct winstar_panel_priv *priv = dev_get_priv(dev);
	int ret;
	u32 video_mode;
	priv->format = MIPI_DSI_FMT_RGB888;
	priv->mode_flags = MIPI_DSI_MODE_VIDEO_HSE | MIPI_DSI_MODE_VIDEO;

	ret = dev_read_u32(dev, "video-mode", &video_mode);
	if (!ret) {
		switch (video_mode) {
		case 0:
			/* burst mode */
			priv->mode_flags |= MIPI_DSI_MODE_VIDEO_BURST;
			break;
		case 1:
			/* non-burst mode with sync event */
			break;
		case 2:
			/* non-burst mode with sync pulse */
			priv->mode_flags |= MIPI_DSI_MODE_VIDEO_SYNC_PULSE;
			break;
		default:
			dev_warn(dev, "invalid video mode %d\n", video_mode);
			break;
		}
	}

	ret = dev_read_u32(dev, "dsi-lanes", &priv->lanes);
	if (ret) {
		printf("Failed to get dsi-lanes property (%d)\n", ret);
		return ret;
	}

	return 0;
}

static int winstar_panel_disable(struct udevice *dev)
{
	struct winstar_panel_priv *priv = dev_get_priv(dev);
        struct mipi_dsi_panel_plat *plat = dev_get_plat(dev);
        struct mipi_dsi_device *dsi = plat->device;
        int ret;

        ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
        if (ret < 0) {
                printf("Failed to exit sleep mode (%d)\n", ret);
                return -EIO;
        }

	dm_gpio_set_value(&priv->reset, true);

	return 0;
}

static const struct panel_ops winstar_panel_ops = {
	.enable_backlight = winstar_panel_enable_backlight,
	.get_display_timing = winstar_panel_get_display_timing,
};

static const struct rad_platform_data rad_winstar = {
	.enable = &winstar_enable,
};


static const struct udevice_id winstar_panel_ids[] = {
	{ .compatible = "panel-winstar", .data = (ulong)&rad_winstar },
	{ }
};

U_BOOT_DRIVER(winstar_panel) = {
	.name			  = "winstar_panel",
	.id			  = UCLASS_PANEL,
	.of_match		  = winstar_panel_ids,
	.ops			  = &winstar_panel_ops,
	.probe			  = winstar_panel_probe,
	.remove			  = winstar_panel_disable,
	.plat_auto = sizeof(struct mipi_dsi_panel_plat),
	.priv_auto = sizeof(struct winstar_panel_priv),
};
