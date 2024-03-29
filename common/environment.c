/*
 * Copyright (c) 2007 Sascha Hauer <s.hauer@pengutronix.de>, Pengutronix
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/**
 * @file
 * @brief Environment handling support (host and target)
 *
 * Important: This file will also be used on the host to create
 * the default environment when building the barebox binary. So
 * do not add any new barebox related functions here!
 */

#ifdef __BAREBOX__
#include <common.h>
#include <command.h>
#include <malloc.h>
#include <errno.h>
#include <fs.h>
#include <fcntl.h>
#include <envfs.h>
#include <xfuncs.h>
#include <libbb.h>
#include <libgen.h>
#include <environment.h>
#else
# define errno_str(x) ("void")
#define EXPORT_SYMBOL(x)
#endif

char *default_environment_path = "/dev/env0";

int file_size_action(const char *filename, struct stat *statbuf,
			    void *userdata, int depth)
{
	struct action_data *data = userdata;

	data->writep += sizeof(struct envfs_inode);
	data->writep += PAD4(strlen(filename) + 1 - strlen(data->base));
	data->writep += sizeof(struct envfs_inode_end);
	if (S_ISLNK(statbuf->st_mode)) {
		char path[PATH_MAX];

		memset(path, 0, PATH_MAX);

		if (readlink(filename, path, PATH_MAX - 1) < 0) {
			perror("read");
			return 0;
		}
		data->writep += PAD4(strlen(path) + 1);
	} else {
		data->writep += PAD4(statbuf->st_size);
	}

	return 1;
}

int file_save_action(const char *filename, struct stat *statbuf,
			    void *userdata, int depth)
{
	struct action_data *data = userdata;
	struct envfs_inode *inode;
	struct envfs_inode_end *inode_end;
	int fd;
	int namelen = strlen(filename) + 1 - strlen(data->base);

	inode = (struct envfs_inode*)data->writep;
	inode->magic = ENVFS_32(ENVFS_INODE_MAGIC);
	inode->headerlen = ENVFS_32(PAD4(namelen + sizeof(struct envfs_inode_end)));
	data->writep += sizeof(struct envfs_inode);

	strcpy(data->writep, filename + strlen(data->base));
	data->writep += PAD4(namelen);
	inode_end = (struct envfs_inode_end*)data->writep;
	data->writep += sizeof(struct envfs_inode_end);
	inode_end->magic = ENVFS_32(ENVFS_INODE_END_MAGIC);
	inode_end->mode = ENVFS_32(S_IRWXU | S_IRWXG | S_IRWXO);

	if (S_ISLNK(statbuf->st_mode)) {
		char path[PATH_MAX];
		int len;

		memset(path, 0, PATH_MAX);

		if (readlink(filename, path, PATH_MAX - 1) < 0) {
			perror("read");
			goto out;
		}
		len = strlen(path) + 1;

		inode_end->mode |= ENVFS_32(S_IFLNK);

		memcpy(data->writep, path, len);
		inode->size = ENVFS_32(len);
		data->writep += PAD4(len);
		debug("handling symlink %s size %ld namelen %d headerlen %d\n", filename + strlen(data->base),
			len, namelen, ENVFS_32(inode->headerlen));
	} else {
		debug("handling file %s size %ld namelen %d headerlen %d\n", filename + strlen(data->base),
			statbuf->st_size, namelen, ENVFS_32(inode->headerlen));

		inode->size = ENVFS_32(statbuf->st_size);
		fd = open(filename, O_RDONLY);
		if (fd < 0) {
			printf("Open %s %s\n", filename, errno_str());
			goto out;
		}

		if (read(fd, data->writep, statbuf->st_size) < statbuf->st_size) {
			perror("read");
			goto out;
		}
		close(fd);

		data->writep += PAD4(statbuf->st_size);
	}

out:
	return 1;
}

/**
 * Make the current environment persistent
 * @param[in] filename where to store
 * @param[in] dirname what to store (all files in this dir)
 * @return 0 on success, anything else in case of failure
 *
 * Note: This function will also be used on the host! See note in the header
 * of this file.
 */
int envfs_save(char *filename, char *dirname)
{
	struct envfs_super *super;
	int envfd, size, ret;
	struct action_data data;
	void *buf = NULL;

	data.writep = NULL;
	data.base = dirname;

	/* first pass: calculate size */
	recursive_action(dirname, ACTION_RECURSE, file_size_action,
			 NULL, &data, 0);

	size = (unsigned long)data.writep;

	buf = xzalloc(size + sizeof(struct envfs_super));
	data.writep = buf + sizeof(struct envfs_super);

	super = (struct envfs_super *)buf;
	super->magic = ENVFS_32(ENVFS_MAGIC);
	super->major = ENVFS_MAJOR;
	super->minor = ENVFS_MINOR;
	super->size = ENVFS_32(size);

	/* second pass: copy files to buffer */
	recursive_action(dirname, ACTION_RECURSE, file_save_action,
			 NULL, &data, 0);

	super->crc = ENVFS_32(crc32(0, buf + sizeof(struct envfs_super), size));
	super->sb_crc = ENVFS_32(crc32(0, buf, sizeof(struct envfs_super) - 4));

	envfd = open(filename, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
	if (envfd < 0) {
		printf("Open %s %s\n", filename, errno_str());
		ret = envfd;
		goto out1;
	}

	if (write(envfd, buf, size  + sizeof(struct envfs_super)) <
			sizeof(struct envfs_super)) {
		perror("write");
		ret = -1;	/* FIXME */
		goto out;
	}

	ret = 0;

out:
	close(envfd);
out1:
	free(buf);
	return ret;
}
EXPORT_SYMBOL(envfs_save);

/**
 * Restore the last environment into the current one
 * @param[in] filename from where to restore
 * @param[in] dir where to store the last content
 * @return 0 on success, anything else in case of failure
 *
 * Note: This function will also be used on the host! See note in the header
 * of this file.
 */
int envfs_load(char *filename, char *dir)
{
	struct envfs_super super;
	void *buf = NULL, *buf_free = NULL;
	int envfd;
	int fd, ret = 0;
	char *str, *tmp;
	int headerlen_full;
	unsigned long size;
	/* for envfs < 1.0 */
	struct envfs_inode_end inode_end_dummy;

	inode_end_dummy.mode = ENVFS_32(S_IRWXU | S_IRWXG | S_IRWXO);
	inode_end_dummy.magic = ENVFS_32(ENVFS_INODE_END_MAGIC);

	envfd = open(filename, O_RDONLY);
	if (envfd < 0) {
		printf("Open %s %s\n", filename, errno_str());
		return -1;
	}

	/* read superblock */
	ret = read(envfd, &super, sizeof(struct envfs_super));
	if ( ret < sizeof(struct envfs_super)) {
		perror("read");
		ret = -errno;
		goto out;
	}

	if ( ENVFS_32(super.magic) != ENVFS_MAGIC) {
		printf("envfs: wrong magic on %s\n", filename);
		ret = -EIO;
		goto out;
	}

	if (crc32(0, (unsigned char *)&super, sizeof(struct envfs_super) - 4)
		   != ENVFS_32(super.sb_crc)) {
		printf("wrong crc on env superblock\n");
		ret = -EIO;
		goto out;
	}

	size = ENVFS_32(super.size);
	buf = xmalloc(size);
	buf_free = buf;
	ret = read(envfd, buf, size);
	if (ret < size) {
		perror("read");
		ret = -errno;
		goto out;
	}

	if (crc32(0, (unsigned char *)buf, size)
		     != ENVFS_32(super.crc)) {
		printf("wrong crc on env\n");
		ret = -EIO;
		goto out;
	}

	if (super.major < ENVFS_MAJOR)
		printf("envfs version %d.%d loaded into %d.%d\n",
			super.major, super.minor,
			ENVFS_MAJOR, ENVFS_MINOR);

	while (size) {
		struct envfs_inode *inode;
		struct envfs_inode_end *inode_end;
		uint32_t inode_size, inode_headerlen, namelen;

		inode = (struct envfs_inode *)buf;
		buf += sizeof(struct envfs_inode);

		if (ENVFS_32(inode->magic) != ENVFS_INODE_MAGIC) {
			printf("envfs: wrong magic on %s\n", filename);
			ret = -EIO;
			goto out;
		}
		inode_size = ENVFS_32(inode->size);
		inode_headerlen = ENVFS_32(inode->headerlen);
		namelen = strlen(inode->data) + 1;
		if (super.major < 1)
			inode_end = &inode_end_dummy;
		else
			inode_end = (struct envfs_inode_end *)(buf + PAD4(namelen));

		debug("loading %s size %d namelen %d headerlen %d\n", inode->data,
			inode_size, namelen, inode_headerlen);

		str = concat_path_file(dir, inode->data);
		tmp = strdup(str);
		make_directory(dirname(tmp));
		free(tmp);

		headerlen_full = PAD4(inode_headerlen);
		buf += headerlen_full;

		if (ENVFS_32(inode_end->magic) != ENVFS_INODE_END_MAGIC) {
			printf("envfs: wrong inode_end_magic on %s\n", filename);
			ret = -EIO;
			goto out;
		}

		if (S_ISLNK(ENVFS_32(inode_end->mode))) {
			debug("symlink: %s -> %s\n", str, (char*)buf);
			if (symlink((char*)buf, str) < 0) {
				printf("symlink: %s -> %s :", str, (char*)buf);
				perror("");
			}
			free(str);
		} else {
			fd = open(str, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			free(str);
			if (fd < 0) {
				printf("Open %s\n", errno_str());
				ret = fd;
				goto out;
			}

			ret = write(fd, buf, inode_size);
			if (ret < inode_size) {
				perror("write");
				ret = -errno;
				close(fd);
				goto out;
			}
			close(fd);
		}

		buf += PAD4(inode_size);
		size -= headerlen_full + PAD4(inode_size) +
				sizeof(struct envfs_inode);
	}

	ret = 0;
out:
	close(envfd);
	if (buf_free)
		free(buf_free);
	return ret;
}
