/*-----------------------------------------------------------------------------
 * FILE NAME : austin.c
 * 
 * PURPOSE : 
 * 
 * Copyright (C) 2007 - 2012 OHSUNG ELECTRONICS CO., LTD.
 * All right reserved. 
 * 
 * This software is confidential and proprietary to OHSUNG
 * ELECTRONICS CO., LTD. No Part of this software may 
 * be reproduced, stored, transmitted, disclosed or used in any
 * form or by any means other than as expressly provide by the 
 * written license agreement between OHSUNG ELECTRONICS CO., LTD.
 * and its licensee.
 *
 * NOTES:
 *
 * REQUIREMENTS/FUNCTIONAL SPECIFICATIONS REFERENCES:
 * 
 * ALGORITHM (PDL):
 * 
 *---------------------------------------------------------------------------*/ 

/*_____________________ Include Header ______________________________________*/
#include <common.h>
#include <init.h>
#include <gpio.h>
#include <environment.h>
#include <errno.h>
#include <nand.h>
#include <sizes.h>
#include <usb/ehci.h>
#include <asm/armlinux.h>
#include <io.h>
#include <asm/mmu.h>
#include <generated/mach-types.h>
#include <mach/imx-regs.h>
#include <mach/clock.h>
#include <mach/mci.h>
#include <mach/fb.h>
#include <mach/usb.h>

/*_____________________ Constants Definitions _______________________________*/

/*_____________________ Type Definitions ____________________________________*/

/*_____________________ Imported Variables __________________________________*/

/*_____________________ Variables Definitions _______________________________*/

/*_____________________ Local Declarations __________________________________*/
static const uint32_t _pad_setup[] = {
	/* DUART */
	PWM0_DUART_RX, 
	PWM1_DUART_TX,

	/* NAND */
	GPMI_D00 | PULLUP(1),
	GPMI_D01 | PULLUP(1),
	GPMI_D02 | PULLUP(1),
	GPMI_D03 | PULLUP(1),
	GPMI_D04 | PULLUP(1),
	GPMI_D05 | PULLUP(1),
	GPMI_D06 | PULLUP(1),
	GPMI_D07 | PULLUP(1),
	GPMI_CLE,
	GPMI_ALE,
	GPMI_RDY0 | PULLUP(1),
	GPMI_WPM,	/* kernel tries here with 12 mA */
	GPMI_WRN,	/* kernel tries here with 12 mA */
	GPMI_RDN,	/* kernel tries here with 12 mA */
	GPMI_CE0N,

	/* LCD */
	LCD_D0 | STRENGTH(S12MA),
	LCD_D1 | STRENGTH(S12MA),
	LCD_D2 | STRENGTH(S12MA),
	LCD_D3 | STRENGTH(S12MA),
	LCD_D4 | STRENGTH(S12MA),
	LCD_D5 | STRENGTH(S12MA),
	LCD_D6 | STRENGTH(S12MA),
	LCD_D7 | STRENGTH(S12MA),
	LCD_D8 | STRENGTH(S12MA),
	LCD_D9 | STRENGTH(S12MA),
	LCD_D10 | STRENGTH(S12MA),
	LCD_D11 | STRENGTH(S12MA),
	LCD_D12 | STRENGTH(S12MA),
	LCD_D13 | STRENGTH(S12MA),
	LCD_D14 | STRENGTH(S12MA),
	LCD_D15 | STRENGTH(S12MA),
	LCD_RESET | STRENGTH(S12MA),
	LCD_RS | STRENGTH(S12MA),
	LCD_WR | STRENGTH(S12MA),
	LCD_CS | STRENGTH(S12MA),
	LCD_DOTCLOCK | STRENGTH(S12MA),
	LCD_ENABE | STRENGTH(S12MA),
	LCD_HSYNC | STRENGTH(S12MA),
	LCD_VSYNC | STRENGTH(S12MA),

	/* FIXME : LCD Backlight port changed */ 
#if 0
	PWM1_GPIO | GPIO_OUT | GPIO_VALUE(0), /* LCD Backlight (PWM), initially off */
#endif
	PWM4_GPIO | GPIO_OUT | GPIO_VALUE(0), /* LCD Power, 1 enable, 0 disable the lcd */

	/* WLAN */
	AUART1_CTS_GPIO | GPIO_OUT | GPIO_VALUE(1), /* WLAN_RESET */
	SSP1_CMD,
	SSP1_DATA0,
	SSP1_DATA1,
	SSP1_DATA2,
	SSP1_DATA3,
	SSP1_SCK,
	GPMI_CE1N_GPIO | GPIO_OUT | GPIO_VALUE(0), /* WLAN_POWER */

	/* Key */
	GPMI_D08_GPIO | GPIO_IN, /* KR7 */
	GPMI_D09_GPIO | GPIO_IN, /* KR6 */
	GPMI_D10_GPIO | GPIO_IN, /* KR5 */
	GPMI_D11_GPIO | GPIO_IN, /* KR4 */
	GPMI_D12_GPIO | GPIO_IN, /* KR3 */
	GPMI_D13_GPIO | GPIO_IN, /* KR2 */
	GPMI_D14_GPIO | GPIO_IN, /* KR1 */
	GPMI_D15_GPIO | GPIO_IN, /* KR0 */

	GPMI_RDY1_GPIO | GPIO_IN, /* KS0 */
	GPMI_RDY2_GPIO | GPIO_IN, /* KS1 */
	GPMI_RDY3_GPIO | GPIO_IN, /* KS2 */
	ROTARYB_GPIO | GPIO_IN, /* KS3 */
	ROTARYA_GPIO | GPIO_IN, /* KS4 */
	LCD_D16_GPIO | GPIO_IN, /* KS5 */
	LCD_D17_GPIO | GPIO_IN, /* KS6 */

	PWM2_GPIO | GPIO_OUT | GPIO_VALUE(0), /* Key LED (PWM), initially off */

	/* USB */
	GPMI_CE2N_GPIO | GPIO_IN,
	SSP1_DETECT_USB_ID,

	/* Battery Status */
	AUART1_RTS_GPIO | GPIO_IN,

	/* TODO : changed buzzer port */ 
#if 0
	/* Buzzer */
	PWM0_GPIO | GPIO_OUT | GPIO_VALUE(0), /* Buzzer (PWM), initially off */
#endif

	/* TILL */
	PWM3_GPIO | GPIO_IN,

	/* not used pins */
	I2C_CLK_GPIO | GPIO_IN | PULLUP(1),
	I2C_SDA_GPIO | GPIO_IN | PULLUP(1),
	EMI_CE1N_GPIO | GPIO_IN | PULLUP(1),
};

/*_____________________ Program Body ________________________________________*/
static int
_austin_mem_init(void)
{
	arm_add_mem_device("ram0", IMX_MEMORY_BASE, 32 * 1024 * 1024);

	return 0;
}
mem_initcall(_austin_mem_init);

static int
_austin_devices_init(void)
{
	int i;

	/* initizalize gpios */
	for (i = 0; i < ARRAY_SIZE(_pad_setup); i++)
		imx_gpio_mode(_pad_setup[i]);

	armlinux_set_bootparams((void *)IMX_MEMORY_BASE + 0x100);
	armlinux_set_architecture(MACH_TYPE_AUSTIN_MX23);

	return 0;
}

device_initcall(_austin_devices_init);

static int
_austin_console_init(void)
{
	add_generic_device("stm_serial", 0, NULL, IMX_DBGUART_BASE, 8192,
			   IORESOURCE_MEM, NULL);

	return 0;
}

console_initcall(_austin_console_init);

