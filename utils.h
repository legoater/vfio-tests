/*
 * VFIO test suite
 *
 * Copyright (C) 2012-2025, Red Hat Inc.
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#ifndef VFIO_TESTSUITE_UTILS_H
#define VFIO_TESTSUITE_UTILS_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/*
 * Logging
 */
extern int verbose;

int vfio_group_attach(int groupid, int *container_out, int *group_out);
int vfio_device_attach(const char *devname, int *container_out,
		       int *device_out, int *group_out);
int vfio_device_attach_iommu_type(const char *devname, int *container_out,
				  int *device_out, int *group_out,
				  int iommu_type);
int vfio_device_iommufd_getfd(const char *devname);
int vfio_device_iommufd_attach_with_token(
	int iommufd, const char *devname, int *device_out, int *ioas_id_out,
	const char *token);
static inline int vfio_device_iommufd_attach(int iommufd, const char *devname,
					     int *device_out, int *ioas_id_out)
{
	return vfio_device_iommufd_attach_with_token(iommufd, devname,
				     device_out, ioas_id_out, NULL);
}

#define VF_TOKEN_SIZE 16
int parse_vf_token(const char *token, unsigned char bytes[VF_TOKEN_SIZE]);

#define VF_TOKEN_BOGUS "00000000-0000-0000-0000-000000000001"

#define NSEC_PER_SEC 1000000000ul
#define USEC_PER_SEC 1000000ul

static inline unsigned long now_nsec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return NSEC_PER_SEC * (unsigned long) ts.tv_sec + ts.tv_nsec;
}

void hexdump(const void *data, size_t len);
int vfio_pci_is_vf(const char *devname);
int vfio_pci_is_vga(const char *devname);
int vfio_pci_is_d3(const char *devname);
long slab_sunreclaim_kb(void);
long vfio_dma_entry_limit(void);
int vfio_dma_entry_limit_check(unsigned long nr_mappings);
int vfio_noiommu_enabled(void);
int vfio_device_get_groupid(const char *devname);
int vfio_group_open(int groupid, bool noiommu);
long hugepages_free(void);
unsigned int vfio_pci_vendor(const char *devname);
const char *pci_sysfs_attr(const char *devname, const char *attr,
			   char *buf, size_t len);

void *mmap_align(void *addr, size_t length, int prot, int flags,
		 int fd, off_t offset, size_t align);

int pci_find_cap(int device, uint64_t cfg_offset, uint8_t cap_id);
int pci_cfg_read8(int device, uint64_t cfg_offset, int offset, uint8_t *val);
int pci_cfg_read16(int device, uint64_t cfg_offset, int offset, uint16_t *val);
int pci_cfg_read32(int device, uint64_t cfg_offset, int offset, uint32_t *val);
int pci_cfg_write16(int device, uint64_t cfg_offset, int offset, uint16_t val);

#define EXIT_SKIP 77

#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

#define NSEC_PER_MSEC (NSEC_PER_SEC / 1000)
#define USEC_PER_MSEC (USEC_PER_SEC / 1000)

/*
 * VFIO PCI device abstraction (IOMMUFD cdev path)
 */
struct vfio_dev {
	const char *name;
	int device_fd;
	int iommufd;
	int ioas_id;
};

#define VFIO_DEV_INIT { .device_fd = -1, .iommufd = -1 }

int vfio_dev_open(struct vfio_dev *dev, const char *bdf);
void vfio_dev_close(struct vfio_dev *dev);
int vfio_dev_dump_iova_ranges(struct vfio_dev *dev);

/*
 * VFIO DMA-BUF BAR export (IOMMUFD)
 */
#ifndef VFIO_DEVICE_FEATURE_DMA_BUF
#define VFIO_DEVICE_FEATURE_DMA_BUF 11

struct vfio_region_dma_range {
	uint64_t offset;
	uint64_t length;
};

struct vfio_device_feature_dma_buf {
	uint32_t region_index;
	uint32_t open_flags;
	uint32_t flags;
	uint32_t nr_ranges;
	struct vfio_region_dma_range dma_ranges[];
};
#endif

/*
 * VFIO PCI IRQ helpers (work with raw device fd)
 */
#define VFIO_PCI_IRQ_MAX_VECTORS 32

int vfio_pci_irq_set_eventfd(int device, int index, int start, int count,
			     int *fds);
int vfio_pci_irq_trigger(int device, int index, int start, int count);
int vfio_pci_irq_unmask(int device, int index, int start, int count);
int vfio_pci_irq_disable(int device, int index);
int eventfd_check(int fd, int timeout_ms);

int vfio_dev_probe_dmabuf(struct vfio_dev *dev);
int vfio_dev_export_bar_dmabuf(struct vfio_dev *dev, int bar_index,
			       uint64_t length);
int vfio_dev_map_dmabuf(struct vfio_dev *dev, int dmabuf_fd,
			uint64_t length, uint64_t *iova_out);

static inline const char *size_str(unsigned long size, char *buf, size_t len)
{
	if (size >= (1ul << 40) && !(size & ((1ul << 40) - 1)))
		snprintf(buf, len, "%ldTB", size >> 40);
	else if (size >= (1ul << 30) && !(size & ((1ul << 30) - 1)))
		snprintf(buf, len, "%ldGB", size >> 30);
	else if (size >= (1ul << 20) && !(size & ((1ul << 20) - 1)))
		snprintf(buf, len, "%ldMB", size >> 20);
	else
		snprintf(buf, len, "%ldKB", size >> 10);
	return buf;
}

#endif /* VFIO_TESTSUITE_UTILS_H */
