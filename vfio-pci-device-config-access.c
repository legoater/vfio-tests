/*
 * VFIO test suite - PCI config space access
 *
 * Copyright (C) 2012-2025, Red Hat Inc.
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

#include <linux/pci_regs.h>
#include <linux/vfio.h>

#include "utils.h"

#define DEFAULT_CYCLES 1

static const char *cap_names[] = {
	[PCI_CAP_ID_PM]		= "PM",
	[PCI_CAP_ID_VPD]	= "VPD",
	[PCI_CAP_ID_MSI]	= "MSI",
	[PCI_CAP_ID_PCIX]	= "PCI-X",
	[PCI_CAP_ID_VNDR]	= "Vendor",
	[PCI_CAP_ID_EXP]	= "PCIe",
	[PCI_CAP_ID_MSIX]	= "MSI-X",
};

static const char *ext_cap_names[] = {
	[PCI_EXT_CAP_ID_ERR]		= "AER",
	[PCI_EXT_CAP_ID_DSN]		= "DSN",
	[PCI_EXT_CAP_ID_VNDR]		= "Vendor",
	[PCI_EXT_CAP_ID_ACS]		= "ACS",
	[PCI_EXT_CAP_ID_ARI]		= "ARI",
	[PCI_EXT_CAP_ID_ATS]		= "ATS",
	[PCI_EXT_CAP_ID_SRIOV]		= "SR-IOV",
	[PCI_EXT_CAP_ID_PRI]		= "PRI",
	[PCI_EXT_CAP_ID_REBAR]		= "Resizable BAR",
	[PCI_EXT_CAP_ID_TPH]		= "TPH",
	[PCI_EXT_CAP_ID_LTR]		= "LTR",
	[PCI_EXT_CAP_ID_PASID]		= "PASID",
	[PCI_EXT_CAP_ID_DPC]		= "DPC",
	[PCI_EXT_CAP_ID_L1SS]		= "L1 PM Substates",
	[PCI_EXT_CAP_ID_PTM]		= "PTM",
	[PCI_EXT_CAP_ID_SECPCI]		= "Secondary PCIe",
	[PCI_EXT_CAP_ID_DVSEC]		= "DVSEC",
	[PCI_EXT_CAP_ID_VF_REBAR]	= "VF Resizable BAR",
	[PCI_EXT_CAP_ID_DLF]		= "Data Link Feature",
	[PCI_EXT_CAP_ID_PL_16GT]	= "PL 16GT",
	[PCI_EXT_CAP_ID_PL_32GT]	= "PL 32GT",
#ifdef	PCI_EXT_CAP_ID_PL_64GT
	[PCI_EXT_CAP_ID_PL_64GT]	= "PL 64GT",
#endif
	[PCI_EXT_CAP_ID_DOE]		= "DOE",
};

#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))

void usage(char *name)
{
	printf("usage: %s [options] <ssss:bb:dd.f>\n", name);
	printf("\t-c cycles  number of write/read-back cycles (default %d)\n",
	       DEFAULT_CYCLES);
	printf("\nPCI config space access test via VFIO\n");
}

static int read_sysfs_hex(const char *devname, const char *attr,
			  unsigned int *val)
{
	char path[256];
	FILE *f;

	snprintf(path, sizeof(path),
		 "/sys/bus/pci/devices/%s/%s", devname, attr);
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (fscanf(f, "%x", val) != 1) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

static int verify_header(int device, uint64_t cfg_offset, const char *devname)
{
	uint16_t vendor, device_id, subsys_vendor, subsys_id;
	uint32_t class;
	unsigned int sysfs_val;
	int errors = 0;

	if (pci_cfg_read16(device, cfg_offset, PCI_VENDOR_ID, &vendor) ||
	    pci_cfg_read16(device, cfg_offset, PCI_DEVICE_ID, &device_id) ||
	    pci_cfg_read32(device, cfg_offset, PCI_CLASS_REVISION, &class) ||
	    pci_cfg_read16(device, cfg_offset, PCI_SUBSYSTEM_VENDOR_ID, &subsys_vendor) ||
	    pci_cfg_read16(device, cfg_offset, PCI_SUBSYSTEM_ID, &subsys_id))
		return -1;

	class >>= 8;

	printf("  vendor=0x%04x device=0x%04x class=0x%06x "
	       "subsys=0x%04x:0x%04x\n",
	       vendor, device_id, class, subsys_vendor, subsys_id);

	if (!read_sysfs_hex(devname, "vendor", &sysfs_val)) {
		if (vendor != sysfs_val) {
			printf("  FAIL: vendor mismatch: vfio=0x%04x sysfs=0x%04x\n",
			       vendor, sysfs_val);
			errors++;
		}
	}

	if (!read_sysfs_hex(devname, "device", &sysfs_val)) {
		if (device_id != sysfs_val) {
			printf("  FAIL: device mismatch: vfio=0x%04x sysfs=0x%04x\n",
			       device_id, sysfs_val);
			errors++;
		}
	}

	if (!read_sysfs_hex(devname, "class", &sysfs_val)) {
		if (class != sysfs_val) {
			printf("  FAIL: class mismatch: vfio=0x%06x sysfs=0x%06x\n",
			       class, sysfs_val);
			errors++;
		}
	}

	if (!read_sysfs_hex(devname, "subsystem_vendor", &sysfs_val)) {
		if (subsys_vendor != sysfs_val) {
			printf("  FAIL: subsystem_vendor mismatch: vfio=0x%04x sysfs=0x%04x\n",
			       subsys_vendor, sysfs_val);
			errors++;
		}
	}

	if (!read_sysfs_hex(devname, "subsystem_device", &sysfs_val)) {
		if (subsys_id != sysfs_val) {
			printf("  FAIL: subsystem_device mismatch: vfio=0x%04x sysfs=0x%04x\n",
			       subsys_id, sysfs_val);
			errors++;
		}
	}

	if (!errors && verbose)
		printf("  header matches sysfs\n");

	return errors;
}

static int walk_capabilities(int device, uint64_t cfg_offset)
{
	uint8_t cap_ptr, cap_id, cap_next;
	uint16_t status;
	int count = 0;

	if (pci_cfg_read16(device, cfg_offset, PCI_STATUS, &status))
		return -1;

	if (!(status & PCI_STATUS_CAP_LIST)) {
		printf("  no capabilities\n");
		return 0;
	}

	if (pci_cfg_read8(device, cfg_offset, PCI_CAPABILITY_LIST, &cap_ptr))
		return -1;

	cap_ptr &= ~3;

	printf("  capabilities:");

	while (cap_ptr >= 0x40) {
		if (pci_cfg_read8(device, cfg_offset, cap_ptr, &cap_id) ||
		    pci_cfg_read8(device, cfg_offset, cap_ptr + 1, &cap_next))
			return -1;

		if (cap_id < ARRAY_SIZE(cap_names) && cap_names[cap_id])
			printf(" %s", cap_names[cap_id]);
		else
			printf(" 0x%02x", cap_id);

		if (verbose)
			printf("@0x%02x", cap_ptr);

		count++;
		cap_ptr = cap_next & ~3;

		if (count > 48)
			break;
	}

	printf(" (%d)\n", count);
	return 0;
}

static void decode_aer(int device, uint64_t cfg_offset, int cap_offset)
{
	uint32_t uncor_status, uncor_mask, cor_status;

	if (pci_cfg_read32(device, cfg_offset, cap_offset + PCI_ERR_UNCOR_STATUS,
		       &uncor_status) ||
	    pci_cfg_read32(device, cfg_offset, cap_offset + PCI_ERR_UNCOR_MASK,
		       &uncor_mask) ||
	    pci_cfg_read32(device, cfg_offset, cap_offset + PCI_ERR_COR_STATUS,
		       &cor_status))
		return;

	printf("    AER: UncorSta=0x%08x UncorMask=0x%08x CorSta=0x%08x\n",
	       uncor_status, uncor_mask, cor_status);
}

static void decode_ari(int device, uint64_t cfg_offset, int cap_offset)
{
	uint16_t cap, ctrl;

	if (pci_cfg_read16(device, cfg_offset, cap_offset + PCI_ARI_CAP, &cap) ||
	    pci_cfg_read16(device, cfg_offset, cap_offset + PCI_ARI_CTRL, &ctrl))
		return;

	printf("    ARI: NextFn=%d", PCI_ARI_CAP_NFN(cap));
	if (cap & PCI_ARI_CAP_MFVC)
		printf(" MFVC");
	if (cap & PCI_ARI_CAP_ACS)
		printf(" ACS");
	printf(" (ctrl=0x%04x)\n", ctrl);
}

static void decode_sriov(int device, uint64_t cfg_offset, int cap_offset)
{
	uint16_t ctrl, total_vf, num_vf, initial_vf, vf_offset, vf_stride, vf_did;

	if (pci_cfg_read16(device, cfg_offset, cap_offset + PCI_SRIOV_CTRL, &ctrl) ||
	    pci_cfg_read16(device, cfg_offset, cap_offset + PCI_SRIOV_TOTAL_VF,
		       &total_vf) ||
	    pci_cfg_read16(device, cfg_offset, cap_offset + PCI_SRIOV_NUM_VF,
		       &num_vf) ||
	    pci_cfg_read16(device, cfg_offset, cap_offset + PCI_SRIOV_INITIAL_VF,
		       &initial_vf) ||
	    pci_cfg_read16(device, cfg_offset, cap_offset + PCI_SRIOV_VF_OFFSET,
		       &vf_offset) ||
	    pci_cfg_read16(device, cfg_offset, cap_offset + PCI_SRIOV_VF_STRIDE,
		       &vf_stride) ||
	    pci_cfg_read16(device, cfg_offset, cap_offset + PCI_SRIOV_VF_DID,
		       &vf_did))
		return;

	printf("    SR-IOV: InitialVFs=%d TotalVFs=%d NumVFs=%d",
	       initial_vf, total_vf, num_vf);
	printf(" VFOffset=%d VFStride=%d VFDevID=0x%04x",
	       vf_offset, vf_stride, vf_did);
	if (ctrl & PCI_SRIOV_CTRL_VFE)
		printf(" VFEnable");
	if (ctrl & PCI_SRIOV_CTRL_ARI)
		printf(" ARICap");
	printf("\n");
}

static void decode_acs(int device, uint64_t cfg_offset, int cap_offset)
{
	uint16_t cap, ctrl;

	if (pci_cfg_read16(device, cfg_offset, cap_offset + PCI_ACS_CAP, &cap) ||
	    pci_cfg_read16(device, cfg_offset, cap_offset + PCI_ACS_CTRL, &ctrl))
		return;

	printf("    ACS: cap=0x%04x ctrl=0x%04x", cap, ctrl);
	if (cap & PCI_ACS_SV)
		printf(" SrcValid%s", (ctrl & PCI_ACS_SV) ? "+" : "-");
	if (cap & PCI_ACS_TB)
		printf(" TransBlk%s", (ctrl & PCI_ACS_TB) ? "+" : "-");
	if (cap & PCI_ACS_RR)
		printf(" ReqRedir%s", (ctrl & PCI_ACS_RR) ? "+" : "-");
	if (cap & PCI_ACS_CR)
		printf(" CmpltRedir%s", (ctrl & PCI_ACS_CR) ? "+" : "-");
	if (cap & PCI_ACS_UF)
		printf(" UpFwd%s", (ctrl & PCI_ACS_UF) ? "+" : "-");
	if (cap & PCI_ACS_EC)
		printf(" EgressCtrl%s", (ctrl & PCI_ACS_EC) ? "+" : "-");
	if (cap & PCI_ACS_DT)
		printf(" DirectTrans%s", (ctrl & PCI_ACS_DT) ? "+" : "-");
	printf("\n");
}

static void decode_ext_cap(int device, uint64_t cfg_offset, int id,
			   int cap_offset)
{
	switch (id) {
	case PCI_EXT_CAP_ID_ERR:
		decode_aer(device, cfg_offset, cap_offset);
		break;
	case PCI_EXT_CAP_ID_ARI:
		decode_ari(device, cfg_offset, cap_offset);
		break;
	case PCI_EXT_CAP_ID_SRIOV:
		decode_sriov(device, cfg_offset, cap_offset);
		break;
	case PCI_EXT_CAP_ID_ACS:
		decode_acs(device, cfg_offset, cap_offset);
		break;
	}
}

#define MAX_EXT_CAPS 48

static int walk_ext_capabilities(int device, uint64_t cfg_offset,
				 uint64_t cfg_size)
{
	uint32_t header;
	int offset = 0x100;
	int ids[MAX_EXT_CAPS], offsets[MAX_EXT_CAPS];
	int id, next;
	int count = 0, i;

	if (cfg_size <= 256) {
		if (verbose)
			printf("  no extended config space (size=%llu)\n",
			       (unsigned long long)cfg_size);
		return 0;
	}

	if (pci_cfg_read32(device, cfg_offset, offset, &header))
		return 0;

	if (header == 0 || header == (uint32_t)-1)
		return 0;

	printf("  ext capabilities:");

	while (offset >= 0x100 && offset < (int)cfg_size) {
		if (pci_cfg_read32(device, cfg_offset, offset, &header))
			return -1;

		if (header == 0 || header == (uint32_t)-1)
			break;

		id = PCI_EXT_CAP_ID(header);
		next = PCI_EXT_CAP_NEXT(header);

		if (id < (int)ARRAY_SIZE(ext_cap_names) && ext_cap_names[id])
			printf(" %s", ext_cap_names[id]);
		else
			printf(" 0x%04x", id);

		if (verbose)
			printf("@0x%03x", offset);

		if (count < MAX_EXT_CAPS) {
			ids[count] = id;
			offsets[count] = offset;
		}
		count++;
		offset = next;

		if (!offset || count > MAX_EXT_CAPS)
			break;
	}

	printf(" (%d)\n", count);

	if (verbose) {
		for (i = 0; i < count; i++)
			decode_ext_cap(device, cfg_offset, ids[i], offsets[i]);
	}

	return 0;
}

static int show_bars(int device, uint64_t cfg_offset)
{
	int i, count = 0;
	uint32_t bar, bar_hi;
	struct vfio_region_info region = { .argsz = sizeof(region) };

	for (i = 0; i < 6; i++) {
		if (pci_cfg_read32(device, cfg_offset, PCI_BASE_ADDRESS_0 + i * 4,
			       &bar))
			return -1;

		if (bar == 0)
			continue;

		int is_io = bar & PCI_BASE_ADDRESS_SPACE_IO;
		int is_64 = !is_io &&
			((bar & PCI_BASE_ADDRESS_MEM_TYPE_MASK) ==
			 PCI_BASE_ADDRESS_MEM_TYPE_64);
		int prefetch = !is_io &&
			(bar & PCI_BASE_ADDRESS_MEM_PREFETCH);

		region.index = VFIO_PCI_BAR0_REGION_INDEX + i;
		if (ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &region))
			continue;

		printf("  BAR%d: ", i);

		if (is_io) {
			printf("I/O");
		} else {
			if (is_64) {
				pci_cfg_read32(device, cfg_offset,
					   PCI_BASE_ADDRESS_0 + (i + 1) * 4,
					   &bar_hi);
				printf("mem 0x%08x%08x",
				       bar_hi,
				       (unsigned int)(bar & PCI_BASE_ADDRESS_MEM_MASK));
			} else {
				printf("mem 0x%08x",
				       (unsigned int)(bar & PCI_BASE_ADDRESS_MEM_MASK));
			}
			printf(" %s %s",
			       is_64 ? "64-bit" : "32-bit",
			       prefetch ? "prefetchable" : "non-prefetchable");
		}

		printf(" [size=");
		if (region.size >= (1 << 20))
			printf("%lluMB", (unsigned long long)region.size >> 20);
		else if (region.size >= (1 << 10))
			printf("%lluKB", (unsigned long long)region.size >> 10);
		else
			printf("%llu", (unsigned long long)region.size);
		printf("]");

		if (verbose) {
			printf(" flags=0x%x", region.flags);
			if (region.flags & VFIO_REGION_INFO_FLAG_READ)
				printf(" read");
			if (region.flags & VFIO_REGION_INFO_FLAG_WRITE)
				printf(" write");
			if (region.flags & VFIO_REGION_INFO_FLAG_MMAP)
				printf(" mmap");
		}
		printf("\n");
		count++;

		if (is_64)
			i++;
	}

	/* Expansion ROM */
	region.index = VFIO_PCI_ROM_REGION_INDEX;
	if (!ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &region) &&
	    region.size > 0) {
		printf("  ROM:  [size=%lluKB]",
		       (unsigned long long)region.size >> 10);
		if (verbose) {
			printf(" flags=0x%x", region.flags);
			if (region.flags & VFIO_REGION_INFO_FLAG_READ)
				printf(" read");
			if (region.flags & VFIO_REGION_INFO_FLAG_MMAP)
				printf(" mmap");
		}
		printf("\n");
		count++;
	}

	if (!count)
		printf("  no BARs\n");

	return 0;
}

static int test_write_readback(int device, uint64_t cfg_offset, int cycle,
			       int max_cycles)
{
	uint16_t cmd_orig, cmd_modified, cmd_readback;

	if (pci_cfg_read16(device, cfg_offset, PCI_COMMAND, &cmd_orig))
		return -1;

	/* Toggle bus master bit */
	cmd_modified = cmd_orig ^ PCI_COMMAND_MASTER;
	if (pci_cfg_write16(device, cfg_offset, PCI_COMMAND, cmd_modified))
		return -1;

	if (pci_cfg_read16(device, cfg_offset, PCI_COMMAND, &cmd_readback))
		return -1;

	if (verbose)
		printf("  write/readback [%d/%d]: 0x%04x -> 0x%04x -> 0x%04x",
		       cycle + 1, max_cycles, cmd_orig,
		       cmd_modified, cmd_readback);

	/* Restore */
	if (pci_cfg_write16(device, cfg_offset, PCI_COMMAND, cmd_orig))
		return -1;

	if ((cmd_readback & PCI_COMMAND_MASTER) !=
	    (cmd_modified & PCI_COMMAND_MASTER)) {
		printf(" FAIL\n");
		return 1;
	}

	/* Toggle memory space bit */
	cmd_modified = cmd_orig ^ PCI_COMMAND_MEMORY;
	if (pci_cfg_write16(device, cfg_offset, PCI_COMMAND, cmd_modified))
		return -1;

	if (pci_cfg_read16(device, cfg_offset, PCI_COMMAND, &cmd_readback))
		return -1;

	if (verbose)
		printf(", 0x%04x -> 0x%04x -> 0x%04x",
		       cmd_orig, cmd_modified, cmd_readback);

	/* Restore */
	if (pci_cfg_write16(device, cfg_offset, PCI_COMMAND, cmd_orig))
		return -1;

	if ((cmd_readback & PCI_COMMAND_MEMORY) !=
	    (cmd_modified & PCI_COMMAND_MEMORY)) {
		printf(" FAIL\n");
		return 1;
	}

	if (verbose)
		printf(" OK\n");

	return 0;
}

int main(int argc, char **argv)
{
	const char *devname;
	int opt, ret, container, device;
	int max_cycles = DEFAULT_CYCLES;
	int i;
	struct vfio_device_info device_info = { .argsz = sizeof(device_info) };
	struct vfio_region_info config_info = { .argsz = sizeof(config_info) };

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

	config_info.index = VFIO_PCI_CONFIG_REGION_INDEX;
	ret = ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &config_info);
	if (ret) {
		printf("Failed to get config region info: %s\n",
		       strerror(errno));
		return -1;
	}

	printf("Device %s: config region size=%llu\n", devname,
	       (unsigned long long)config_info.size);

	/* Verify header fields against sysfs */
	ret = verify_header(device, config_info.offset, devname);
	if (ret < 0)
		return -1;
	if (ret > 0) {
		printf("%d header mismatches\n", ret);
		return 1;
	}

	/* Walk PCI capabilities */
	if (walk_capabilities(device, config_info.offset))
		return -1;

	/* Walk PCIe extended capabilities */
	if (walk_ext_capabilities(device, config_info.offset, config_info.size))
		return -1;

	/* Show BARs */
	if (show_bars(device, config_info.offset))
		return -1;

	/* Write/read-back cycles */
	for (i = 0; i < max_cycles; i++) {
		ret = test_write_readback(device, config_info.offset, i,
					  max_cycles);
		if (ret)
			return 1;

		if (!verbose) {
			printf(".");
			fflush(stdout);
		}
	}

	if (!verbose)
		printf("\n");

	printf("%d write/readback cycle%s, Success\n",
	       max_cycles, max_cycles > 1 ? "s" : "");
	return 0;
}
