/*
 * VFIO test suite
 *
 * Copyright (C) 2026 Red Hat, Inc.
 *
 * This work is licensed under the terms of the GNU GPL, version 2. See
 * the COPYING file in the top-level directory.
 *
 * Test the VF token mechanism through the IOMMUFD-backed VFIO device path:
 *
 *  Prerequisites:
 *   - vfio-pci must be loaded with enable_sriov=1 so that the PF can
 *     create its VFs while bound to vfio-pci.
 *
 *  1. Verify that the first device is an SR-IOV PF and the second one is a VF.
 *  2. Open the PF through VFIO and attach it to IOMMUFD.
 *  3. Assign a VF token to the PF.
 *  4. Close the PF device FD and IOMMUFD, leaving the PF bound to vfio-pci.
 *  5. Open the VF through VFIO and attach it to IOMMUFD using the token.
 *  6. The test succeeds if the VF can be opened with the assigned token.
*/

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <linux/iommufd.h>
#include <linux/vfio.h>

#include "utils.h"

void usage(char *name)
{
	printf("usage: %s <PF BDF> <VF BDF>\n", name);
}

static int set_vf_token(int device, const char *token)
{
	struct {
		struct vfio_device_feature feature;
		unsigned char token[VF_TOKEN_SIZE];
	} data = {
		.feature = {
			.argsz = sizeof(data),
			.flags = VFIO_DEVICE_FEATURE_SET |
				 VFIO_DEVICE_FEATURE_PCI_VF_TOKEN,
		},
	};

	if (parse_vf_token(token, data.token)) {
		fprintf(stderr, "Invalid VF token: %s\n", token);
		return -1;
	}

	if (ioctl(device, VFIO_DEVICE_FEATURE, &data.feature)) {
		fprintf(stderr, "Failed to set VF token: %d (%s)\n",
			errno, strerror(errno));
		return -1;
	}

	printf("VF token %s successfully assigned to PF\n", token);
	return 0;
}

static int open_vf(const char *vf_bdf, const char *token)
{
	int vf_device = -1;
	int vf_iommufd = -1;
	int vf_ioas_id;

	vf_iommufd = open("/dev/iommu", O_RDWR);
	if (vf_iommufd < 0) {
		printf("Failed to open /dev/iommu: %d (%s)\n",
		       errno, strerror(errno));
		return -1;
	}

	if (vfio_device_iommufd_attach_with_token(vf_iommufd, vf_bdf,
				       &vf_device, &vf_ioas_id, token)) {
		return -1;
	}

	close(vf_device);
	close(vf_iommufd);
	return 0;
}

int main(int argc, char **argv)
{
	const char *pf_bdf;
	const char *vf_bdf;
	int pf_device = -1;
	int pf_iommufd = -1;
	int pf_ioas_id;
	int ret;

	if (argc < 2) {
		usage(argv[0]);
		return -1;
	}

	pf_bdf = argv[1];
	if (vfio_pci_is_vf(pf_bdf)) {
		printf("%s is a VF, expected a PF\n", pf_bdf);
		return -1;
	}

	vf_bdf = argv[2];
	if (vf_bdf && !vfio_pci_is_vf(vf_bdf)) {
		printf("%s is not a VF\n", vf_bdf);
		return -1;
	}

	pf_iommufd = open("/dev/iommu", O_RDWR);
	if (pf_iommufd < 0) {
		printf("Failed to open /dev/iommu: %d (%s)\n",
		       errno, strerror(errno));
		return -1;
	}

	if (vfio_device_iommufd_attach(pf_iommufd, pf_bdf, &pf_device,
				       &pf_ioas_id)) {
		return -1;
	}

	if (set_vf_token(pf_device, VF_TOKEN))
		return -1;

	printf("VF token successfully assigned to PF\n");

	/* Close PF VFIO device fd to check token persistence */
	close(pf_device);
	close(pf_iommufd);

	if (vf_bdf) {
		/* Now, open the VF using the token assigned to its PF. */
		ret = open_vf(vf_bdf, VF_TOKEN);
		if (ret)
			return -1;

		printf("Successfully opened VF %s with vf_token=%s\n",
		       vf_bdf, VF_TOKEN);

		/* Now, open the VF using the token assigned to its PF. */
		ret = open_vf(vf_bdf, VF_TOKEN_BOGUS);
		if (!ret)
			return -1;

		printf("open VF %s with vf_token=%s failed as expected\n",
		       vf_bdf, VF_TOKEN_BOGUS);

	}

	printf("Success\n");
	return 0;
}
