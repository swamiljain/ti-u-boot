/*
 * Copyright (C) 2013, Boundary Devices <info@boundarydevices.com>
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., http://www.fsf.org/about/contact/
 *
 */

#include <display_options.h>
#include <env.h>
#include <splash.h>
#include <video.h>
#include <vsprintf.h>
#include <linux/kernel.h>

static struct splash_location default_splash_locations[] = {
	{
		.name = "sf",
		.storage = SPLASH_STORAGE_SF,
		.flags = SPLASH_STORAGE_RAW,
		.offset = 0x0,
	},
	{
		.name = "mmc_fs",
		.storage = SPLASH_STORAGE_MMC,
		.flags = SPLASH_STORAGE_FS,
		.devpart = "0:1",
	},
	{
		.name = "mmc_raw",
		.storage = SPLASH_STORAGE_MMC,
		.flags = SPLASH_STORAGE_RAW,
		.devpart = "0:1",
	},
	{
		.name = "usb_fs",
		.storage = SPLASH_STORAGE_USB,
		.flags = SPLASH_STORAGE_FS,
		.devpart = "0:1",
	},
	{
		.name = "sata_fs",
		.storage = SPLASH_STORAGE_SATA,
		.flags = SPLASH_STORAGE_FS,
		.devpart = "0:1",
	},
};

#ifdef CONFIG_VIDEO_LOGO

#include <bmp_logo_data.h>

static int splash_video_logo_load(void)
{
	char *splashimage;
	ulong bmp_load_addr;

	printf("%s: entry\n", __func__);

	splashimage = env_get("splashimage");
	if (!splashimage) {
		printf("%s: 'splashimage' env var not set\n", __func__);
		return -ENOENT;
	}

	bmp_load_addr = hextoul(splashimage, 0);
	if (!bmp_load_addr) {
		printf("Error: bad 'splashimage' address\n");
		return -EFAULT;
	}

	printf("%s: copying %u byte logo to 0x%lx\n", __func__,
	       (unsigned int)ARRAY_SIZE(bmp_logo_bitmap), bmp_load_addr);
	memcpy((void *)bmp_load_addr, bmp_logo_bitmap,
	       ARRAY_SIZE(bmp_logo_bitmap));

	printf("%s: done\n", __func__);
	return 0;
}
#else
static inline int splash_video_logo_load(void)
{
	printf("%s: CONFIG_VIDEO_LOGO not enabled\n", __func__);
	return -ENOSYS;
}
#endif

__weak int splash_screen_prepare(void)
{
	int ret;

	printf("%s: entry, SPLASH_SOURCE=%d\n", __func__,
	       CONFIG_IS_ENABLED(SPLASH_SOURCE));

	if (CONFIG_IS_ENABLED(SPLASH_SOURCE)) {
		ret = splash_source_load(default_splash_locations,
					  ARRAY_SIZE(default_splash_locations));
		printf("%s: splash_source_load returned %d\n", __func__, ret);
		return ret;
	}

	ret = splash_video_logo_load();
	printf("%s: splash_video_logo_load returned %d\n", __func__, ret);
	return ret;
}

void splash_get_pos(int *x, int *y)
{
	char *s = env_get("splashpos");

	printf("%s: entry, splashpos='%s'\n", __func__, s ? s : "<unset>");

	if (!CONFIG_IS_ENABLED(SPLASH_SCREEN_ALIGN) || !s) {
		printf("%s: exit, using defaults x=%d, y=%d\n", __func__, *x, *y);
		return;
	}

	if (s[0] == 'm')
		*x = BMP_ALIGN_CENTER;
	else
		*x = simple_strtol(s, NULL, 0);

	s = strchr(s + 1, ',');
	if (s != NULL) {
		if (s[1] == 'm')
			*y = BMP_ALIGN_CENTER;
		else
			*y = simple_strtol(s + 1, NULL, 0);
	}

	printf("%s: exit, x=%d, y=%d\n", __func__, *x, *y);
}

#if CONFIG_IS_ENABLED(VIDEO) && !CONFIG_IS_ENABLED(HIDE_LOGO_VERSION)

#ifdef CONFIG_VIDEO_LOGO
#include <bmp_logo.h>
#endif
#include <dm.h>
#include <video_console.h>
#include <video_font.h>
#include <video_font_data.h>

void splash_display_banner(void)
{
	struct video_fontdata __maybe_unused *fontdata = fonts;
	struct udevice *dev;
	char buf[DISPLAY_OPTIONS_BANNER_LENGTH];
	int col, row, ret;

	printf("%s: entry\n", __func__);

	ret = uclass_get_device(UCLASS_VIDEO_CONSOLE, 0, &dev);
	printf("%s: uclass_get_device(UCLASS_VIDEO_CONSOLE) ret=%d, dev=%s\n",
	       __func__, ret, (!ret && dev) ? dev->name : "<none>");
	if (ret)
		return;

#if IS_ENABLED(CONFIG_VIDEO_LOGO)
	col = BMP_LOGO_WIDTH / fontdata->width + 1;
	row = BMP_LOGO_HEIGHT / fontdata->height + 1;
#else
	col = 0;
	row = 0;
#endif

	display_options_get_banner(false, buf, sizeof(buf));
	printf("%s: banner='%s', col=%d, row=%d\n", __func__, buf, col, row);
	vidconsole_position_cursor(dev, col, 1);
	vidconsole_put_string(dev, buf);
	vidconsole_position_cursor(dev, 0, row);
	printf("%s: exit\n", __func__);
}
#endif /* CONFIG_VIDEO && !CONFIG_HIDE_LOGO_VERSION */

/*
 * Common function to show a splash image if env("splashimage") is set.
 * For additional details please refer to doc/README.splashprepare.
 */
int splash_display(void)
{
	ulong addr;
	char *s;
	int x = 0, y = 0, ret;

	printf("%s: entry\n", __func__);

	if (!CONFIG_IS_ENABLED(SPLASH_SCREEN)) {
		printf("%s: SPLASH_SCREEN not enabled\n", __func__);
		return -ENOSYS;
	}
	s = env_get("splashimage");
	if (!s) {
		printf("%s: 'splashimage' env var not set\n", __func__);
		return -EINVAL;
	}

	addr = hextoul(s, NULL);
	printf("%s: splashimage='%s', addr=0x%lx\n", __func__, s, addr);

	ret = splash_screen_prepare();
	printf("%s: splash_screen_prepare returned %d\n", __func__, ret);
	if (ret)
		return ret;

	splash_get_pos(&x, &y);
	printf("%s: splash_get_pos x=%d, y=%d\n", __func__, x, y);

	if (CONFIG_IS_ENABLED(BMP)) {
		ret = bmp_display(addr, x, y);
		printf("%s: bmp_display returned %d\n", __func__, ret);
	} else {
		printf("%s: CONFIG_BMP not enabled\n", __func__);
		return -ENOSYS;
	}

	/* Skip banner output on video console if the logo is not at 0,0 */
	if (x || y) {
		printf("%s: skipping banner (x=%d, y=%d)\n", __func__, x, y);
		goto end;
	}

#if CONFIG_IS_ENABLED(VIDEO) && !CONFIG_IS_ENABLED(HIDE_LOGO_VERSION)
	splash_display_banner();
#endif
end:
	printf("%s: exit, returning %d\n", __func__, ret);
	return ret;
}
