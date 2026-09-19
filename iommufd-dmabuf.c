/*
 * VFIO test suite
 *
 * Copyright (C) 2012-2025, Red Hat Inc.
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/ioctl.h>
#include <linux/iommufd.h>
#include <linux/vfio.h>

#include "utils.h"

static int test_export_bars(struct vfio_dev *src, struct vfio_dev *dst,
			    int bar_conflict)
{
	struct vfio_region_info region_info = { .argsz = sizeof(region_info) };
	int i, ret;

	printf("\nExporting %s BARs as dma-buf, mapping into %s ioas %d\n\n",
	       src->name, dst->name, dst->ioas_id);

	for (i = 0; i < VFIO_PCI_ROM_REGION_INDEX; i++) {
		int dmabuf_fd;

		region_info.index = i;
		ret = ioctl(src->device_fd, VFIO_DEVICE_GET_REGION_INFO,
			    &region_info);
		if (ret)
			continue;

		if (!region_info.size ||
		    !(region_info.flags & VFIO_REGION_INFO_FLAG_MMAP))
			continue;

		printf("BAR%d: size 0x%lx, offset 0x%lx, flags 0x%x\n",
		       i, (unsigned long)region_info.size,
		       (unsigned long)region_info.offset, region_info.flags);

		dmabuf_fd = vfio_dev_export_bar_dmabuf(src, i,
						       region_info.size);
		if (dmabuf_fd < 0) {
			if (i == bar_conflict) {
				printf("\t[PASS] dma-buf rejected\n");
				continue;
			}
			continue;
		}

		if (i == bar_conflict) {
			printf("\t[FAIL] dma-buf should have been rejected\n");
			close(dmabuf_fd);
			return -1;
		}

		void *map = mmap(NULL, (size_t)region_info.size,
				 PROT_READ, MAP_SHARED, src->device_fd,
				 (off_t)region_info.offset);
		if (map == MAP_FAILED) {
			printf("\tmmap failed (%s)\n", strerror(errno));
		} else {
			if (verbose)
				hexdump(map, region_info.size > 64 ? 64 :
					region_info.size);
			munmap(map, (size_t)region_info.size);
		}

		ret = vfio_dev_map_dmabuf(dst, dmabuf_fd,
					  region_info.size, NULL);
		close(dmabuf_fd);
		if (ret < 0)
			continue;
	}

	return 0;
}

static int test_export_invalid_indices(struct vfio_dev *dev)
{
	int invalid_indices[] = {
		VFIO_PCI_ROM_REGION_INDEX,
		VFIO_PCI_ROM_REGION_INDEX + 1,
		0xff,
	};
	int i, ret;

	printf("\nTesting dma-buf rejection for invalid region indices\n\n");

	for (i = 0; i < (int)(sizeof(invalid_indices) / sizeof(invalid_indices[0])); i++) {
		printf("Region index %d (out of range)\n", invalid_indices[i]);

		ret = vfio_dev_export_bar_dmabuf(dev, invalid_indices[i], 4096);
		if (ret >= 0) {
			printf("\t[FAIL] dma-buf export should have been rejected\n");
			close(ret);
			return -1;
		}
		printf("\t[PASS] rejected\n");
	}

	return 0;
}

void usage(char *name)
{
	printf("usage: %s [options] <ssss:bb:dd.f>\n", name);
	printf("\t-b BAR  expect dma-buf export to fail for this BAR index\n");
	printf("\t-d BDF  destination device for P2P mapping\n");
	printf("\tWithout -d: export all mmappable BARs and self-map dma-buf\n");
	printf("\tWith -d:    export src BARs, map into dst IOAS (P2P)\n");
	printf("\nTest VFIO dma-buf BAR export and P2P mapping via iommufd\n");
}

int main(int argc, char **argv)
{
	const char *src_name, *dst_name = NULL;
	struct vfio_dev src = VFIO_DEV_INIT;
	struct vfio_dev dst = VFIO_DEV_INIT;
	int opt, ret;
	int bar_conflict = -1;
	struct vfio_device_info device_info = { .argsz = sizeof(device_info) };

	while ((opt = getopt(argc, argv, "b:d:h")) != -1) {
		switch (opt) {
		case 'b':
			bar_conflict = atoi(optarg);
			break;
		case 'd':
			dst_name = optarg;
			break;
		case 'h':
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : -1;
		}
	}

	if (optind >= argc) {
		usage(argv[0]);
		return -1;
	}

	src_name = argv[optind];

	if (vfio_dev_open(&src, src_name))
		return 1;

	if (vfio_dev_probe_dmabuf(&src))
		return EXIT_SKIP;

	if (dst_name) {
		dst.name = dst_name;
		dst.iommufd = src.iommufd;
		if (vfio_device_iommufd_attach(src.iommufd, dst_name,
					       &dst.device_fd, &dst.ioas_id))
			return 1;
	} else {
		dst = src;
	}

	ret = ioctl(src.device_fd, VFIO_DEVICE_GET_INFO, &device_info);
	if (ret) {
		printf("Failed to get device info\n");
		return -1;
	}

	ret = test_export_bars(&src, &dst, bar_conflict);
	if (ret)
		return ret;

	ret = test_export_invalid_indices(&src);
	if (ret)
		return ret;

	if (dst_name)
		close(dst.device_fd);
	vfio_dev_close(&src);

	printf("Success\n");
	return 0;
}
