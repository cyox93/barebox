/*
 * Copyright (C) 2004 Sascha Hauer, Synertronixx GmbH
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 *
 */

#include <common.h>
#include <net.h>
#include <init.h>
#include <environment.h>
#include <generated/mach-types.h>
#include <mach/imx-regs.h>
#include <asm/armlinux.h>
#include <mach/gpio.h>
#include <io.h>
#include <partition.h>
#include <fs.h>
#include <fcntl.h>
#include <dm9000.h>
#include <led.h>
#include <mach/devices-imx1.h>

static struct dm9000_platform_data dm9000_data = {
	.srom     = 1,
};

struct gpio_led leds[] = {
	{
		.gpio = 32 + 21,
	}, {
		.gpio = 32 + 22,
	}, {
		.gpio = 32 + 23,
	}, {
		.gpio = 32 + 24,
	},
};

static int scb9328_mem_init(void)
{
	arm_add_mem_device("ram0", 0x08000000, 16 * 1024 * 1024);

	return 0;
}
mem_initcall(scb9328_mem_init);

static int scb9328_devices_init(void)
{
	int i;

	imx_gpio_mode(PA23_PF_CS5);
	imx_gpio_mode(GPIO_PORTB | GPIO_GPIO | GPIO_OUT | 21);
	imx_gpio_mode(GPIO_PORTB | GPIO_GPIO | GPIO_OUT | 22);
	imx_gpio_mode(GPIO_PORTB | GPIO_GPIO | GPIO_OUT | 23);
	imx_gpio_mode(GPIO_PORTB | GPIO_GPIO | GPIO_OUT | 24);

	for (i = 0; i < ARRAY_SIZE(leds); i++)
		led_gpio_register(&leds[i]);

/* CS3 becomes CS3 by clearing reset default bit 1 in FMCR */
	FMCR = 0x1;

	CS0U = 0x000F2000;
	CS0L = 0x11110d01;

	CS1U = 0x000F0a00;
	CS1L = 0x11110601;
	CS2U = 0x0;
	CS2L = 0x0;
	CS3U = 0x000FFFFF;
	CS3L = 0x00000303;
	CS4U = 0x000F0a00;
	CS4L = 0x11110301;
	CS5U = 0x00008400;
	CS5L = 0x00000D03;

	add_cfi_flash_device(DEVICE_ID_DYNAMIC, 0x10000000, 16 * 1024 * 1024, 0);
	add_dm9000_device(DEVICE_ID_DYNAMIC, 0x16000000, 0x16000004,
			  IORESOURCE_MEM_16BIT, &dm9000_data);

	devfs_add_partition("nor0", 0x00000, 0x40000, DEVFS_PARTITION_FIXED, "self0");
	devfs_add_partition("nor0", 0x40000, 0x20000, DEVFS_PARTITION_FIXED, "env0");
	protect_file("/dev/env0", 1);

	armlinux_set_bootparams((void *)0x08000100);
	armlinux_set_architecture(MACH_TYPE_SCB9328);

	return 0;
}

device_initcall(scb9328_devices_init);

static int scb9328_console_init(void)
{
	/* init gpios for serial port */
	imx_gpio_mode(PC11_PF_UART1_TXD);
	imx_gpio_mode(PC12_PF_UART1_RXD);

	imx1_add_uart0();

	return 0;
}

console_initcall(scb9328_console_init);

