/*
 * VFIO test suite - PCI IRQ setup (INTx, MSI, MSI-X, ERR, REQ)
 *
 * Copyright (C) 2012-2025, Red Hat Inc.
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>

#include <linux/vfio.h>

#include "utils.h"

static const char *irq_index_name[] = {
	[VFIO_PCI_INTX_IRQ_INDEX]	= "INTx",
	[VFIO_PCI_MSI_IRQ_INDEX]	= "MSI",
	[VFIO_PCI_MSIX_IRQ_INDEX]	= "MSI-X",
	[VFIO_PCI_ERR_IRQ_INDEX]	= "ERR",
	[VFIO_PCI_REQ_IRQ_INDEX]	= "REQ",
};

void usage(char *name)
{
	printf("usage: %s [options] <ssss:bb:dd.f>\n", name);
	printf("\t-c cycles  number of enable/disable cycles (default 1)\n");
	printf("\nPCI IRQ setup test via VFIO (IOMMUFD cdev)\n");
}

static int test_irq_index(int device, const char *name, int index,
			  int count, int flags, int cycle, int max_cycles)
{
	int automasked = flags & VFIO_IRQ_INFO_AUTOMASKED;
	int fds[VFIO_PCI_IRQ_MAX_VECTORS];
	int nr, i, ret, triggered = 0;

	nr = count > VFIO_PCI_IRQ_MAX_VECTORS ? VFIO_PCI_IRQ_MAX_VECTORS : count;

	for (i = 0; i < nr; i++) {
		fds[i] = eventfd(0, EFD_CLOEXEC);
		if (fds[i] < 0) {
			printf("%s: eventfd failed: %s\n", name,
			       strerror(errno));
			while (--i >= 0)
				close(fds[i]);
			return -1;
		}
	}

	ret = vfio_pci_irq_set_eventfd(device, index, 0, nr, fds);
	if (ret) {
		printf("%s: SET_IRQS enable failed: %s\n", name,
		       strerror(errno));
		goto out;
	}

	if (verbose)
		printf("  %s [%d/%d]: enabled %d vector%s", name,
		       cycle + 1, max_cycles, nr, nr > 1 ? "s" : "");

	/* Loopback trigger vector 0 */
	ret = vfio_pci_irq_trigger(device, index, 0, 1);
	if (ret) {
		if (verbose)
			printf(", trigger failed: %s", strerror(errno));
	} else {
		ret = eventfd_check(fds[0], 100);
		if (ret) {
			if (verbose)
				printf(", trigger: no event");
		} else {
			triggered = 1;
			if (verbose)
				printf(", triggered");
		}

		if (automasked) {
			ret = vfio_pci_irq_unmask(device, index, 0, 1);
			if (ret && verbose)
				printf(", unmask failed: %s", strerror(errno));
			else if (verbose)
				printf(", unmasked");
		}
	}

	ret = vfio_pci_irq_disable(device, index);
	if (ret) {
		printf("%s: SET_IRQS disable failed: %s\n", name,
		       strerror(errno));
		goto out;
	}

	if (verbose)
		printf(", disabled\n");
	else if (!triggered && cycle == 0)
		printf("(%s: trigger not delivered) ", name);

	ret = 0;
out:
	for (i = 0; i < nr; i++)
		close(fds[i]);
	return ret;
}

int main(int argc, char **argv)
{
	struct vfio_dev dev = VFIO_DEV_INIT;
	int opt, ret;
	int max_cycles = 1;
	int i, cycle;
	struct vfio_device_info device_info = { .argsz = sizeof(device_info) };
	struct vfio_irq_info irq_info[5];
	int supported = 0;

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

	if (vfio_dev_open(&dev, argv[optind]))
		return -1;

	ret = ioctl(dev.device_fd, VFIO_DEVICE_GET_INFO, &device_info);
	if (ret) {
		printf("VFIO_DEVICE_GET_INFO failed: %s\n", strerror(errno));
		return -1;
	}

	printf("Device %s: %d irq indexes\n", dev.name, device_info.num_irqs);

	for (i = 0; i < 5 && i < device_info.num_irqs; i++) {
		irq_info[i].argsz = sizeof(irq_info[i]);
		irq_info[i].index = i;
		ret = ioctl(dev.device_fd, VFIO_DEVICE_GET_IRQ_INFO, &irq_info[i]);
		if (ret) {
			printf("  %s: GET_IRQ_INFO failed: %s\n",
			       irq_index_name[i], strerror(errno));
			return -1;
		}

		printf("  %s: count=%d flags=0x%x", irq_index_name[i],
		       irq_info[i].count, irq_info[i].flags);
		if (irq_info[i].flags & VFIO_IRQ_INFO_EVENTFD)
			printf(" eventfd");
		if (irq_info[i].flags & VFIO_IRQ_INFO_MASKABLE)
			printf(" maskable");
		if (irq_info[i].flags & VFIO_IRQ_INFO_AUTOMASKED)
			printf(" automasked");
		if (irq_info[i].flags & VFIO_IRQ_INFO_NORESIZE)
			printf(" noresize");
		printf("\n");

		if (irq_info[i].count > 0 &&
		    (irq_info[i].flags & VFIO_IRQ_INFO_EVENTFD))
			supported++;
	}

	if (!supported) {
		printf("Skipping: no IRQ indexes support eventfd\n");
		return EXIT_SKIP;
	}

	for (cycle = 0; cycle < max_cycles; cycle++) {
		if (!verbose) {
			printf(".");
			fflush(stdout);
		}

		for (i = 0; i < 5 && i < device_info.num_irqs; i++) {
			if (irq_info[i].count == 0)
				continue;
			if (!(irq_info[i].flags & VFIO_IRQ_INFO_EVENTFD))
				continue;

			ret = test_irq_index(dev.device_fd, irq_index_name[i], i,
					     irq_info[i].count,
					     irq_info[i].flags,
					     cycle, max_cycles);
			if (ret)
				return 1;
		}
	}

	if (!verbose)
		printf("\n");

	printf("%d cycle%s, %d IRQ index%s tested, Success\n",
	       max_cycles, max_cycles > 1 ? "s" : "",
	       supported, supported > 1 ? "es" : "");
	return 0;
}
