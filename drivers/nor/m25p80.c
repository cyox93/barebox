/*
 * MTD SPI driver for ST M25Pxx (and similar) serial flash chips
 *
 * Author: Mike Lavender, mike@steroidmicros.com
 * Copyright (c) 2005, Intec Automation Inc.
 *
 * Adapted to barebox :  Franck JULLIEN <elec4fun@gmail.com>
 * Copyright (c) 2011
 *
 * Some parts are based on lart.c by Abraham Van Der Merwe
 *
 * Cleaned up and generalized based on mtd_dataflash.c
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <common.h>
#include <init.h>
#include <driver.h>
#include <spi/spi.h>
#include <spi/flash.h>
#include <xfuncs.h>
#include <malloc.h>
#include <errno.h>
#include <linux/err.h>
#include <clock.h>
#include <linux/mtd/mtd.h>
#include "m25p80.h"

/****************************************************************************/

/*
 * Internal helper functions
 */

/*
 * Read the status register, returning its value in the location
 * Return the status register value.
 * Returns negative if error occurred.
 */
static int read_sr(struct m25p *flash)
{
	ssize_t retval;
	u8 code = OPCODE_RDSR;
	u8 val;

	retval = spi_write_then_read(flash->spi, &code, 1, &val, 1);

	if (retval < 0) {
		dev_err(&flash->spi->dev, "error %d reading SR\n",
				(int) retval);
		return retval;
	}

	return val;
}

/*
 * Write status register 1 byte
 * Returns negative if error occurred.
 */
static int write_sr(struct m25p *flash, u8 val)
{
	flash->command[0] = OPCODE_WRSR;
	flash->command[1] = val;

	return spi_write(flash->spi, flash->command, 2);
}

/*
 * Set write enable latch with Write Enable command.
 * Returns negative if error occurred.
 */
static inline int write_enable(struct m25p *flash)
{
	u8	code = OPCODE_WREN;

	return spi_write_then_read(flash->spi, &code, 1, NULL, 0);
}

/*
 * Send write disble instruction to the chip.
 */
static inline int write_disable(struct m25p *flash)
{
	u8	code = OPCODE_WRDI;

	return spi_write_then_read(flash->spi, &code, 1, NULL, 0);
}

/*
 * Enable/disable 4-byte addressing mode.
 */
static inline int set_4byte(struct m25p *flash, int enable)
{
	u8	code = enable ? OPCODE_EN4B : OPCODE_EX4B;

	return spi_write_then_read(flash->spi, &code, 1, NULL, 0);
}

/*
 * Service routine to read status register until ready, or timeout occurs.
 * Returns non-zero if error.
 */
static int wait_till_ready(struct m25p *flash)
{
	int sr;
	uint64_t timer_start;

	timer_start = get_time_ns();

	do {
		if ((sr = read_sr(flash)) < 0)
			break;
		else if (!(sr & SR_WIP))
			return 0;

	} while (!(is_timeout(timer_start, MAX_READY_WAIT * SECOND)));

	return -ETIMEDOUT;
}

/*
 * Erase the whole flash memory
 *
 * Returns 0 if successful, non-zero otherwise.
 */
static int erase_chip(struct m25p *flash)
{
	dev_dbg(&flash->spi->dev, "%s %lldKiB\n",
		__func__, (long long)(flash->size >> 10));

	/* Wait until finished previous write command. */
	if (wait_till_ready(flash))
		return -ETIMEDOUT;

	/* Send write enable, then erase commands. */
	write_enable(flash);

	/* Set up command buffer. */
	flash->command[0] = OPCODE_CHIP_ERASE;

	spi_write(flash->spi, flash->command, 1);

	return 0;
}

static void m25p_addr2cmd(struct m25p *flash, unsigned int addr, u8 *cmd)
{
	/* opcode is in cmd[0] */
	cmd[1] = addr >> (flash->addr_width * 8 -  8);
	cmd[2] = addr >> (flash->addr_width * 8 - 16);
	cmd[3] = addr >> (flash->addr_width * 8 - 24);
	cmd[4] = addr >> (flash->addr_width * 8 - 32);
}

static int m25p_cmdsz(struct m25p *flash)
{
	return 1 + flash->addr_width;
}

/*
 * Erase one sector of flash memory at offset ``offset'' which is any
 * address within the sector which should be erased.
 *
 * Returns 0 if successful, non-zero otherwise.
 */
static int erase_sector(struct m25p *flash, u32 offset, u32 command)
{
	dev_dbg(&flash->spi->dev, "%s %dKiB at 0x%08x\n",
		__func__, flash->erasesize / 1024, offset);

	/* Wait until finished previous write command. */
	if (wait_till_ready(flash))
		return -ETIMEDOUT;

	/* Send write enable, then erase commands. */
	write_enable(flash);

	/* Set up command buffer. */
	flash->command[0] = command;
	m25p_addr2cmd(flash, offset, flash->command);

	spi_write(flash->spi, flash->command, m25p_cmdsz(flash));

	return 0;
}

/*
 * Erase an address range on the flash chip.  The address range may extend
 * one or more erase sectors.  Return an error is there is a problem erasing.
 */
static ssize_t m25p80_erase(struct cdev *cdev, size_t count, loff_t offset)
{
	struct m25p *flash = cdev->priv;
	u32 addr, len;

	dev_dbg(&flash->spi->dev, "%s %s 0x%llx, len %lld\n",
		__func__, "at", (long long)offset, (long long)count);

	/* sanity checks */
	if (offset + count > flash->size)
		return -EINVAL;

	/* Align start and len to erase blocks */
	addr = offset & ~(flash->erasesize - 1);
	len = ALIGN(offset + count, flash->erasesize) - addr;

	/* whole-chip erase? */
	if (len == flash->size) {
		if (erase_chip(flash))
			return -EIO;
		return 0;
	}

	if (flash->erase_opcode_4k) {
		while (len && (addr & (flash->sector_size - 1))) {
			if (ctrlc())
				return -EINTR;
			if (erase_sector(flash, addr, flash->erase_opcode_4k))
				return -EIO;
			addr += flash->erasesize;
			len -= flash->erasesize;
		}

		while (len >= flash->sector_size) {
			if (ctrlc())
				return -EINTR;
			if (erase_sector(flash, addr, flash->erase_opcode))
				return -EIO;
			addr += flash->sector_size;
			len -= flash->sector_size;
		}

		while (len) {
			if (ctrlc())
				return -EINTR;
			if (erase_sector(flash, addr, flash->erase_opcode_4k))
				return -EIO;
			addr += flash->erasesize;
			len -= flash->erasesize;
		}
	} else {
		while (len) {
			if (ctrlc())
				return -EINTR;
			if (erase_sector(flash, addr, flash->erase_opcode))
				return -EIO;

			if (len <= flash->erasesize)
				break;
			addr += flash->erasesize;
			len -= flash->erasesize;
		}
	}

	if (wait_till_ready(flash))
		return -ETIMEDOUT;

	return 0;
}

ssize_t m25p80_read(struct cdev *cdev, void *buf, size_t count, loff_t offset,
		ulong flags)
{
	struct m25p *flash = cdev->priv;
	struct spi_transfer t[2];
	struct spi_message m;
	ssize_t retlen;
	int fast_read = 0;

	/* sanity checks */
	if (!count)
		return 0;

	if (offset + count > flash->size)
		return -EINVAL;

	if (flash->spi->max_speed_hz >= 25000000)
		fast_read = 1;

	spi_message_init(&m);
	memset(t, 0, (sizeof t));

	/* NOTE:
	 * OPCODE_FAST_READ (if available) is faster.
	 * Should add 1 byte DUMMY_BYTE.
	 */
	t[0].tx_buf = flash->command;
	t[0].len = m25p_cmdsz(flash) + fast_read;
	spi_message_add_tail(&t[0], &m);

	t[1].rx_buf = buf;
	t[1].len = count;
	spi_message_add_tail(&t[1], &m);

	/* Byte count starts at zero. */
	retlen = 0;

	/* Wait till previous write/erase is done. */
	if (wait_till_ready(flash))
		return -ETIMEDOUT;

	/* FIXME switch to OPCODE_FAST_READ.  It's required for higher
	 * clocks; and at this writing, every chip this driver handles
	 * supports that opcode.
	 */

	/* Set up the write data buffer. */
	flash->command[0] = fast_read ? OPCODE_FAST_READ : OPCODE_NORM_READ;
	m25p_addr2cmd(flash, offset, flash->command);

	spi_sync(flash->spi, &m);

	retlen = m.actual_length - m25p_cmdsz(flash) - fast_read;

	return retlen;
}

ssize_t m25p80_write(struct cdev *cdev, const void *buf, size_t count,
		loff_t offset, ulong flags)
{
	struct m25p *flash = cdev->priv;
	struct spi_transfer t[2];
	struct spi_message m;
	ssize_t retlen = 0;
	u32 page_offset, page_size;

	debug("m25p80_write %ld bytes at 0x%08lX\n", (unsigned long)count, offset);

	if (offset + count > flash->size)
		return -EINVAL;

	spi_message_init(&m);
	memset(t, 0, (sizeof t));

	t[0].tx_buf = flash->command;
	t[0].len = m25p_cmdsz(flash);
	spi_message_add_tail(&t[0], &m);

	t[1].tx_buf = buf;
	spi_message_add_tail(&t[1], &m);

	/* Wait until finished previous write command. */
	if (wait_till_ready(flash))
		return -ETIMEDOUT;

	write_enable(flash);

	/* Set up the opcode in the write buffer. */
	flash->command[0] = OPCODE_PP;
	m25p_addr2cmd(flash, offset, flash->command);

	page_offset = offset & (flash->page_size - 1);

	/* do all the bytes fit onto one page? */
	if (page_offset + count <= flash->page_size) {
		t[1].len = count;

		spi_sync(flash->spi, &m);

		retlen = m.actual_length - m25p_cmdsz(flash);
	} else {
		u32 i;

		/* the size of data remaining on the first page */
		page_size = flash->page_size - page_offset;

		t[1].len = page_size;
		spi_sync(flash->spi, &m);

		retlen = m.actual_length - m25p_cmdsz(flash);

		/* write everything in flash->page_size chunks */
		for (i = page_size; i < count; i += page_size) {
			page_size = count - i;
			if (page_size > flash->page_size)
				page_size = flash->page_size;

			/* write the next page to flash */
			m25p_addr2cmd(flash, offset + i, flash->command);

			t[1].tx_buf = buf + i;
			t[1].len = page_size;

			wait_till_ready(flash);

			write_enable(flash);

			spi_sync(flash->spi, &m);

			retlen += m.actual_length - m25p_cmdsz(flash);

		}
	}

	return retlen;
}
#ifdef CONFIG_MTD_SST25L
ssize_t sst_write(struct cdev *cdev, const void *buf, size_t count, loff_t offset,
		ulong flags)
{
	struct m25p *flash = cdev->priv;
	struct spi_transfer t[2];
	struct spi_message m;
	size_t actual;
	ssize_t retlen;
	int cmd_sz, ret;

	debug("sst_write %ld bytes at 0x%08lX\n", (unsigned long)count, offset);

	retlen = 0;

	/* sanity checks */
	if (!count)
		return 0;

	if (offset + count > flash->size)
		return -EINVAL;

	spi_message_init(&m);
	memset(t, 0, (sizeof t));

	t[0].tx_buf = flash->command;
	t[0].len = m25p_cmdsz(flash);
	spi_message_add_tail(&t[0], &m);

	t[1].tx_buf = buf;
	spi_message_add_tail(&t[1], &m);

	/* Wait until finished previous write command. */
	ret = wait_till_ready(flash);
	if (ret)
		goto time_out;

	write_enable(flash);

	actual = offset % 2;
	/* Start write from odd address. */
	if (actual) {
		flash->command[0] = OPCODE_BP;
		m25p_addr2cmd(flash, offset, flash->command);

		/* write one byte. */
		t[1].len = 1;
		spi_sync(flash->spi, &m);
		ret = wait_till_ready(flash);
		if (ret)
			goto time_out;
		retlen += m.actual_length - m25p_cmdsz(flash);
	}
	offset += actual;

	flash->command[0] = OPCODE_AAI_WP;
	m25p_addr2cmd(flash, offset, flash->command);

	/* Write out most of the data here. */
	cmd_sz = m25p_cmdsz(flash);
	for (; actual < count - 1; actual += 2) {
		t[0].len = cmd_sz;
		/* write two bytes. */
		t[1].len = 2;
		t[1].tx_buf = buf + actual;

		spi_sync(flash->spi, &m);
		ret = wait_till_ready(flash);
		if (ret)
			goto time_out;
		retlen += m.actual_length - cmd_sz;
		cmd_sz = 1;
		offset += 2;
	}
	write_disable(flash);
	ret = wait_till_ready(flash);
	if (ret)
		goto time_out;

	/* Write out trailing byte if it exists. */
	if (actual != count) {
		write_enable(flash);
		flash->command[0] = OPCODE_BP;
		m25p_addr2cmd(flash, offset, flash->command);
		t[0].len = m25p_cmdsz(flash);
		t[1].len = 1;
		t[1].tx_buf = buf + actual;

		spi_sync(flash->spi, &m);
		ret = wait_till_ready(flash);
		if (ret)
			goto time_out;
		retlen += m.actual_length - m25p_cmdsz(flash);
		write_disable(flash);
	}

time_out:
	return retlen;
}
#endif

static void m25p80_info(struct device_d *dev)
{
	struct m25p		*flash = dev->priv;
	struct flash_info	*info = flash->info;

	printf("Flash type        : %s\n", flash->name);
	printf("Size              : %lldKiB\n", (long long)flash->size / 1024);
	printf("Number of sectors : %d\n", info->n_sectors);
	printf("Sector size       : %dKiB\n", info->sector_size / 1024);
	printf("\n");
}


/****************************************************************************/

/*
 * SPI device driver setup and teardown
 */

#define INFO(_jedec_id, _ext_id, _sector_size, _n_sectors, _flags)	\
	((unsigned long)&(struct flash_info) {				\
		.jedec_id = (_jedec_id),				\
		.ext_id = (_ext_id),					\
		.sector_size = (_sector_size),				\
		.n_sectors = (_n_sectors),				\
		.page_size = 256,					\
		.flags = (_flags),					\
	})

#define CAT25_INFO(_sector_size, _n_sectors, _page_size, _addr_width)	\
	((unsigned long)&(struct flash_info) {				\
		.sector_size = (_sector_size),				\
		.n_sectors = (_n_sectors),				\
		.page_size = (_page_size),				\
		.addr_width = (_addr_width),				\
		.flags = M25P_NO_ERASE,					\
	})

/* NOTE: double check command sets and memory organization when you add
 * more flash chips.  This current list focusses on newer chips, which
 * have been converging on command sets which including JEDEC ID.
 */
static const struct spi_device_id m25p_ids[] = {
	/* Atmel -- some are (confusingly) marketed as "DataFlash" */
	{ "at25fs010",  INFO(0x1f6601, 0, 32 * 1024,   4, SECT_4K) },
	{ "at25fs040",  INFO(0x1f6604, 0, 64 * 1024,   8, SECT_4K) },

	{ "at25df041a", INFO(0x1f4401, 0, 64 * 1024,   8, SECT_4K) },
	{ "at25df641",  INFO(0x1f4800, 0, 64 * 1024, 128, SECT_4K) },

	{ "at26f004",   INFO(0x1f0400, 0, 64 * 1024,  8, SECT_4K) },
	{ "at26df081a", INFO(0x1f4501, 0, 64 * 1024, 16, SECT_4K) },
	{ "at26df161a", INFO(0x1f4601, 0, 64 * 1024, 32, SECT_4K) },
	{ "at26df321",  INFO(0x1f4700, 0, 64 * 1024, 64, SECT_4K) },

	/* EON -- en25xxx */
	{ "en25f32", INFO(0x1c3116, 0, 64 * 1024,  64, SECT_4K) },
	{ "en25p32", INFO(0x1c2016, 0, 64 * 1024,  64, 0) },
	{ "en25p64", INFO(0x1c2017, 0, 64 * 1024, 128, 0) },

	/* Intel/Numonyx -- xxxs33b */
	{ "160s33b",  INFO(0x898911, 0, 64 * 1024,  32, 0) },
	{ "320s33b",  INFO(0x898912, 0, 64 * 1024,  64, 0) },
	{ "640s33b",  INFO(0x898913, 0, 64 * 1024, 128, 0) },

	/* Macronix */
	{ "mx25l4005a",  INFO(0xc22013, 0, 64 * 1024,   8, SECT_4K) },
	{ "mx25l8005",   INFO(0xc22014, 0, 64 * 1024,  16, 0) },
	{ "mx25l3205d",  INFO(0xc22016, 0, 64 * 1024,  64, 0) },
	{ "mx25l6405d",  INFO(0xc22017, 0, 64 * 1024, 128, 0) },
	{ "mx25l12805d", INFO(0xc22018, 0, 64 * 1024, 256, 0) },
	{ "mx25l12855e", INFO(0xc22618, 0, 64 * 1024, 256, 0) },
	{ "mx25l25635e", INFO(0xc22019, 0, 64 * 1024, 512, 0) },
	{ "mx25l25655e", INFO(0xc22619, 0, 64 * 1024, 512, 0) },

	/* Spansion -- single (large) sector size only, at least
	 * for the chips listed here (without boot sectors).
	 */
	{ "s25sl004a",  INFO(0x010212,      0,  64 * 1024,   8, 0) },
	{ "s25sl008a",  INFO(0x010213,      0,  64 * 1024,  16, 0) },
	{ "s25sl016a",  INFO(0x010214,      0,  64 * 1024,  32, 0) },
	{ "s25sl032a",  INFO(0x010215,      0,  64 * 1024,  64, 0) },
	{ "s25sl032p",  INFO(0x010215, 0x4d00,  64 * 1024,  64, SECT_4K) },
	{ "s25sl064a",  INFO(0x010216,      0,  64 * 1024, 128, 0) },
	{ "s25sl12800", INFO(0x012018, 0x0300, 256 * 1024,  64, 0) },
	{ "s25sl12801", INFO(0x012018, 0x0301,  64 * 1024, 256, 0) },
	{ "s25fl129p0", INFO(0x012018, 0x4d00, 256 * 1024,  64, 0) },
	{ "s25fl129p1", INFO(0x012018, 0x4d01,  64 * 1024, 256, 0) },
	{ "s25fl016k",  INFO(0xef4015,      0,  64 * 1024,  32, SECT_4K) },
	{ "s25fl064k",  INFO(0xef4017,      0,  64 * 1024, 128, SECT_4K) },

	/* SST -- large erase sizes are "overlays", "sectors" are 4K */
	{ "sst25vf040b", INFO(0xbf258d, 0, 64 * 1024,  8, SECT_4K) },
	{ "sst25vf080b", INFO(0xbf258e, 0, 64 * 1024, 16, SECT_4K) },
	{ "sst25vf016b", INFO(0xbf2541, 0, 64 * 1024, 32, SECT_4K) },
	{ "sst25vf032b", INFO(0xbf254a, 0, 64 * 1024, 64, SECT_4K) },
	{ "sst25wf512",  INFO(0xbf2501, 0, 64 * 1024,  1, SECT_4K) },
	{ "sst25wf010",  INFO(0xbf2502, 0, 64 * 1024,  2, SECT_4K) },
	{ "sst25wf020",  INFO(0xbf2503, 0, 64 * 1024,  4, SECT_4K) },
	{ "sst25wf040",  INFO(0xbf2504, 0, 64 * 1024,  8, SECT_4K) },

	/* ST Microelectronics -- newer production may have feature updates */
	{ "m25p05",  INFO(0x202010,  0,  32 * 1024,   2, 0) },
	{ "m25p10",  INFO(0x202011,  0,  32 * 1024,   4, 0) },
	{ "m25p20",  INFO(0x202012,  0,  64 * 1024,   4, 0) },
	{ "m25p40",  INFO(0x202013,  0,  64 * 1024,   8, 0) },
	{ "m25p80",  INFO(0x202014,  0,  64 * 1024,  16, 0) },
	{ "m25p16",  INFO(0x202015,  0,  64 * 1024,  32, 0) },
	{ "m25p32",  INFO(0x202016,  0,  64 * 1024,  64, 0) },
	{ "m25p64",  INFO(0x202017,  0,  64 * 1024, 128, 0) },
	{ "m25p128", INFO(0x202018,  0, 256 * 1024,  64, 0) },

	{ "m25p05-nonjedec",  INFO(0, 0,  32 * 1024,   2, 0) },
	{ "m25p10-nonjedec",  INFO(0, 0,  32 * 1024,   4, 0) },
	{ "m25p20-nonjedec",  INFO(0, 0,  64 * 1024,   4, 0) },
	{ "m25p40-nonjedec",  INFO(0, 0,  64 * 1024,   8, 0) },
	{ "m25p80-nonjedec",  INFO(0, 0,  64 * 1024,  16, 0) },
	{ "m25p16-nonjedec",  INFO(0, 0,  64 * 1024,  32, 0) },
	{ "m25p32-nonjedec",  INFO(0, 0,  64 * 1024,  64, 0) },
	{ "m25p64-nonjedec",  INFO(0, 0,  64 * 1024, 128, 0) },
	{ "m25p128-nonjedec", INFO(0, 0, 256 * 1024,  64, 0) },

	{ "m45pe10", INFO(0x204011,  0, 64 * 1024,    2, 0) },
	{ "m45pe80", INFO(0x204014,  0, 64 * 1024,   16, 0) },
	{ "m45pe16", INFO(0x204015,  0, 64 * 1024,   32, 0) },

	{ "m25pe80", INFO(0x208014,  0, 64 * 1024, 16,       0) },
	{ "m25pe16", INFO(0x208015,  0, 64 * 1024, 32, SECT_4K) },

	{ "m25px64", INFO(0x207117,  0, 64 * 1024, 128, 0) },

	/* Winbond -- w25x "blocks" are 64K, "sectors" are 4KiB */
	{ "w25x10", INFO(0xef3011, 0, 64 * 1024,  2,  SECT_4K) },
	{ "w25x20", INFO(0xef3012, 0, 64 * 1024,  4,  SECT_4K) },
	{ "w25x40", INFO(0xef3013, 0, 64 * 1024,  8,  SECT_4K) },
	{ "w25x80", INFO(0xef3014, 0, 64 * 1024,  16, SECT_4K) },
	{ "w25x16", INFO(0xef3015, 0, 64 * 1024,  32, SECT_4K) },
	{ "w25x32", INFO(0xef3016, 0, 64 * 1024,  64, SECT_4K) },
	{ "w25q32", INFO(0xef4016, 0, 64 * 1024,  64, SECT_4K) },
	{ "w25x64", INFO(0xef3017, 0, 64 * 1024, 128, SECT_4K) },
	{ "w25q64", INFO(0xef4017, 0, 64 * 1024, 128, SECT_4K) },

	/* Catalyst / On Semiconductor -- non-JEDEC */
	{ "cat25c11", CAT25_INFO(  16, 8, 16, 1) },
	{ "cat25c03", CAT25_INFO(  32, 8, 16, 2) },
	{ "cat25c09", CAT25_INFO( 128, 8, 32, 2) },
	{ "cat25c17", CAT25_INFO( 256, 8, 32, 2) },
	{ "cat25128", CAT25_INFO(2048, 8, 64, 2) },
	{ },
};

static const struct spi_device_id *jedec_probe(struct spi_device *spi)
{
	int			tmp;
	u8			code = OPCODE_RDID;
	u8			id[5];
	u32			jedec;
	u16			ext_jedec;
	struct flash_info	*info;

	/* JEDEC also defines an optional "extended device information"
	 * string for after vendor-specific data, after the three bytes
	 * we use here.  Supporting some chips might require using it.
	 */
	spi_write_then_read(spi, &code, 1, id, 5);

	jedec = id[0];
	jedec = jedec << 8;
	jedec |= id[1];
	jedec = jedec << 8;
	jedec |= id[2];

	ext_jedec = id[3] << 8 | id[4];

	for (tmp = 0; tmp < ARRAY_SIZE(m25p_ids) - 1; tmp++) {
		info = (void *)m25p_ids[tmp].driver_data;
		if (info->jedec_id == jedec) {
			if (info->ext_id != 0 && info->ext_id != ext_jedec)
				continue;
			return &m25p_ids[tmp];
		}
	}
	dev_err(&spi->dev, "unrecognized JEDEC id %06x\n", jedec);

	return NULL;
}

static struct file_operations m25p80_ops = {
	.read   = m25p80_read,
	.write  = m25p80_write,
	.erase  = m25p80_erase,
	.lseek  = dev_lseek_default,
};

/*
 * board specific setup should have ensured the SPI clock used here
 * matches what the READ command supports, at least until this driver
 * understands FAST_READ (for clocks over 25 MHz).
 */
static int m25p_probe(struct device_d *dev)
{
	struct spi_device *spi = (struct spi_device *)dev->type_data;
	const struct spi_device_id	*id = NULL;
	struct flash_info		*info = NULL;
	struct flash_platform_data	*data;
	struct m25p			*flash;
	unsigned			i;
	unsigned			do_jdec_probe = 1;

	/* Platform data helps sort out which chip type we have, as
	 * well as how this board partitions it.  If we don't have
	 * a chip ID, try the JEDEC id commands; they'll work for most
	 * newer chips, even if we don't recognize the particular chip.
	 */
	data = dev->platform_data;

	if (data && data->type) {
		const struct spi_device_id *plat_id;

		for (i = 0; i < ARRAY_SIZE(m25p_ids) - 1; i++) {
			plat_id = &m25p_ids[i];
			if (strcmp(data->type, plat_id->name))
				continue;
			break;
		}

		if (i < ARRAY_SIZE(m25p_ids) - 1) {
			id = plat_id;
			info = (void *)id->driver_data;
			/* If flash type is provided but the memory is not
			 * JEDEC compliant, don't try to probe the JEDEC id */
			if (!info->jedec_id)
				do_jdec_probe = 0;
		} else
			dev_warn(&spi->dev, "unrecognized id %s\n", data->type);
	}

	if (do_jdec_probe) {
		const struct spi_device_id *jid;

		jid = jedec_probe(spi);
		if (!jid) {
			return -ENODEV;
		} else if (jid != id) {

			/*
			 * JEDEC knows better, so overwrite platform ID. We
			 * can't trust partitions any longer, but we'll let
			 * mtd apply them anyway, since some partitions may be
			 * marked read-only, and we don't want to lose that
			 * information, even if it's not 100% accurate.
			 */
			if (id)
				dev_warn(dev, "found %s, expected %s\n",
					jid->name, id->name);

			id = jid;
			info = (void *)jid->driver_data;
		}
	}

	flash = xzalloc(sizeof *flash);
	flash->command = xmalloc(MAX_CMD_SIZE);

	flash->spi = spi;
	dev->priv = (void *)flash;
	/*
	 * Atmel, SST and Intel/Numonyx serial flash tend to power
	 * up with the software protection bits set
	 */

	if (info->jedec_id >> 16 == 0x1f ||
	    info->jedec_id >> 16 == 0x89 ||
	    info->jedec_id >> 16 == 0xbf) {
		write_enable(flash);
		write_sr(flash, 0);
	}

	flash->name = (char *)id->name;
	flash->info = info;
	flash->size = info->sector_size * info->n_sectors;
	flash->erasesize = info->sector_size;
	flash->sector_size = info->sector_size;
	flash->cdev.size = info->sector_size * info->n_sectors;
	flash->cdev.dev = dev;
	flash->cdev.ops = &m25p80_ops;
	flash->cdev.priv = flash;

	if (data && data->name)
		flash->cdev.name = asprintf("%s%d", data->name, dev->id);
	else
		flash->cdev.name = asprintf("%s", (char *)dev_name(&spi->dev));

#ifdef CONFIG_MTD_SST25L
	/* sst flash chips use AAI word program */
	if (info->jedec_id >> 16 == 0xbf)
		m25p80_ops.write = sst_write;
	else
#endif
		m25p80_ops.write = m25p80_write;

	/* prefer "small sector" erase if possible */
	if (info->flags & SECT_4K) {
		flash->erase_opcode_4k = OPCODE_BE_4K;
		flash->erase_opcode = OPCODE_SE;
		flash->erasesize = 4096;
	} else {
		flash->erase_opcode = OPCODE_SE;
	}

	flash->page_size = info->page_size;

	if (info->addr_width)
		flash->addr_width = info->addr_width;
	else {
		/* enable 4-byte addressing if the device exceeds 16MiB */
		if (flash->size > 0x1000000) {
			flash->addr_width = 4;
			set_4byte(flash, 1);
		} else
			flash->addr_width = 3;
	}

	dev_info(dev, "%s (%lld Kbytes)\n", id->name, (long long)flash->size >> 10);

	devfs_create(&flash->cdev);

	return 0;
}

static struct driver_d epcs_flash_driver = {
	.name  = "m25p",
	.probe = m25p_probe,
	.info = m25p80_info,
};

static int epcs_init(void)
{
	register_driver(&epcs_flash_driver);
	return 0;
}

device_initcall(epcs_init);
