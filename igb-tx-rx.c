/*
 * VFIO test suite - Intel 82576 (igb) TX/RX DMA test
 *
 * Copyright (C) 2012-2026, Red Hat Inc.
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "drivers/igb.h"

#define DMA_IOVA		0x100000000ULL
#define TX_QUEUE		0
#define RX_QUEUE		0
#define TX_TIMEOUT_MS		1000
#define LINK_TIMEOUT_MS		5000
#define SWITCH_SETTLE_MS	2000

static void build_test_frame(uint8_t *buf, size_t bufsize,
			     const uint8_t *src_mac)
{
	size_t i;

	assert(src_mac);

	memset(buf, 0, bufsize);

	for (i = 0; i < 6; i++)
		buf[i] = 0xff;

	memcpy(buf + 6, src_mac, 6);

	buf[12] = 0x88;
	buf[13] = 0xb5;

	for (i = 14; i < ETH_FRAME_MIN && i < bufsize; i++)
		buf[i] = (uint8_t)i;
}

struct igb_rx_match_ctx {
	const uint8_t *expected;
	size_t len;
};

static int igb_rx_match_frame(struct igb *igb, unsigned int idx,
			      const uint8_t *buf, uint16_t length, void *ctx)
{
	struct igb_rx_match_ctx *m = ctx;

	if (length >= m->len && memcmp(buf, m->expected, m->len) == 0) {
		printf("%s igb: RX complete descriptor %d length=%u status=%08x\n",
		       igb->dev.name, idx, length,
		       igb->dma->rx_ring[idx].wb.status_error);
		printf("%s igb: RX payload verified %zu bytes match\n",
		       igb->dev.name, m->len);
		return 0;
	}

	if (verbose)
		printf("RX[%d]: length=%u status=%08x (not our frame)\n",
		       idx, length, igb->dma->rx_ring[idx].wb.status_error);

	return -1;
}

static int igb_send(struct igb *igb, unsigned int queue,
		    unsigned int timeout_ms)
{
	uint8_t frame[ETH_FRAME_MIN];

	build_test_frame(frame, sizeof(frame), igb->mac);

	if (igb_send_frame(igb, queue, frame, sizeof(frame), timeout_ms)) {
		fprintf(stderr, "%s: test frame not sent (timeout)\n",
			__func__);
		return -1;
	}

	return 0;
}

static int igb_receive(struct igb *igb, unsigned int queue,
		       unsigned int timeout_ms, const uint8_t *src_mac)
{
	uint8_t expected[ETH_FRAME_MIN];
	struct igb_rx_match_ctx ctx = {
		.expected = expected,
		.len = ETH_FRAME_MIN,
	};

	build_test_frame(expected, sizeof(expected), src_mac);

	if (igb_rx_poll(igb, queue, timeout_ms, igb_rx_match_frame, &ctx)) {
		fprintf(stderr, "%s: test frame not received (timeout)\n",
			__func__);
		return -1;
	}

	return 0;
}

enum igb_mode {
	IGB_MODE_TX,
	IGB_MODE_RX,
	IGB_MODE_LOOPBACK,
};

static int igb_open(struct igb *igb, const char *bdf, enum igb_mode mode)
{
	int ret;

	ret = igb_probe(igb, bdf);
	if (ret)
		return ret;

	if (igb_reset(igb, TX_TIMEOUT_MS))
		return 1;

	if (igb_wait_link(igb, LINK_TIMEOUT_MS))
		return 1;

	if (vfio_dev_set_bus_master(&igb->dev, true))
		return 1;

	if (igb_alloc_dma(igb, DMA_IOVA))
		return 1;

	switch (mode) {
	case IGB_MODE_TX:
		if (igb_init_tx(igb, TX_QUEUE))
			return 1;
		break;
	case IGB_MODE_RX:
		if (igb_init_rx(igb, RX_QUEUE))
			return 1;
		break;
	case IGB_MODE_LOOPBACK:
		if (igb_init_tx(igb, TX_QUEUE))
			return 1;
		if (igb_init_rx(igb, RX_QUEUE))
			return 1;
		igb_set_loopback(igb);
		break;
	}

	return 0;
}

void usage(char *name)
{
	fprintf(stderr, "usage: %s [-l] <tx-bdf> [rx-bdf]\n", name);
	fprintf(stderr, "\nIntel 82576 (igb) TX/RX DMA test (iommufd)\n");
	fprintf(stderr, "  -l  loopback: TX and RX on a single device\n");
}

int main(int argc, char **argv)
{
	struct igb igb_tx = {
		.dev = VFIO_DEV_INIT,
	};
	struct igb igb_rx = {
		.dev = VFIO_DEV_INIT,
	};
	const char *tx_bdf;
	const char *rx_bdf;
	int loopback = 0;
	int opt, ret;

	while ((opt = getopt(argc, argv, "l")) != -1) {
		switch (opt) {
		case 'l':
			loopback = 1;
			break;
		default:
			usage(argv[0]);
			return -1;
		}
	}

	if (optind >= argc) {
		usage(argv[0]);
		return -1;
	}

	argv += optind;

	tx_bdf = argv[0];
	rx_bdf = argv[1];

	if (loopback) {
		ret = igb_open(&igb_tx, tx_bdf, IGB_MODE_LOOPBACK);
		if (ret)
			return ret;
	} else {
		ret = igb_open(&igb_tx, tx_bdf, IGB_MODE_TX);
		if (ret)
			return ret;

		if (rx_bdf) {
			ret = igb_open(&igb_rx, rx_bdf, IGB_MODE_RX);
			if (ret)
				return ret;

			/*
			 * On real HW, the physical switch needs time to learn
			 * the newly-active ports after link up. QEMU socket
			 * netdev forwards frames immediately.
			 */
			printf("Waiting for switch to learn ports...\n");
			usleep(SWITCH_SETTLE_MS * 1000);
		}
	}

	if (igb_send(&igb_tx, TX_QUEUE, TX_TIMEOUT_MS))
		return 1;

	igb_check_tx_stats(&igb_tx);

	if (loopback || rx_bdf) {
		struct igb *rx = loopback ? &igb_tx : &igb_rx;

		if (igb_receive(rx, RX_QUEUE, TX_TIMEOUT_MS, igb_tx.mac))
			return 1;
		igb_check_rx_stats(rx);
	}

	if (verbose)
		igb_dump_regs(&igb_tx, TX_QUEUE);

	printf("Success\n");

	vfio_dev_close(&igb_tx.dev);

	if (rx_bdf)
		vfio_dev_close(&igb_rx.dev);

	return 0;
}
