/*
 * VFIO test suite
 *
 * Copyright (C) 2012-2025, Red Hat Inc.
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <dirent.h>

#include <linux/iommufd.h>
#include <linux/pci_regs.h>
#include <linux/vfio.h>

#include "utils.h"

int verbose = -1;

static void __attribute__((constructor)) init_verbose(void)
{
	verbose = !!getenv("VFIO_VERBOSE");
}

void hexdump(const void *data, size_t len)
{
	const unsigned char *p = data;
	size_t i;

	for (i = 0; i < len; i++) {
		if (i % 16 == 0)
			printf("\n%08zx: ", i);
		else if (i % 4 == 0)
			printf(" ");
		printf("%02x", p[i]);
	}
	printf("\n");
}

/* minimal, dependency-free replacement for uuid_parse() */
int parse_vf_token(const char *token, unsigned char bytes[VF_TOKEN_SIZE])
{
	if (sscanf(token,
		   "%2hhx%2hhx%2hhx%2hhx-%2hhx%2hhx-%2hhx%2hhx-"
		   "%2hhx%2hhx-%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx",
		   &bytes[0], &bytes[1], &bytes[2], &bytes[3],
		   &bytes[4], &bytes[5], &bytes[6], &bytes[7],
		   &bytes[8], &bytes[9], &bytes[10], &bytes[11],
		   &bytes[12], &bytes[13], &bytes[14], &bytes[15]) != VF_TOKEN_SIZE)
		return -1;

	return 0;
}

int vfio_pci_is_vf(const char *devname)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path),
		 "/sys/bus/pci/devices/%s/physfn", devname);
	return !access(path, F_OK);
}

int vfio_pci_is_vga(const char *devname)
{
	char path[PATH_MAX];
	FILE *f;
	unsigned int class = 0;

	snprintf(path, sizeof(path),
		 "/sys/bus/pci/devices/%s/class", devname);
	f = fopen(path, "r");
	if (!f)
		return 0;
	fscanf(f, "%x", &class);
	fclose(f);
	return (class >> 8) == 0x0300;
}

int vfio_pci_is_d3(const char *devname)
{
	char path[PATH_MAX];
	FILE *f;
	char state[16] = "";

	snprintf(path, sizeof(path),
		 "/sys/bus/pci/devices/%s/power_state", devname);
	f = fopen(path, "r");
	if (!f)
		return 0;
	fscanf(f, "%15s", state);
	fclose(f);
	return !strncmp(state, "D3", 2);
}

long slab_sunreclaim_kb(void)
{
	FILE *f;
	char line[128];
	long val = 0;

	f = fopen("/proc/meminfo", "r");
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "SUnreclaim: %ld kB", &val) == 1)
			break;
	fclose(f);
	return val;
}

#define VFIO_DMA_ENTRY_LIMIT_PATH \
	"/sys/module/vfio_iommu_type1/parameters/dma_entry_limit"

long vfio_dma_entry_limit(void)
{
	FILE *f;
	long val = 0;

	f = fopen(VFIO_DMA_ENTRY_LIMIT_PATH, "r");
	if (!f)
		return 0;
	fscanf(f, "%ld", &val);
	fclose(f);
	return val;
}

int vfio_dma_entry_limit_check(unsigned long nr_mappings)
{
	long limit = vfio_dma_entry_limit();

	if (!limit || nr_mappings <= limit)
		return 0;

	printf("Need %lu mappings but dma_entry_limit is %ld\n",
	       nr_mappings, limit);
	printf("increase with: echo %lu > " VFIO_DMA_ENTRY_LIMIT_PATH "\n",
	       nr_mappings);
	return -1;
}

int vfio_noiommu_enabled(void)
{
	FILE *f;
	char val;

	f = fopen("/sys/module/vfio/parameters/enable_unsafe_noiommu_mode", "r");
	if (!f)
		return 0;
	val = fgetc(f);
	fclose(f);
	return val == 'Y';
}

long hugepages_free(void)
{
	FILE *f;
	long val = 0;

	f = fopen("/sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages", "r");
	if (!f)
		return 0;
	fscanf(f, "%ld", &val);
	fclose(f);
	return val;
}

unsigned int vfio_pci_vendor(const char *devname)
{
	char path[PATH_MAX];
	FILE *f;
	unsigned int vendor = 0;

	snprintf(path, sizeof(path),
		 "/sys/bus/pci/devices/%s/vendor", devname);
	f = fopen(path, "r");
	if (!f)
		return 0;
	fscanf(f, "%x", &vendor);
	fclose(f);
	return vendor;
}

unsigned int vfio_pci_device(const char *devname)
{
	char path[PATH_MAX];
	FILE *f;
	unsigned int device = 0;

	snprintf(path, sizeof(path),
		 "/sys/bus/pci/devices/%s/device", devname);
	f = fopen(path, "r");
	if (!f)
		return 0;
	fscanf(f, "%x", &device);
	fclose(f);
	return device;
}

const char *pci_sysfs_attr(const char *devname, const char *attr,
			   char *buf, size_t len)
{
	char path[PATH_MAX];
	int fd, n;

	snprintf(path, sizeof(path),
		 "/sys/bus/pci/devices/%s/%s", devname, attr);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		snprintf(buf, len, "unknown");
		return buf;
	}
	n = read(fd, buf, len - 1);
	close(fd);
	if (n <= 0) {
		snprintf(buf, len, "unknown");
		return buf;
	}
	buf[n - 1] = '\0';
	return buf;
}

int vfio_device_iommufd_getfd(const char *devname)
{
	int  domain, bus, dev, func;
	char path[PATH_MAX];
	char *vfio_path = NULL;
	DIR *dir = NULL;
	struct dirent *dent;
	int ret;

	ret = sscanf(devname, "%04x:%02x:%02x.%d", &domain, &bus, &dev, &func);
	if (ret != 4) {
		printf("Invalid PCI device identifier \"%s\"\n", devname);
		return -1;
	}

	snprintf(path, sizeof(path),
		 "/sys/bus/pci/devices/%04x:%02x:%02x.%01x/vfio-dev",
		 domain, bus, dev, func);

	dir = opendir(path);
	if (!dir) {
		printf("couldn't open directory %s\n", path);
		return -1;
	}

	while ((dent = readdir(dir))) {
		if (!strncmp(dent->d_name, "vfio", 4)) {
			snprintf(path, sizeof(path),
				 "/dev/vfio/devices/%s", dent->d_name);
			vfio_path = path;
			break;
		}
	}
	closedir(dir);

	if (!vfio_path) {
		printf("failed to find vfio-dev/vfioX");
		return -1;
	}

	ret = open(path, O_RDWR);
	if (ret < 0) {
		printf("Failed to open %s, %d (%s)\n",
		       path, ret, strerror(errno));
	}

	return ret;
}

int vfio_device_iommufd_attach_with_token(int iommufd, const char *devname,
			       int *device_out, int *ioas_id_out, const char *token)
{
	int device, ret;

	struct vfio_device_bind_iommufd bind = {
		.argsz = sizeof(bind),
		.iommufd = iommufd,
	};
	struct iommu_ioas_alloc alloc_data = {
		.size = sizeof(alloc_data),
	};
	struct vfio_device_attach_iommufd_pt attach_data = {
		.argsz = sizeof(attach_data),
	};

	device = vfio_device_iommufd_getfd(devname);
	if (device < 0)
		return -1;

	if (token) {
		unsigned char token_bytes[VF_TOKEN_SIZE];

		if (parse_vf_token(token, token_bytes)) {
			fprintf(stderr, "Invalid VF token: %s\n", token);
			return -1;
		}

		bind.flags |= VFIO_DEVICE_BIND_FLAG_TOKEN;
		bind.token_uuid_ptr = (uintptr_t)token_bytes;
	}

	ret = ioctl(device, VFIO_DEVICE_BIND_IOMMUFD, &bind);
	if (ret < 0) {
		printf("Failed VFIO_DEVICE_BIND_IOMMUFD %s: %s\n",
		       devname, strerror(errno));
		return -1;
	}

	printf("%s: bind to IOMMUFD %d with dev_id %d\n",
	       devname, iommufd, bind.out_devid);

	ret = ioctl(iommufd, IOMMU_IOAS_ALLOC, &alloc_data);
	if (ret < 0) {
		printf("Failed IOMMU_IOAS_ALLOC: %s\n", strerror(errno));
		return -1;
	}

	attach_data.pt_id = alloc_data.out_ioas_id;
	ret = ioctl(device, VFIO_DEVICE_ATTACH_IOMMUFD_PT, &attach_data);
	if (ret < 0) {
		printf("Failed VFIO_DEVICE_ATTACH_IOMMUFD_PT %s: %s\n",
		       devname, strerror(errno));
		return -1;
	}

	printf("%s: attached ioas %d hwpt %d\n",
	       devname, alloc_data.out_ioas_id, attach_data.pt_id);

	*device_out = device;
	*ioas_id_out = alloc_data.out_ioas_id;
	return 0;
}

int vfio_device_get_groupid(const char *devname)
{
	int  domain, bus, dev, func;
	char group_path[PATH_MAX];
	char tmp[PATH_MAX];
	char *group_name = NULL;
	int ret, groupid;
	ssize_t len;

	ret = sscanf(devname, "%04x:%02x:%02x.%d", &domain, &bus, &dev, &func);
	if (ret != 4) {
		printf("Invalid PCI device identifier \"%s\"\n", devname);
		return -1;
	}

	snprintf(tmp, sizeof(tmp),
		 "/sys/bus/pci/devices/%04x:%02x:%02x.%01x/iommu_group",
		 domain, bus, dev, func);

	len = readlink(tmp, group_path, sizeof(group_path));
	if (len <= 0 || len >= sizeof(group_path)) {
		printf("%s: no iommu_group found\n", devname);
		return -1;
	}

	group_path[len] = 0;

	group_name = basename(group_path);
	if (sscanf(group_name, "%d", &groupid) != 1) {
		printf("failed to read %s", group_path);
		return -1;
	}

	printf("Using device %04x:%02x:%02x.%d in IOMMU group %d\n",
               domain, bus, dev, func, groupid);
	return groupid;
}

int vfio_group_open(int groupid, bool noiommu)
{
	int fd;
	char path[PATH_MAX];
	int ret;
	struct vfio_group_status status = { .argsz = sizeof(status) };

	snprintf(path, sizeof(path), "/dev/vfio/%s%d",
		 noiommu ? "noiommu-" : "", groupid);
	fd = open(path, O_RDWR);
	if (fd < 0) {
		printf("Failed to open %s, %d (%s)\n",
		       path, fd, strerror(errno));
		return -1;
	}

	ret = ioctl(fd, VFIO_GROUP_GET_STATUS, &status);
	if (ret) {
		printf("failed to get group %d status: %d (%s)\n",
		       groupid, ret, strerror(errno));
		return -1;
	}

	if (!(status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		printf("group %d is not viable", groupid);
		return -1;
	}

	return fd;
}

static int vfio_group_set_container(int group, int container, int iommu_type)
{
	int ret;
	bool noiommu = VFIO_NOIOMMU_IOMMU == iommu_type;

	struct vfio_group_status group_status = {
		.argsz = sizeof(group_status)
	};

	ret = ioctl(group, VFIO_GROUP_GET_STATUS, &group_status);
	if (ret) {
		printf("ioctl(VFIO_GROUP_GET_STATUS) failed: %d (%s)\n",
		       ret, strerror(errno));
		return ret;
	}

	if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		printf("Group not viable, are all devices attached to vfio?\n");
		return -1;
	}

	if (verbose) {
		printf("pre-SET_CONTAINER:\n");
		printf("VFIO_CHECK_EXTENSION VFIO_TYPE1_IOMMU: %sPresent\n",
		       ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) ?
		       "" : "Not ");
		printf("VFIO_CHECK_EXTENSION VFIO_NOIOMMU_IOMMU: %sPresent\n",
		       ioctl(container, VFIO_CHECK_EXTENSION, VFIO_NOIOMMU_IOMMU) ?
		       "" : "Not ");
	}

	ret = ioctl(group, VFIO_GROUP_SET_CONTAINER, &container);
	if (ret) {
		printf("Failed to set group container: %d (%s)\n", ret, strerror(errno));
		return ret;
	}

	if (verbose) {
		printf("post-SET_CONTAINER:\n");
		printf("VFIO_CHECK_EXTENSION VFIO_TYPE1_IOMMU: %sPresent\n",
		       ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) ?
		       "" : "Not ");
		printf("VFIO_CHECK_EXTENSION VFIO_NOIOMMU_IOMMU: %sPresent\n",
		       ioctl(container, VFIO_CHECK_EXTENSION, VFIO_NOIOMMU_IOMMU) ?
		       "" : "Not ");
	}

	ret = ioctl(container, VFIO_SET_IOMMU, noiommu ?
		    VFIO_TYPE1_IOMMU : VFIO_NOIOMMU_IOMMU);
	if (!ret) {
		printf("Incorrectly allowed %s-iommu usage!\n", noiommu ?
		       "type1" : "no");
		return -1;
	}

	ret = ioctl(container, VFIO_SET_IOMMU, iommu_type);
	if (ret) {
		printf("Failed to set IOMMU: %d (%s)\n", ret, strerror(errno));

		return ret;
	}

	return 0;
}

static int vfio_container_open(void)
{
	int fd;

	fd = open("/dev/vfio/vfio", O_RDWR);
	if (fd < 0) {
		printf("Failed to open /dev/vfio/vfio : %d (%s)\n",
		       fd, strerror(errno));
	}
	return fd;
}

static int __vfio_group_attach(int groupid, int *container_out, int *group_out,
			       int iommu_type)
{
	int container, group;
	bool noiommu = VFIO_NOIOMMU_IOMMU == iommu_type;

	group = vfio_group_open(groupid, noiommu);
	if (group < 0)
		return -1;

	container = vfio_container_open();
	if (container < 0)
		return -1;

	if (vfio_group_set_container(group, container, iommu_type))
		return -1;

	if (container_out)
		*container_out = container;
	if (group_out)
		*group_out = group;
	return 0;
}

int vfio_group_attach(int groupid, int *container_out, int *group_out)
{
	return __vfio_group_attach(groupid, container_out, group_out,
				   VFIO_TYPE1_IOMMU);
}

static int __vfio_device_attach(const char *devname, int *container_out,
				int *device_out, int *group_out,
				int iommu_type)
{
	int container, group, groupid, device;

	groupid = vfio_device_get_groupid(devname);
	if (groupid < 0)
		return -1;

	if (__vfio_group_attach(groupid, &container, &group, iommu_type) < 0)
		return -1;

	if (device_out) {
		device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, devname);
		if (device < 0) {
			printf("Failed to get device %s: %d (%s)\n",
			       devname, container, strerror(errno));
			return -1;
		}
		*device_out = device;
	}

	if (container_out)
		*container_out = container;
	if (group_out)
		*group_out = group;
	return 0;
}

int vfio_device_attach(const char *devname, int *container_out, int *device_out,
		       int *group_out)
{
	return __vfio_device_attach(devname, container_out, device_out,
				    group_out, VFIO_TYPE1_IOMMU);
}

int vfio_device_attach_iommu_type(const char *devname, int *container_out,
				  int *device_out, int *group_out,
				  int iommu_type)
{
	return __vfio_device_attach(devname, container_out, device_out,
				    group_out, iommu_type);
}

int pci_cfg_read8(int device, uint64_t cfg_offset, int offset, uint8_t *val)
{
	if (pread(device, val, 1, cfg_offset + offset) != 1) {
		printf("config read8 at 0x%x failed: %s\n",
		       offset, strerror(errno));
		return -1;
	}
	return 0;
}

int pci_cfg_read16(int device, uint64_t cfg_offset, int offset, uint16_t *val)
{
	if (pread(device, val, 2, cfg_offset + offset) != 2) {
		printf("config read16 at 0x%x failed: %s\n",
		       offset, strerror(errno));
		return -1;
	}
	return 0;
}

int pci_cfg_read32(int device, uint64_t cfg_offset, int offset, uint32_t *val)
{
	if (pread(device, val, 4, cfg_offset + offset) != 4) {
		printf("config read32 at 0x%x failed: %s\n",
		       offset, strerror(errno));
		return -1;
	}
	return 0;
}

int pci_cfg_write16(int device, uint64_t cfg_offset, int offset, uint16_t val)
{
	if (pwrite(device, &val, 2, cfg_offset + offset) != 2) {
		printf("config write16 at 0x%x failed: %s\n",
		       offset, strerror(errno));
		return -1;
	}
	return 0;
}

int pci_find_cap(int device, uint64_t cfg_offset, uint8_t cap_id)
{
	uint8_t pos, id;

	if (pci_cfg_read8(device, cfg_offset, PCI_CAPABILITY_LIST, &pos))
		return 0;

	while (pos) {
		if (pci_cfg_read8(device, cfg_offset, pos + PCI_CAP_LIST_ID, &id))
			return 0;
		if (id == cap_id)
			return pos;
		if (pci_cfg_read8(device, cfg_offset, pos + PCI_CAP_LIST_NEXT, &pos))
			return 0;
	}
	return 0;
}

/*
 * VFIO PCI device abstraction (IOMMUFD cdev path)
 */

int vfio_dev_open(struct vfio_dev *dev, const char *bdf)
{
	dev->name = bdf;

	dev->iommufd = open("/dev/iommu", O_RDWR);
	if (dev->iommufd < 0) {
		fprintf(stderr, "%s: open /dev/iommu: %s\n",
			__func__, strerror(errno));
		return -1;
	}

	if (vfio_device_iommufd_attach(dev->iommufd, bdf,
				       &dev->device_fd, &dev->ioas_id))
		return -1;

	return 0;
}

void vfio_dev_close(struct vfio_dev *dev)
{
	int i;

	vfio_dev_msix_disable(dev);

	vfio_dev_dma_free(dev);

	for (i = 0; i < VFIO_PCI_NUM_BARS; i++)
		if (dev->bar[i].addr && dev->bar[i].addr != MAP_FAILED)
			munmap(dev->bar[i].addr, dev->bar[i].size);

	if (dev->device_fd >= 0)
		close(dev->device_fd);

	if (dev->iommufd >= 0)
		close(dev->iommufd);
}

int vfio_dev_map_bar(struct vfio_dev *dev, int index)
{
	struct vfio_region_info reg = {
		.argsz = sizeof(reg),
		.index = index,
	};

	if (index < 0 || index >= VFIO_PCI_NUM_BARS) {
		fprintf(stderr, "%s: BAR index %d out of range\n",
			__func__, index);
		return -1;
	}

	if (ioctl(dev->device_fd, VFIO_DEVICE_GET_REGION_INFO, &reg) < 0) {
		fprintf(stderr, "%s: VFIO_DEVICE_GET_REGION_INFO(BAR%d): %s\n",
			__func__, index, strerror(errno));
		return -1;
	}

	printf("%s: BAR%d info: offset=0x%" PRIx64
	       " size=0x%" PRIx64 " flags=0x%x\n",
	       dev->name, index, (uint64_t)reg.offset,
	       (uint64_t)reg.size, reg.flags);

	dev->bar[index].size = reg.size;

	dev->bar[index].addr = mmap(NULL, reg.size,
				    PROT_READ | PROT_WRITE,
				    MAP_SHARED, dev->device_fd,
				    reg.offset);
	if (dev->bar[index].addr == MAP_FAILED) {
		fprintf(stderr, "%s: mmap BAR%d: %s\n",
			__func__, index, strerror(errno));
		return -1;
	}

	printf("%s: BAR%d mapped at %p\n",
	       dev->name, index, dev->bar[index].addr);
	return 0;
}

int vfio_dev_dma_alloc(struct vfio_dev *dev, size_t size, uint64_t iova)
{
	struct iommu_ioas_map map = {
		.size = sizeof(map),
		.ioas_id = dev->ioas_id,
		.flags = IOMMU_IOAS_MAP_READABLE | IOMMU_IOAS_MAP_WRITEABLE |
			 IOMMU_IOAS_MAP_FIXED_IOVA,
		.iova = iova,
	};

	size = ALIGN_UP(size, sysconf(_SC_PAGESIZE));

	dev->dma_va = mmap(NULL, size, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (dev->dma_va == MAP_FAILED) {
		fprintf(stderr, "%s: mmap: %s\n", __func__, strerror(errno));
		return -1;
	}

	map.user_va = (uintptr_t)dev->dma_va;
	map.length = size;

	if (ioctl(dev->iommufd, IOMMU_IOAS_MAP, &map)) {
		fprintf(stderr, "%s: IOMMU_IOAS_MAP: %s\n",
			__func__, strerror(errno));
		munmap(dev->dma_va, size);
		dev->dma_va = NULL;
		return -1;
	}

	dev->dma_size = size;
	dev->dma_iova = iova;
	return 0;
}

void vfio_dev_dma_free(struct vfio_dev *dev)
{
	struct iommu_ioas_unmap unmap = {
		.size = sizeof(unmap),
		.ioas_id = dev->ioas_id,
		.iova = dev->dma_iova,
		.length = dev->dma_size,
	};

	if (!dev->dma_va)
		return;

	if (ioctl(dev->iommufd, IOMMU_IOAS_UNMAP, &unmap))
		fprintf(stderr, "%s: IOMMU_IOAS_UNMAP: %s\n",
			__func__, strerror(errno));

	munmap(dev->dma_va, dev->dma_size);
	dev->dma_va = NULL;
	dev->dma_size = 0;
	dev->dma_iova = 0;
}

uint64_t vfio_dev_to_iova(struct vfio_dev *dev, void *va)
{
	size_t offset = (uint8_t *)va - (uint8_t *)dev->dma_va;

	if (offset >= dev->dma_size) {
		fprintf(stderr, "%s: VA %p out of DMA range [%p, %p)\n",
			__func__, va, dev->dma_va,
			(uint8_t *)dev->dma_va + dev->dma_size);
		abort();
	}

	return dev->dma_iova + offset;
}

uint32_t vfio_dev_reg_read(struct vfio_dev *dev, uint32_t off)
{
	if (off + sizeof(uint32_t) > dev->bar[0].size) {
		fprintf(stderr, "%s: BAR0 register out of range: 0x%x\n",
			__func__, off);
		exit(EXIT_FAILURE);
	}
	return *(volatile uint32_t *)((uint8_t *)dev->bar[0].addr + off);
}

void vfio_dev_reg_write(struct vfio_dev *dev, uint32_t off, uint32_t val)
{
	if (off + sizeof(uint32_t) > dev->bar[0].size) {
		fprintf(stderr, "%s: BAR0 register out of range: 0x%x\n",
			__func__, off);
		exit(EXIT_FAILURE);
	}
	*(volatile uint32_t *)((uint8_t *)dev->bar[0].addr + off) = val;
}

int vfio_dev_dump_iova_ranges(struct vfio_dev *dev)
{
	struct iommu_ioas_iova_ranges ranges = {
		.size = sizeof(ranges),
		.ioas_id = dev->ioas_id,
	};
	struct iommu_iova_range *iovars;
	unsigned int i;

	if (ioctl(dev->iommufd, IOMMU_IOAS_IOVA_RANGES, &ranges) < 0 &&
	    errno != EMSGSIZE) {
		fprintf(stderr, "%s: IOMMU_IOAS_IOVA_RANGES: %s\n",
			dev->name, strerror(errno));
		return -1;
	}

	iovars = calloc(ranges.num_iovas, sizeof(*iovars));
	if (!iovars)
		return -1;

	ranges.allowed_iovas = (uintptr_t)iovars;

	if (ioctl(dev->iommufd, IOMMU_IOAS_IOVA_RANGES, &ranges) < 0) {
		fprintf(stderr, "%s: IOMMU_IOAS_IOVA_RANGES: %s\n",
			dev->name, strerror(errno));
		free(iovars);
		return -1;
	}

	printf("%s: allowed IOVA ranges (ioas %d, alignment 0x%" PRIx64 "):\n",
	       dev->name, dev->ioas_id, (uint64_t)ranges.out_iova_alignment);

	for (i = 0; i < ranges.num_iovas; i++)
		printf("  [0x%" PRIx64 " - 0x%" PRIx64 "]\n",
		       (uint64_t)iovars[i].start, (uint64_t)iovars[i].last);

	free(iovars);
	return 0;
}

/*
 * VFIO PCI IRQ helpers
 */

int vfio_pci_irq_set_eventfd(int device, int index, int start, int count,
			     int *fds)
{
	struct {
		struct vfio_irq_set set;
		int32_t data[VFIO_PCI_IRQ_MAX_VECTORS];
	} irq = {
		.set.argsz = sizeof(struct vfio_irq_set) +
			     count * sizeof(int32_t),
		.set.flags = VFIO_IRQ_SET_DATA_EVENTFD |
			     VFIO_IRQ_SET_ACTION_TRIGGER,
		.set.index = index,
		.set.start = start,
		.set.count = count,
	};
	int i;

	if (count > VFIO_PCI_IRQ_MAX_VECTORS)
		return -1;

	for (i = 0; i < count; i++)
		irq.data[i] = fds[i];

	return ioctl(device, VFIO_DEVICE_SET_IRQS, &irq);
}

int vfio_pci_irq_trigger(int device, int index, int start, int count)
{
	struct vfio_irq_set irq = {
		.argsz = sizeof(irq),
		.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER,
		.index = index,
		.start = start,
		.count = count,
	};

	return ioctl(device, VFIO_DEVICE_SET_IRQS, &irq);
}

int vfio_pci_irq_unmask(int device, int index, int start, int count)
{
	struct vfio_irq_set irq = {
		.argsz = sizeof(irq),
		.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_UNMASK,
		.index = index,
		.start = start,
		.count = count,
	};

	return ioctl(device, VFIO_DEVICE_SET_IRQS, &irq);
}

int vfio_pci_irq_disable(int device, int index)
{
	struct vfio_irq_set irq = {
		.argsz = sizeof(irq),
		.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER,
		.index = index,
		.start = 0,
		.count = 0,
	};

	return ioctl(device, VFIO_DEVICE_SET_IRQS, &irq);
}

int eventfd_check(int fd, int timeout_ms)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	uint64_t val;
	int ret;

	ret = poll(&pfd, 1, timeout_ms);
	if (ret <= 0)
		return -1;

	if (read(fd, &val, sizeof(val)) != sizeof(val))
		return -1;

	return 0;
}

int vfio_dev_probe_dmabuf(struct vfio_dev *dev)
{
	struct vfio_device_feature probe = {
		.argsz = sizeof(probe),
		.flags = VFIO_DEVICE_FEATURE_PROBE | VFIO_DEVICE_FEATURE_DMA_BUF,
	};

	if (ioctl(dev->device_fd, VFIO_DEVICE_FEATURE, &probe) < 0) {
		fprintf(stderr, "%s: DMA-BUF not supported (%s)\n",
			dev->name, strerror(errno));
		return -1;
	}

	printf("%s: DMA-BUF feature supported\n", dev->name);
	return 0;
}

int vfio_dev_export_bar_dmabuf(struct vfio_dev *dev, int bar_index,
			      uint64_t length)
{
	struct {
		struct vfio_device_feature hdr;
		struct vfio_device_feature_dma_buf dma_buf;
		struct vfio_region_dma_range range;
	} req = {
		.hdr = {
			.argsz = sizeof(req),
			.flags = VFIO_DEVICE_FEATURE_GET |
				 VFIO_DEVICE_FEATURE_DMA_BUF,
		},
		.dma_buf = {
			.region_index = bar_index,
			.open_flags = O_RDWR,
			.nr_ranges = 1,
		},
		.range = {
			.length = length,
		},
	};
	int fd;

	fd = ioctl(dev->device_fd, VFIO_DEVICE_FEATURE, &req);
	if (fd < 0) {
		fprintf(stderr, "%s: BAR%d dmabuf export failed (%s)\n",
			dev->name, bar_index, strerror(errno));
		return -1;
	}

	printf("%s: BAR%d exported as dmabuf fd %d (size 0x%" PRIx64 ")\n",
	       dev->name, bar_index, fd, length);
	return fd;
}

int vfio_dev_map_dmabuf(struct vfio_dev *dev, int dmabuf_fd,
			uint64_t length, uint64_t *iova_out)
{
	struct iommu_ioas_map_file map_file = {
		.size = sizeof(map_file),
		.flags = IOMMU_IOAS_MAP_READABLE | IOMMU_IOAS_MAP_WRITEABLE,
		.ioas_id = dev->ioas_id,
		.fd = dmabuf_fd,
		.length = length,
	};

	if (ioctl(dev->iommufd, IOMMU_IOAS_MAP_FILE, &map_file) < 0) {
		fprintf(stderr, "%s: IOMMU_IOAS_MAP_FILE: %s\n",
			__func__, strerror(errno));
		return -1;
	}

	if (iova_out)
		*iova_out = map_file.iova;
	printf("%s: dmabuf fd %d mapped at IOVA 0x%" PRIx64 " (size 0x%" PRIx64 ")\n",
	       dev->name, dmabuf_fd, (uint64_t)map_file.iova, length);
	return 0;
}

int vfio_dev_msix_enable(struct vfio_dev *dev, unsigned int nr_vectors)
{
	int efd;

	efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (efd < 0) {
		fprintf(stderr, "%s: eventfd: %s\n", __func__, strerror(errno));
		return -1;
	}

	if (vfio_pci_irq_set_eventfd(dev->device_fd, VFIO_PCI_MSIX_IRQ_INDEX,
				     0, nr_vectors, &efd)) {
		fprintf(stderr, "%s: MSI-X enable failed: %s\n",
			dev->name, strerror(errno));
		close(efd);
		return -1;
	}

	dev->msix_fd = efd;
	printf("%s: MSI-X enabled (%u vector%s, eventfd %d)\n",
	       dev->name, nr_vectors, nr_vectors > 1 ? "s" : "", efd);
	return 0;
}

void vfio_dev_msix_disable(struct vfio_dev *dev)
{
	if (dev->msix_fd < 0)
		return;

	vfio_pci_irq_disable(dev->device_fd, VFIO_PCI_MSIX_IRQ_INDEX);
	close(dev->msix_fd);
	dev->msix_fd = -1;
}

void *mmap_align(void *addr, size_t length, int prot, int flags,
		 int fd, off_t offset, size_t align)
{
	void *addr_align;
	void *addr_base;
	void *map;

	addr_base = mmap(NULL, length + align, PROT_NONE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (addr_base == MAP_FAILED) {
		return addr_base;
	}

	addr_align = (void *)ALIGN_UP((uintptr_t)addr_base, (uintptr_t)align);
	munmap(addr_base, addr_align - addr_base);
	munmap(addr_align + length, align - (addr_align - addr_base));

	map = mmap(addr_align, length, prot, flags | MAP_FIXED, fd, offset);
	if (map != MAP_FAILED)
		madvise(map, length, MADV_HUGEPAGE);
	return map;
}
