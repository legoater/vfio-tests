/*
 * VFIO test suite
 *
 * Copyright (C) 2026 Red Hat, Inc.
 *
 * This work is licensed under the terms of the GNU GPL, version 2. See
 * the COPYING file in the top-level directory.
 *
 *  Prerequisite: vfio-pci must be loaded with 'enable_sriov=1' so
 *  that the PF can create its VFs while bound to vfio-pci.
 *
 *  1. Open the PF through VFIO and assign a VF token to it.
 *  2. Close the PF device FD and its VFIO container, leaving the PF bound
 *     to vfio-pci.
 *  3. Open the VF through VFIO using the assigned token.
 *  4. The test succeeds if the VF can be opened with the assigned token.
*/

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/vfio.h>

#include "utils.h"

static void usage(const char *name)
{
	printf("usage: %s <PF BDF> [<VF BDF>]\n", name);
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
	char device_name[256];
	int groupid;
	int container = -1;
	int group = -1;
	int device = -1;
	int ret;

	groupid = vfio_device_get_groupid(vf_bdf);
	if (groupid < 0)
		return -1;

	ret = vfio_group_attach(groupid, &container, &group);
	if (ret)
		return -1;

	ret = snprintf(device_name, sizeof(device_name), "%s vf_token=%s",
		       vf_bdf, token);
	if (ret < 0 || ret >= sizeof(device_name)) {
		printf("VF token device name is too long\n");
		return -1;
	}

	device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, device_name);
	if (device < 0) {
		printf("Failed to get device %s: %d (%s)\n", device_name,
		       errno, strerror(errno));
		return -1;
	}

	close(device);
	close(group);
	close(container);
	return 0;
}

int main(int argc, char **argv)
{
	const char *pf_bdf;
	const char *vf_bdf;
	int pf_container = -1;
	int pf_device = -1;
	int ret;

	if (argc < 2) {
		usage(argv[0]);
		return -1;
	}

	pf_bdf = argv[1];
	if (vfio_pci_is_vf(pf_bdf)) {
                printf("Skipping: %s is a VF\n", pf_bdf);
                return EXIT_SKIP;
        }

	vf_bdf = argv[2];
	if (vf_bdf && !vfio_pci_is_vf(vf_bdf)) {
                printf("%s should be a VF of %s\n", vf_bdf, pf_bdf);
                return -1;
        }

	ret = vfio_device_attach(pf_bdf, &pf_container, &pf_device, NULL);
	if (ret)
		return -1;

	ret = set_vf_token(pf_device, VF_TOKEN);
	if (ret)
		return -1;

	printf("VF token successfully assigned to PF\n");

	/* Close PF VFIO device fd to check token persistence */
	close(pf_device);
	close(pf_container);

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
