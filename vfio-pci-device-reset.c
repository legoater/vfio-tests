/*
 * VFIO test suite - PCI device reset (FLR)
 *
 * Copyright (C) 2012-2026, Red Hat Inc.
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/param.h>

#include <linux/pci_regs.h>
#include <linux/vfio.h>

#include "utils.h"

#define DEFAULT_CYCLES 5

#define PCI_STATUS_ERROR_BITS	(PCI_STATUS_DETECTED_PARITY | \
				 PCI_STATUS_SIG_SYSTEM_ERROR | \
				 PCI_STATUS_REC_MASTER_ABORT | \
				 PCI_STATUS_REC_TARGET_ABORT | \
				 PCI_STATUS_SIG_TARGET_ABORT)

static int test_ioctl_reset(int device, uint64_t cfg_offset,
			    uint16_t cmd_before, int max_cycles)
{
	uint16_t cmd_after = 0, cmd_modified, status;
	int i, ret;

	printf("Testing VFIO_DEVICE_RESET ioctl\n");

	for (i = 0; i < max_cycles; i++) {
		if (verbose)
			printf("reset %d/%d: ", i + 1, max_cycles);

		cmd_modified = cmd_before | PCI_COMMAND_MASTER | PCI_COMMAND_MEMORY;
		if (pci_cfg_write16(device, cfg_offset, PCI_COMMAND,
				    cmd_modified))
			return -1;

		ret = ioctl(device, VFIO_DEVICE_RESET);
		if (ret) {
			printf("VFIO_DEVICE_RESET failed: %s\n",
			       strerror(errno));
			return 1;
		}

		if (pci_cfg_read16(device, cfg_offset, PCI_COMMAND, &cmd_after))
			return -1;
		if (pci_cfg_read16(device, cfg_offset, PCI_STATUS, &status))
			return -1;

		if (verbose)
			printf("cmd 0x%04x -> 0x%04x -> 0x%04x, status=0x%04x\n",
			       cmd_before, cmd_modified, cmd_after, status);

		if (status & PCI_STATUS_ERROR_BITS)
			printf("WARNING: status error bits set after reset: 0x%04x\n",
			       status);

		if (!verbose) {
			printf(".");
			fflush(stdout);
		}
	}

	if (!verbose)
		printf("\n");

	if (cmd_after != cmd_before)
		printf("Note: command register not restored after ioctl reset (0x%04x -> 0x%04x)\n",
		       cmd_before, cmd_after);

	printf("%d ioctl resets, Success\n", max_cycles);
	return 0;
}

static int test_flr_reset(int device, uint64_t cfg_offset,
			  uint16_t cmd_before, int max_cycles)
{
	uint16_t cmd_after = 0, cmd_modified, status, devctl;
	uint32_t devcap;
	int pcie_cap, i;

	pcie_cap = pci_find_cap(device, cfg_offset, PCI_CAP_ID_EXP);
	if (!pcie_cap) {
		printf("FAIL: reset_method reports FLR but no PCIe capability found\n");
		return -1;
	}

	if (pci_cfg_read32(device, cfg_offset,
			   pcie_cap + PCI_EXP_DEVCAP, &devcap))
		return -1;

	if (!(devcap & PCI_EXP_DEVCAP_FLR)) {
		printf("FAIL: reset_method reports FLR but DevCap FLR bit not set\n");
		return -1;
	}

	printf("Testing FLR via DevCtl write\n");

	for (i = 0; i < max_cycles; i++) {
		if (verbose)
			printf("FLR %d/%d: ", i + 1, max_cycles);

		cmd_modified = cmd_before | PCI_COMMAND_MASTER | PCI_COMMAND_MEMORY;
		if (pci_cfg_write16(device, cfg_offset, PCI_COMMAND,
				    cmd_modified))
			return -1;

		if (pci_cfg_read16(device, cfg_offset,
				   pcie_cap + PCI_EXP_DEVCTL, &devctl))
			return -1;
		if (pci_cfg_write16(device, cfg_offset,
				    pcie_cap + PCI_EXP_DEVCTL,
				    devctl | PCI_EXP_DEVCTL_BCR_FLR))
			return -1;

		usleep(100000);

		if (pci_cfg_read16(device, cfg_offset, PCI_COMMAND, &cmd_after))
			return -1;
		if (pci_cfg_read16(device, cfg_offset, PCI_STATUS, &status))
			return -1;

		if (verbose)
			printf("cmd 0x%04x -> 0x%04x -> 0x%04x, status=0x%04x\n",
			       cmd_before, cmd_modified, cmd_after, status);

		if (status & PCI_STATUS_ERROR_BITS)
			printf("WARNING: status error bits set after FLR: 0x%04x\n",
			       status);

		if (!verbose) {
			printf(".");
			fflush(stdout);
		}
	}

	if (!verbose)
		printf("\n");

	if (cmd_after != cmd_before)
		printf("Note: command register not restored after FLR (0x%04x -> 0x%04x)\n",
		       cmd_before, cmd_after);

	printf("%d FLR resets, Success\n", max_cycles);
	return 0;
}

void usage(char *name)
{
	printf("usage: %s [options] <ssss:bb:dd.f>\n", name);
	printf("\t-c cycles  number of reset cycles (default %d)\n",
	       DEFAULT_CYCLES);
	printf("\nPCI device reset via VFIO (FLR reset)\n");
}

int main(int argc, char **argv)
{
	const char *devname;
	int opt, ret, container, device;
	int max_cycles = DEFAULT_CYCLES;
	char methods[128];
	struct vfio_device_info device_info = { .argsz = sizeof(device_info) };
	struct vfio_region_info config_info = { .argsz = sizeof(config_info) };
	uint16_t cmd_before;

	while ((opt = getopt(argc, argv, "c:h")) != -1) {
		switch (opt) {
		case 'c':
			max_cycles = atoi(optarg);
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

	devname = argv[optind];

	if (vfio_device_attach(devname, &container, &device, NULL))
		return -1;

	ret = ioctl(device, VFIO_DEVICE_GET_INFO, &device_info);
	if (ret) {
		printf("VFIO_DEVICE_GET_INFO failed: %s\n", strerror(errno));
		return -1;
	}

	if (!(device_info.flags & VFIO_DEVICE_FLAGS_RESET)) {
		printf("Skipping: device does not support reset\n");
		return EXIT_SKIP;
	}

	pci_sysfs_attr(devname, "reset_method", methods, sizeof(methods));

	if (!strcmp(methods, "unknown") || !strlen(methods)) {
		printf("FAIL: VFIO_DEVICE_FLAGS_RESET set but reset_method is empty\n");
		return -1;
	}

	printf("Device %s: reset supported (%s), %d regions, %d irqs\n",
	       devname, methods, device_info.num_regions, device_info.num_irqs);

	config_info.index = VFIO_PCI_CONFIG_REGION_INDEX;
	ret = ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &config_info);
	if (ret) {
		printf("Failed to get config region info: %s\n",
		       strerror(errno));
		return -1;
	}

	if (pci_cfg_read16(device, config_info.offset, PCI_COMMAND, &cmd_before))
		return -1;

	printf("PCI command register: 0x%04x\n", cmd_before);

	ret = test_ioctl_reset(device, config_info.offset, cmd_before, max_cycles);
	if (ret)
		return ret;

	if (strstr(methods, "flr")) {
		ret = test_flr_reset(device, config_info.offset, cmd_before,
				     max_cycles);
		if (ret)
			return ret;
	}

	return 0;
}
