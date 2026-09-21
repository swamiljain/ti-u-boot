// SPDX-License-Identifier: GPL-2.0+
/*
 * SII902x RGB/HDMI bridge driver
 *
 * Copyright (C) 2024 Texas Instruments Incorporated - https://www.ti.com/
 *
 * Ported from Linux kernel driver:
 *   Copyright (C) 2018 Renesas Electronics
 *   Copyright (C) 2016 Atmel
 *   Copyright (C) 2010-2011 Freescale Semiconductor, Inc.
 */

#include <dm.h>
#include <errno.h>
#include <edid.h>
#include <fdtdec.h>
#include <i2c.h>
#include <log.h>
#include <panel.h>
#include <video_bridge.h>
#include <linux/delay.h>
#include <time.h>
#include <dm/device_compat.h>
#include <dm/ofnode.h>
#include <linux/kernel.h>

#define SII902X_TPI_VIDEO_DATA			0x00

#define SII902X_TPI_PIXEL_REPETITION		0x08
#define SII902X_TPI_AVI_PIXEL_REP_BUS_24BIT	BIT(5)
#define SII902X_TPI_AVI_PIXEL_REP_RISING_EDGE	BIT(4)
#define SII902X_TPI_AVI_PIXEL_REP_NONE		0
#define SII902X_TPI_CLK_RATIO_1X		BIT(6)

#define SII902X_TPI_AVI_IN_FORMAT		0x09
#define SII902X_TPI_AVI_INPUT_RANGE_AUTO	(0 << 2)
#define SII902X_TPI_AVI_INPUT_COLORSPACE_RGB	(0 << 0)

#define SII902X_SYS_CTRL_DATA			0x1a
#define SII902X_SYS_CTRL_PWR_DWN		BIT(4)
#define SII902X_SYS_CTRL_AV_MUTE		BIT(3)
#define SII902X_SYS_CTRL_DDC_BUS_REQ		BIT(2)
#define SII902X_SYS_CTRL_DDC_BUS_GRTD		BIT(1)
#define SII902X_SYS_CTRL_OUTPUT_MODE		BIT(0)
#define SII902X_SYS_CTRL_OUTPUT_HDMI		1
#define SII902X_SYS_CTRL_OUTPUT_DVI		0

#define SII902X_REG_CHIPID(n)			(0x1b + (n))

#define SII902X_PWR_STATE_CTRL			0x1e
#define SII902X_AVI_POWER_STATE_MSK		GENMASK(1, 0)
#define SII902X_AVI_POWER_STATE_D(l)		((l) & SII902X_AVI_POWER_STATE_MSK)

#define SII902X_INT_ENABLE			0x3c
#define SII902X_INT_STATUS			0x3d
#define SII902X_HOTPLUG_EVENT			BIT(0)
#define SII902X_PLUGGED_STATUS			BIT(2)

#define SII902X_REG_TPI_RQB			0xc7

#define SII902X_I2C_BUS_ACQUISITION_TIMEOUT_MS	500
#define SII902X_DDC_ADDR			0x50

#define SII902X_MIN_PIXEL_CLOCK_KHZ		25000
#define SII902X_MAX_PIXEL_CLOCK_KHZ		165000

static const struct display_timing default_timing = {
	.pixelclock.typ		= 148500,
	.hactive.typ		= 1920,
	.hfront_porch.typ	= 88,
	.hback_porch.typ	= 148,
	.hsync_len.typ		= 44,
	.vactive.typ		= 1080,
	.vfront_porch.typ	= 4,
	.vback_porch.typ	= 36,
	.vsync_len.typ		= 5,
	.flags			= DISPLAY_FLAGS_HSYNC_HIGH | DISPLAY_FLAGS_VSYNC_HIGH,
};

struct sii902x_priv {
	u8 edid[EDID_EXT_SIZE];
	struct display_timing timing;
};

static int sii902x_reg_read(struct udevice *dev, u8 reg)
{
	int ret = dm_i2c_reg_read(dev, reg);
	printf("%s: reg=0x%02x ret=%d\n", __func__, reg, ret);
	return ret;
}

static int sii902x_reg_write(struct udevice *dev, u8 reg, u8 val)
{
	int ret = dm_i2c_reg_write(dev, reg, val);
	printf("%s: reg=0x%02x val=0x%02x ret=%d\n", __func__, reg, val, ret);
	return ret;
}

static int sii902x_update_bits(struct udevice *dev, u8 reg, u8 mask, u8 val)
{
	int ret, old_val, new_val;

	ret = sii902x_reg_read(dev, reg);
	if (ret < 0) {
		printf("%s: reg=0x%02x read failed: %d\n", __func__, reg, ret);
		return ret;
	}

	old_val = ret;
	new_val = (ret & ~mask) | (val & mask);

	printf("%s: reg=0x%02x mask=0x%02x val=0x%02x old=0x%02x new=0x%02x\n",
	       __func__, reg, mask, val, old_val, new_val);

	return sii902x_reg_write(dev, reg, new_val);
}

/**
 * sii902x_bridge_mode_set() - Configure display timings
 *
 * Internal function to configure the SII902X bridge with display timings.
 * This writes video timing data to specific TPI registers based on the
 * Linux kernel implementation.
 *
 * @dev: Bridge device
 * @timing: Display timing configuration
 * @return: 0 on success, negative error code on failure
 */
static int sii902x_bridge_mode_set(struct udevice *dev,
				   const struct display_timing *timing)
{
	u8 buf[10];
	u16 pixel_clock_10khz;
	int ret;

	printf("%s: entry\n", __func__);

	if (!timing) {
		dev_err(dev, "Invalid timing parameter\n");
		printf("%s: timing is NULL\n", __func__);
		return -EINVAL;
	}

	/* Convert pixel clock from Hz to 10kHz units */
	pixel_clock_10khz = timing->pixelclock.typ / 10000;

	dev_info(dev, "Setting mode: %dx%d@%dkHz\n",
		 timing->hactive.typ, timing->vactive.typ,
		 timing->pixelclock.typ / 1000);

	/* Prepare TPI video data (registers 0x00-0x09) based on Linux kernel */
	buf[0] = pixel_clock_10khz & 0xff;		/* Pixel clock low byte */
	buf[1] = (pixel_clock_10khz >> 8) & 0xff;	/* Pixel clock high byte */
	buf[2] = 60;					/* Refresh rate (assume 60Hz) */
	buf[3] = 0x00;					/* Reserved */
	buf[4] = timing->hactive.typ & 0xff;		/* H active low byte */
	buf[5] = (timing->hactive.typ >> 8) & 0xff;	/* H active high byte */
	buf[6] = timing->vactive.typ & 0xff;		/* V active low byte */
	buf[7] = (timing->vactive.typ >> 8) & 0xff;	/* V active high byte */
	buf[8] = SII902X_TPI_CLK_RATIO_1X |
		 SII902X_TPI_AVI_PIXEL_REP_NONE |
		 SII902X_TPI_AVI_PIXEL_REP_BUS_24BIT;	/* TPI pixel repetition */
	buf[9] = SII902X_TPI_AVI_INPUT_RANGE_AUTO |
		 SII902X_TPI_AVI_INPUT_COLORSPACE_RGB;	/* TPI AVI input format */

	/* Write video timing data to TPI registers 0x00-0x09 */
	for (int i = 0; i < 10; i++) {
		ret = sii902x_reg_write(dev, SII902X_TPI_VIDEO_DATA + i, buf[i]);
		if (ret) {
			dev_err(dev, "Failed to write TPI video data[%d]: %d\n", i, ret);
			printf("%s: failed to write TPI video data[%d]: %d\n",
			       __func__, i, ret);
			return ret;
		}
	}

	/* Validate timing constraints */
	if (timing->pixelclock.typ < SII902X_MIN_PIXEL_CLOCK_KHZ * 1000 ||
	    timing->pixelclock.typ > SII902X_MAX_PIXEL_CLOCK_KHZ * 1000) {
		dev_warn(dev, "Pixel clock %d Hz out of range (%d-%d kHz)\n",
			 timing->pixelclock.typ,
			 SII902X_MIN_PIXEL_CLOCK_KHZ,
			 SII902X_MAX_PIXEL_CLOCK_KHZ);
	}

	dev_info(dev, "Mode set complete: %dx%d@%dHz, pixel_clock=%dkHz\n",
		 timing->hactive.typ, timing->vactive.typ,
		 60, timing->pixelclock.typ / 1000);

	return 0;
}

/**
 * sii902x_bridge_enable() - Enable the SII902X bridge
 *
 * This function enables the SII902X bridge by configuring the output mode
 * and power state registers. Based on Linux kernel's sii902x_bridge_atomic_enable().
 *
 * @dev: Bridge device
 * @return: 0 on success, negative error code on failure
 */
static int sii902x_bridge_init(struct udevice *dev)
{
	int ret;
	u8 output_mode = SII902X_SYS_CTRL_OUTPUT_HDMI;  /* Default to HDMI mode */

	printf("%s: entry\n", __func__);

	/* Set output mode to HDMI */
	ret = sii902x_update_bits(dev, SII902X_SYS_CTRL_DATA,
				  SII902X_SYS_CTRL_OUTPUT_MODE, output_mode);
	printf("%s: set output mode ret=%d\n", __func__, ret);
	if (ret) {
		dev_err(dev, "Failed to set output mode: %d\n", ret);
		return ret;
	}

	/* Power up the device (Power State D0) */
	ret = sii902x_update_bits(dev, SII902X_PWR_STATE_CTRL,
				  SII902X_AVI_POWER_STATE_MSK,
				  SII902X_AVI_POWER_STATE_D(0));
	printf("%s: set power state ret=%d\n", __func__, ret);
	if (ret) {
		dev_err(dev, "Failed to set power state: %d\n", ret);
		return ret;
	}

	/* Clear power down bit to enable the device */
	ret = sii902x_update_bits(dev, SII902X_SYS_CTRL_DATA,
				  SII902X_SYS_CTRL_PWR_DWN, 0);
	printf("%s: clear power down ret=%d\n", __func__, ret);
	if (ret) {
		dev_err(dev, "Failed to clear power down: %d\n", ret);
		return ret;
	}

	printf("%s: done\n", __func__);
	return 0;
}

static int sii902x_ddc_bus_request(struct udevice *dev)
{
	u8 timeout_count = 0;
	int ret, status;

	printf("%s: entry\n", __func__);

	ret = sii902x_update_bits(dev, SII902X_SYS_CTRL_DATA,
				  SII902X_SYS_CTRL_DDC_BUS_REQ,
				  SII902X_SYS_CTRL_DDC_BUS_REQ);
	if (ret) {
		printf("%s: failed to request DDC bus: %d\n", __func__, ret);
		return ret;
	}

	timeout_count = 0;
	while (timeout_count < 5) {
		status = sii902x_reg_read(dev, SII902X_SYS_CTRL_DATA);
		if (status < 0) {
			printf("%s: status read failed: %d\n", __func__, status);
			return status;
		}

		if (status & SII902X_SYS_CTRL_DDC_BUS_GRTD) {
			printf("%s: DDC bus granted after %u ms\n", __func__,
			       timeout_count);

			/*
			 * Per datasheet: once granted, the host must write
			 * back bits[2:1]=11 (REQ|GRTD). This closes the
			 * switch and allows subsequent I2C accesses to flow
			 * out onto the DDC bus.
			 */
			ret = sii902x_update_bits(dev, SII902X_SYS_CTRL_DATA,
						  SII902X_SYS_CTRL_DDC_BUS_REQ |
						  SII902X_SYS_CTRL_DDC_BUS_GRTD,
						  SII902X_SYS_CTRL_DDC_BUS_REQ |
						  SII902X_SYS_CTRL_DDC_BUS_GRTD);
			printf("%s: close DDC switch write ret=%d\n",
			       __func__, ret);
			if (ret) {
				printf("%s: failed to close DDC switch: %d\n",
				       __func__, ret);
				return ret;
			}

			return 0;
		}

		udelay(1000); /* Wait 1ms between attempts */
		timeout_count++;
	}

	printf("%s: timed out waiting for DDC bus grant, status=0x%02x\n",
	       __func__, status);
	return -EBUSY;
}

static int sii902x_ddc_bus_release(struct udevice *dev)
{
	unsigned long start;
	int ret, status;

	printf("%s: entry\n", __func__);

	/*
	 * Per datasheet: writing 0x1A[2:1]=00 to clear the DDC bus request
	 * is expected to NACK ("the device will not ACK this write"), so a
	 * write error here is not fatal - success is verified via read-back
	 * below, repeating the write if the bits haven't cleared yet.
	 */
	ret = sii902x_update_bits(dev, SII902X_SYS_CTRL_DATA,
				  SII902X_SYS_CTRL_DDC_BUS_REQ |
				  SII902X_SYS_CTRL_DDC_BUS_GRTD, 0);
	printf("%s: clear req/grant write ret=%d (NACK expected per datasheet)\n",
	       __func__, ret);

	start = get_timer(0);
	do {
		status = sii902x_reg_read(dev, SII902X_SYS_CTRL_DATA);
		if (status < 0) {
			printf("%s: status read failed: %d\n", __func__, status);
			return status;
		}

		if (!(status & (SII902X_SYS_CTRL_DDC_BUS_REQ |
				SII902X_SYS_CTRL_DDC_BUS_GRTD))) {
			printf("%s: DDC bus released after %lu ms\n", __func__,
			       get_timer(start));
			return 0;
		}

		/* Bits not yet clear - repeat the clearing write */
		ret = sii902x_update_bits(dev, SII902X_SYS_CTRL_DATA,
					  SII902X_SYS_CTRL_DDC_BUS_REQ |
					  SII902X_SYS_CTRL_DDC_BUS_GRTD, 0);
		printf("%s: retry clear req/grant write ret=%d\n",
		       __func__, ret);

		udelay(1000);
	} while (get_timer(start) < SII902X_I2C_BUS_ACQUISITION_TIMEOUT_MS);

	dev_err(dev, "failed to release DDC bus\n");
	printf("%s: timed out releasing DDC bus, status=0x%02x\n", __func__,
	       status);
	return -ETIMEDOUT;
}

/**
 * sii902x_ddc_read_edid() - Read EDID from the HDMI sink via DDC
 *
 * The DDC device (address 0x50) shares the same physical I2C bus as
 * the bridge itself.  We read EDID by sending raw I2C messages to
 * the DDC address while the bus is granted to us.
 */
static int sii902x_ddc_read_edid(struct udevice *dev, u8 *buf, int size)
{
	struct udevice *i2c_bus = dev_get_parent(dev);
	struct udevice *ddc_dev;
	int ret;

	printf("%s: entry, size=%d\n", __func__, size);

	if (!buf || size < 128) {
		printf("%s: invalid buf/size (buf=%p, size=%d)\n", __func__,
		       buf, size);
		return -EINVAL;
	}

	ret = i2c_get_chip(i2c_bus, SII902X_DDC_ADDR, 1, &ddc_dev);
	printf("%s: i2c_get_chip(addr=0x%02x) ret=%d\n", __func__,
	       SII902X_DDC_ADDR, ret);
	if (ret) {
		dev_err(dev, "cannot get DDC chip device: %d\n", ret);
		return ret;
	}

	/* Read the base EDID block (128 bytes) */
	ret = dm_i2c_read(ddc_dev, 0, buf, EDID_SIZE);
	printf("%s: dm_i2c_read(EDID_SIZE=%d) ret=%d\n", __func__, EDID_SIZE,
	       ret);
	if (ret) {
		dev_err(dev, "failed to read base EDID block: %d\n", ret);
		return ret;
	}

	return EDID_SIZE;
}

static int sii902x_check_chipid(struct udevice *dev)
{
	int ret;
	u8 chipid[4];
	int i;

	printf("%s: entry\n", __func__);

	for (i = 0; i < 4; i++) {
		ret = sii902x_reg_read(dev, SII902X_REG_CHIPID(i));
		if (ret < 0) {
			dev_err(dev, "failed to read chip ID[%d]: %d\n",
				i, ret);
			printf("%s: failed to read chip ID[%d]: %d\n",
			       __func__, i, ret);
			return ret;
		}
		chipid[i] = ret;
	}

	printf("%s: chipid=%02x %02x %02x %02x\n", __func__,
	       chipid[0], chipid[1], chipid[2], chipid[3]);

	if (chipid[0] != 0xb0) {
		dev_err(dev, "invalid chip ID: %02x (expected 0xb0)\n",
			chipid[0]);
		printf("%s: invalid chip ID: 0x%02x (expected 0xb0)\n",
		       __func__, chipid[0]);
		return -ENODEV;
	}

	return 0;
}

static int sii902x_read_edid(struct udevice *dev, u8 *buf, int buf_size)
{
	struct sii902x_priv *priv = dev_get_priv(dev);
	int ret, size;

	printf("%s: entry, buf_size=%d\n", __func__, buf_size);
	i2c_set_chip_flags(dev, 0);
	ret = sii902x_ddc_bus_request(dev);
	printf("%s: ddc_bus_request ret=%d\n", __func__, ret);
	if (ret)
		return ret;
	i2c_set_chip_flags(dev, 0);
	size = sii902x_ddc_read_edid(dev, priv->edid,
				     min(buf_size, (int)sizeof(priv->edid)));
	printf("%s: ddc_read_edid size=%d\n", __func__, size);

	/*
	 * Per datasheet, clearing the DDC bus request/grant bits is expected
	 * to NACK. Mark the chip ignore-NAK for the release step only, so
	 * that expected NACK is treated as success instead of propagating as
	 * -EREMOTEIO; restore normal ACK checking immediately afterward.
	 */
	i2c_set_chip_flags(dev, DM_I2C_CHIP_IGNORE_NAK);
	ret = sii902x_ddc_bus_release(dev);
	i2c_set_chip_flags(dev, 0);
	printf("%s: ddc_bus_release ret=%d\n", __func__, ret);
	if (ret)
		return ret;

	if (size <= 0) {
		printf("%s: EDID read failed, size=%d\n", __func__, size);
		return size ? size : -EIO;
	}

	size = min(size, buf_size);
	memcpy(buf, priv->edid, size);

	printf("%s: returning %s (size=%d)\n", __func__,
	       size >= 128 ? "0" : "-EIO", size);
	return size >= 128 ? 0 : -EIO;
}

/**
 * sii902x_get_display_timings() - Get display timings from panel device tree
 *
 * This function searches for a panel device and extracts display timings
 * from either the panel device or directly from the device tree panel node.
 *
 * @dev: SII902X bridge device
 * @timings: Pointer to display_timing structure to fill
 * @return: 0 on success, negative error code on failure
 */
static int sii902x_get_display_timings(struct udevice *dev, struct display_timing *timing)
{
	struct sii902x_priv *priv = dev_get_priv(dev);
	u8 edid[128];
	int bpc, ret;

	printf("%s: entry\n", __func__);

	ret = sii902x_read_edid(dev, edid, sizeof(edid));
	printf("%s: sii902x_read_edid ret=%d\n", __func__, ret);
	if (ret) {
		printf("%s: falling back to default_timing\n", __func__);
		memcpy(timing, &default_timing, sizeof(*timing));
		return 0;
	}

	ret = edid_get_timing(edid, sizeof(edid), timing, &bpc);
	printf("%s: edid_get_timing ret=%d\n", __func__, ret);
	if (ret) {
		printf("%s: falling back to default_timing\n", __func__);
		memcpy(timing, &default_timing, sizeof(*timing));
		return 0;
	}

	/*
	 * edid_get_timing() returns pixelclock in Hz; U-Boot display_timing
	 * convention expects kHz (tidss does pclk * 1000 for clk_set_rate).
	 */
	timing->pixelclock.typ /= 1000;
	timing->pixelclock.min /= 1000;
	timing->pixelclock.max /= 1000;

	printf("%s: EDID timing: %ux%u @ %u kHz\n", __func__,
	       timing->hactive.typ, timing->vactive.typ, timing->pixelclock.typ);

	priv->timing = *timing;
	return 0;
}

static int sii902x_enable(struct udevice *dev)
{
	struct display_timing timing;
	int ret;

	printf("%s: entry\n", __func__);

	ret = sii902x_get_display_timings(dev, &timing);
	printf("%s: get_display_timings ret=%d\n", __func__, ret);
	// if (ret = 0) {
	// 	dev_err(dev, "Failed to get display timings: %d\n", ret);
	// 	return ret;
	// }

	/* Configure the bridge with the retrieved timings */
	ret = sii902x_bridge_mode_set(dev, &timing);
	printf("%s: bridge_mode_set ret=%d\n", __func__, ret);
	// if (ret = 0) {
	// 	dev_err(dev, "Failed to set bridge mode: %d\n", ret);
	// 	return ret;
	// }
	printf("%s: done\n", __func__);
	return 0;
}

static int sii902x_probe(struct udevice *dev)
{
	int ret;

	printf("%s: entry for '%s'\n", __func__, dev->name);

	if (device_get_uclass_id(dev->parent) != UCLASS_I2C) {
		printf("%s: parent is not UCLASS_I2C\n", __func__);
		return -EPROTONOSUPPORT;
	}

	/* Enable TPI mode */
	ret = sii902x_reg_write(dev, SII902X_REG_TPI_RQB, 0x00);
	printf("%s: enable TPI mode ret=%d\n", __func__, ret);
	if (ret) {
		dev_err(dev, "failed to enable TPI mode: %d\n", ret);
		return ret;
	}

	ret = sii902x_check_chipid(dev);
	printf("%s: check_chipid ret=%d\n", __func__, ret);
	if (ret)
		return ret;

	ret = sii902x_bridge_init(dev);
	printf("%s: bridge_init ret=%d, probe complete\n", __func__, ret);
	return ret;
}

static struct video_bridge_ops sii902x_ops = {
	.read_edid	= sii902x_read_edid,
	.get_display_timing = sii902x_get_display_timings,
	.enable		= sii902x_enable,
};

static const struct udevice_id sii902x_ids[] = {
	{ .compatible = "sil,sii9022", },
	{ }
};

U_BOOT_DRIVER(sii902x) = {
	.name	= "sii902x",
	.id	= UCLASS_VIDEO_BRIDGE,
	.of_match = sii902x_ids,
	.probe	= sii902x_probe,
	.ops	= &sii902x_ops,
	.priv_auto	= sizeof(struct sii902x_priv),
};
