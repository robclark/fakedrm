/*
 * Userspace DRM emulation library - file management
 *
 * Copyright 2014 Tomasz Figa <tomasz.figa@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xf86drm.h>

#include "file.h"
#include "gem.h"
#include "kms.h"
#include "utils.h"

/*
 * Dummy file descriptors
 */

static struct locked_hash_table file_table;

void __file_get(struct fakedrm_file_desc *file)
{
	__sync_add_and_fetch(&file->refcnt, 1);
}

void __file_put(struct fakedrm_file_desc *file)
{
	if (__sync_sub_and_fetch(&file->refcnt, 1) == 0) {
		hash_destroy(&file->bo_table);
		free(file);
	}
}

/* DRM device IOCTLs */

#define DUMMY_VERSION_MAJOR		1
#define DUMMY_VERSION_MINOR		0
#define DUMMY_VERSION_PATCH		0
#define DUMMY_VERSION_NAME		"exynos"
#define DUMMY_VERSION_DATE		"20110530"
#define DUMMY_VERSION_DESC		"Samsung SoC DRM"

static int dummy_version(void *arg)
{
	struct drm_version *version = arg;

	version->version_major = DUMMY_VERSION_MAJOR;
	version->version_minor = DUMMY_VERSION_MINOR;
	version->version_patchlevel = DUMMY_VERSION_PATCH;

	version->name_len = strlen(DUMMY_VERSION_NAME);
	version->date_len = strlen(DUMMY_VERSION_DATE);
	version->desc_len = strlen(DUMMY_VERSION_DESC);

	if (!version->name || !version->date || !version->desc)
		return 0;

	strcpy(version->name, DUMMY_VERSION_NAME);
	strcpy(version->date, DUMMY_VERSION_DATE);
	strcpy(version->desc, DUMMY_VERSION_DESC);

	return 0;
}

static int dummy_get_unique(void *arg)
{
	return -EINVAL;
}

static uint32_t magic;

static int dummy_get_magic(void *arg)
{
	struct drm_auth *auth = arg;

	auth->magic = magic;

	return 0;
}

static int dummy_auth_magic(void *arg)
{
	struct drm_auth *auth = arg;

	magic = auth->magic;

	return 0;
}

/*
 * Implementation of file operations for emulated DRM devices
 */
int file_open(const char *pathname, int flags, mode_t mode)
{
	struct fakedrm_file_desc *file;
	sigset_t oldmask;
	int fd;

	pthread_sigmask(SIG_BLOCK, &captured_signals, &oldmask);

	fd = open_real("/dev/null", O_RDWR, 0);
	if (!fd) {
		ERROR_MSG("failed to open '/dev/null': %s",
			strerror(errno));
		goto err_sigmask;
	}

	file = calloc(1, sizeof(*file));
	if (!file) {
		ERROR_MSG("failed to allocate file descriptor");
		errno = ENOMEM;
		goto err_close;
	}

	hash_create(&file->bo_table);
	file->fd = fd;

	file_get(file);
	hash_insert(&file_table, fd, file);

	pthread_sigmask(SIG_SETMASK, &oldmask, NULL);

	return fd;

err_close:
	close_real(fd);
err_sigmask:
	pthread_sigmask(SIG_SETMASK, &oldmask, NULL);

	return -1;
}

static void __file_close(struct fakedrm_file_desc *file)
{
	unsigned long key;
	void *value;
	int ret;

	ret = drmHashFirst(file->bo_table.table, &key, &value);
	while (ret) {
		struct fakedrm_bo_handle *handle = value;

		bo_handle_put(handle);

		ret = drmHashNext(file->bo_table.table, &key, &value);
	}
}

void file_close(struct fakedrm_file_desc *file)
{
	sigset_t oldmask;

	pthread_sigmask(SIG_BLOCK, &captured_signals, &oldmask);

	hash_remove(&file_table, file->fd);
	__file_close(file);
	close_real(file->fd);
	file->fd = -1;
	file_put(file);

	pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
}

int file_ioctl(struct fakedrm_file_desc *file, unsigned long request, void *arg)
{
	int ret;

	ret = exynos_ioctl(file, request, arg);
	if (ret != -ENOTTY) {
		if (!ret)
			return 0;

		errno = -ret;
		return -1;
	}

	switch (request) {
	/* Core IOCTLs */
	case DRM_IOCTL_VERSION:
		ret = dummy_version(arg);
		break;
	case DRM_IOCTL_GET_UNIQUE:
		ret = dummy_get_unique(arg);
		break;
	case DRM_IOCTL_GET_MAGIC:
		ret = dummy_get_magic(arg);
		break;
	case DRM_IOCTL_AUTH_MAGIC:
		ret = dummy_auth_magic(arg);
		break;

	/* Mode setting IOCTLs */
	case DRM_IOCTL_MODE_GETRESOURCES:
		ret = dummy_mode_getresources(arg);
		break;
	case DRM_IOCTL_MODE_GETCRTC:
		ret = dummy_mode_getcrtc(arg);
		break;
	case DRM_IOCTL_MODE_SETCRTC:
		ret = dummy_mode_setcrtc(arg);
		break;
	case DRM_IOCTL_MODE_GETENCODER:
		ret = dummy_mode_getencoder(arg);
		break;
	case DRM_IOCTL_MODE_GETCONNECTOR:
		ret = dummy_mode_getconnector(arg);
		break;
	case DRM_IOCTL_MODE_ADDFB:
		ret = dummy_mode_addfb(arg);
		break;
	case DRM_IOCTL_MODE_RMFB:
		ret = dummy_mode_rmfb(arg);
		break;
	case DRM_IOCTL_MODE_PAGE_FLIP:
		ret = dummy_mode_page_flip(arg);
		break;
	case DRM_IOCTL_MODE_MAP_DUMB:
		ret = dummy_mode_map_dumb(file, arg);
		break;

	/* Generic GEM IOCTLs */
	case DRM_IOCTL_GEM_OPEN:
		ret = dummy_gem_open(file, arg);
		break;
	case DRM_IOCTL_GEM_CLOSE:
		ret = dummy_gem_close(file, arg);
		break;
	case DRM_IOCTL_GEM_FLINK:
		ret = dummy_gem_flink(file, arg);
		break;

	default:
		ERROR_MSG("%s: Not implemented dummy handler for IOCTL %08lx",
			__func__, request);
		ret = -EINVAL;
	}

	DEBUG_MSG("%s: IOCTL %08lx, ret=%d", __func__, request, ret);

	if (ret) {
		errno = -ret;
		return -1;
	}

	return 0;
}

void *file_mmap(struct fakedrm_file_desc *file, void *addr, size_t length,
		int prot, int flags, off_t offset)
{
	void *out_addr = NULL;
	uint32_t handle;
	int ret;

	if (offset % 4096)
		return MAP_FAILED;

	handle = offset / 4096;
	if (!handle)
		return MAP_FAILED;

	ret = bo_map(file, handle, &out_addr);
	if (ret)
		return MAP_FAILED;

	return out_addr;
}

int file_fstat(struct fakedrm_file_desc *file, int ver, struct stat *buf)
{
	/* TODO: Fake stat info */
	return __fxstat_real(ver, file->fd, buf);
}

struct fakedrm_file_desc *file_lookup(int fd)
{
	return hash_lookup(&file_table, fd);
}

/*
 * VT IOCTL emulation
 */

#ifndef K_OFF
#define K_OFF 0x4
#endif

#ifndef KDSKBMUTE
#define KDSKBMUTE 0x4B51
#endif

static struct vt_stat vt_state = {
	.v_active = 1,
};

static struct vt_mode vt_mode;

static int vt_getstate(void *arg)
{
	memcpy(arg, &vt_state, sizeof(vt_state));

	return 0;
}

static int vt_openqry(void *arg)
{
	int *free_vt = arg;

	if (free_vt)
		*free_vt = 1;

	return 0;
}

static int vt_getmode(void *arg)
{
	if (arg)
		memcpy(arg, &vt_mode, sizeof(vt_mode));

	return 0;
}

static int vt_setmode(void *arg)
{
	if (arg)
		memcpy(&vt_mode, arg, sizeof(vt_mode));

	return 0;
}

int vt_ioctl(unsigned long request, void *arg)
{
	int ret;

	switch (request) {
	case VT_GETSTATE:
		ret = vt_getstate(arg);
		break;

	case VT_OPENQRY:
		ret = vt_openqry(arg);
		break;

	case VT_GETMODE:
		ret = vt_getmode(arg);
		break;

	case VT_SETMODE:
		ret = vt_setmode(arg);
		break;

	case KDSETMODE:
	case KDGKBMODE:
	case KDSKBMUTE:
	case VT_ACTIVATE:
	case VT_WAITACTIVE:
		ret = 0;
		break;

	default:
		ERROR_MSG("%s: Not implemented dummy handler for IOCTL %08lx",
			__func__, request);
		ret = -EINVAL;
	}

	DEBUG_MSG("%s: IOCTL %08lx, ret=%d", __func__, request, ret);

	if (ret) {
		errno = -ret;
		return -1;
	}

	return 0;
}

/* Init/clean-up */

void file_init(void)
{
	hash_create(&file_table);
}

void file_cleanup(void)
{
	unsigned long key;
	void *value;
	int ret;

	ret = drmHashFirst(file_table.table, &key, &value);
	while (ret) {
		struct fakedrm_file_desc *file = value;

		__file_close(file);

		ret = drmHashNext(file_table.table, &key, &value);
	}
}
