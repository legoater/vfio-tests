/*
 * VFIO test suite - Intel 82576 (igb) minimal driver
 *
 * Copyright (C) 2012-2026, Red Hat Inc.
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#ifndef IGB_H
#define IGB_H

#include "utils.h"

#define IGB_PCI_VENDOR_ID		0x8086
#define IGB_PCI_DEVICE_ID_82576		0x10C9
#define IGB_PCI_DEVICE_ID_I350		0x1521

/* TX descriptor */
struct igb_tx_desc {
	uint64_t buffer_addr;
	uint32_t cmd_type_len;
	uint32_t olinfo_status;
} __attribute__((packed));

/* RX descriptor */
union igb_rx_desc {
	struct {
		uint64_t pkt_addr;
		uint64_t hdr_addr;
	} read;
	struct {
		uint32_t info;
		uint32_t rss;
		uint32_t status_error;
		uint16_t length;
		uint16_t vlan;
	} wb;
} __attribute__((packed));

#define ETH_FRAME_MIN		60

#define TX_DESC_COUNT		128
#define TX_BUF_SIZE		2048

#define RX_DESC_COUNT		128
#define RX_BUF_SIZE		2048

/* HW requires descriptor rings to be 128-byte aligned */
struct igb_dma_region {
	struct igb_tx_desc tx_ring[TX_DESC_COUNT]
		__attribute__((aligned(128)));
	uint8_t tx_buf[TX_DESC_COUNT][TX_BUF_SIZE];
	union igb_rx_desc rx_ring[RX_DESC_COUNT]
		__attribute__((aligned(128)));
	uint8_t rx_buf[RX_DESC_COUNT][RX_BUF_SIZE];
} __attribute__((aligned(128)));

struct igb {
	struct vfio_dev dev;
	struct igb_dma_region *dma;

	unsigned int tx_idx;
	unsigned int rx_idx;
	uint8_t mac[6];
};

int igb_probe(struct igb *igb, const char *bdf);
int igb_reset(struct igb *igb, unsigned int timeout_ms);
int igb_wait_link(struct igb *igb, unsigned int timeout_ms);
int igb_init_tx(struct igb *igb, unsigned int q);
int igb_init_rx(struct igb *igb, unsigned int q);
void igb_set_loopback(struct igb *igb);
int igb_alloc_dma(struct igb *igb, uint64_t iova);
int igb_send_frame(struct igb *igb, unsigned int q,
		   const void *frame, size_t len, unsigned int timeout_ms);
int igb_send_iova(struct igb *igb, unsigned int q,
		  uint64_t iova, size_t len, unsigned int timeout_ms);
typedef int (*igb_rx_handler_t)(struct igb *igb, unsigned int idx,
				const uint8_t *buf, uint16_t len, void *ctx);
int igb_rx_poll(struct igb *igb, unsigned int q, unsigned int timeout_ms,
		igb_rx_handler_t handler, void *ctx);

int igb_setup_msix(struct igb *igb, unsigned int vector);
void igb_irq_enable(struct igb *igb, unsigned int vector);
void igb_check_tx_stats(struct igb *igb);
void igb_check_rx_stats(struct igb *igb);
void igb_dump_regs(struct igb *igb, unsigned int q);

#endif /* IGB_H */
