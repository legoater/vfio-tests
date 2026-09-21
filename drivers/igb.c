/*
 * VFIO test suite - Intel 82576 (igb) minimal driver
 *
 * Copyright (C) 2012-2026, Red Hat Inc.
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Minimal userspace driver for TX/RX on the Intel 82576 (igb).
 * One TX queue, one RX queue.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "drivers/igb.h"

/*
 * 82575/82576 registers
 */
#define E1000_CTRL		0x00000
#define E1000_STATUS		0x00008
#define E1000_STATUS_LU		0x00000002
#define E1000_STATUS_FUNC_MASK	0x0000000C
#define E1000_STATUS_FUNC_SHIFT	2
#define E1000_CTRL_EXT		0x00018
#define E1000_CTRL_EXT_DRV_LOAD	0x10000000

/* TX statistics (read-to-clear) */
#define E1000_GPTC		0x04080
#define E1000_GOTCL		0x04090
#define E1000_GOTCH		0x04094

/* TX error counters */
#define E1000_ECOL		0x04018
#define E1000_LATECOL		0x0401C
#define E1000_COLC		0x04028
#define E1000_DC		0x04030
#define E1000_TNCRS		0x04034
#define E1000_CEXTERR		0x0403C

#define E1000_TCTL		0x00400

#define E1000_TDBAL(_n) \
	((_n) < 4 ? (0x03800 + ((_n) * 0x100)) : \
		    (0x0e000 + ((_n) * 0x040)))
#define E1000_TDBAH(_n) \
	((_n) < 4 ? (0x03804 + ((_n) * 0x100)) : \
		    (0x0e004 + ((_n) * 0x040)))
#define E1000_TDLEN(_n) \
	((_n) < 4 ? (0x03808 + ((_n) * 0x100)) : \
		    (0x0e008 + ((_n) * 0x040)))
#define E1000_TDH(_n) \
	((_n) < 4 ? (0x03810 + ((_n) * 0x100)) : \
		    (0x0e010 + ((_n) * 0x040)))
#define E1000_TDT(_n) \
	((_n) < 4 ? (0x03818 + ((_n) * 0x100)) : \
		    (0x0e018 + ((_n) * 0x040)))
#define E1000_TXDCTL(_n) \
	((_n) < 4 ? (0x03828 + ((_n) * 0x100)) : \
		    (0x0e028 + ((_n) * 0x040)))

#define E1000_RCTL		0x00100

#define E1000_RDBAL(_n) \
	((_n) < 4 ? (0x02800 + ((_n) * 0x100)) : \
		    (0x0C000 + ((_n) * 0x040)))
#define E1000_RDBAH(_n) \
	((_n) < 4 ? (0x02804 + ((_n) * 0x100)) : \
		    (0x0C004 + ((_n) * 0x040)))
#define E1000_RDLEN(_n) \
	((_n) < 4 ? (0x02808 + ((_n) * 0x100)) : \
		    (0x0C008 + ((_n) * 0x040)))
#define E1000_RDH(_n) \
	((_n) < 4 ? (0x02810 + ((_n) * 0x100)) : \
		    (0x0C010 + ((_n) * 0x040)))
#define E1000_RDT(_n) \
	((_n) < 4 ? (0x02818 + ((_n) * 0x100)) : \
		    (0x0C018 + ((_n) * 0x040)))
#define E1000_RXDCTL(_n) \
	((_n) < 4 ? (0x02828 + ((_n) * 0x100)) : \
		    (0x0C028 + ((_n) * 0x040)))
#define E1000_SRRCTL(_n) \
	((_n) < 4 ? (0x0280C + ((_n) * 0x100)) : \
		    (0x0C00C + ((_n) * 0x040)))

/* Receive Address */
#define E1000_RAL(_n)		(0x05400 + ((_n) * 0x08))
#define E1000_RAH(_n)		(0x05404 + ((_n) * 0x08))

/* RX statistics (read-to-clear) */
#define E1000_GPRC		0x04074
#define E1000_GORCL		0x04088
#define E1000_GORCH		0x0408C

/* CTRL bits */
#define E1000_CTRL_FD		0x00000001
#define E1000_CTRL_RST		0x04000000

/* TCTL - Transmit Control */
#define E1000_TCTL_EN		0x00000002
#define E1000_TCTL_PSP		0x00000008
#define E1000_TCTL_CT		0x00000ff0
#define E1000_TCTL_COLD		0x003ff000
#define E1000_TCTL_RTLC		0x01000000

#define E1000_COLLISION_THRESHOLD	15
#define E1000_CT_SHIFT			4
#define E1000_COLLISION_DISTANCE	63
#define E1000_COLD_SHIFT		12

/* TXDCTL bits */
#define E1000_TXDCTL_PTHRESH(x)		((uint32_t)(x) & 0x7f)
#define E1000_TXDCTL_HTHRESH(x)		(((uint32_t)(x) & 0x1f) << 8)
#define E1000_TXDCTL_WTHRESH(x)		(((uint32_t)(x) & 0x1f) << 16)
#define E1000_TXDCTL_QUEUE_ENABLE	0x02000000

#define IGB_TX_PTHRESH		8
#define IGB_TX_HTHRESH		1
#define IGB_TX_WTHRESH		1

/* RCTL - Receive Control */
#define E1000_RCTL_EN		0x00000002
#define E1000_RCTL_LBM_MAC	0x00000040
#define E1000_RCTL_BAM		0x00008000
#define E1000_RCTL_SECRC	0x04000000

/* RXDCTL bits */
#define E1000_RXDCTL_QUEUE_ENABLE	0x02000000

/* GCR - PCI-Express Control */
#define E1000_GCR			0x05B00
#define E1000_GCR_CMPL_TMOUT_RESEND	0x00010000

/* VMOLR - VM Offload Register (82576+, per-pool RX filter) */
#define E1000_VMOLR(_n)			(0x05AD0 + (4 * (_n)))
#define E1000_VMOLR_AUPE		0x01000000
#define E1000_VMOLR_BAM			0x08000000

/* MSI-X interrupt registers */
#define E1000_GPIE		0x01514
#define E1000_EICR		0x01580
#define E1000_EICS		0x01520
#define E1000_EIMS		0x01524
#define E1000_EIMC		0x01528
#define E1000_EIAC		0x0152C
#define E1000_EIAM		0x01530
#define E1000_IVAR0		0x01700

#define E1000_GPIE_MSIX_MODE	0x00000010
#define E1000_GPIE_EIAME	0x40000000
#define E1000_IVAR_VALID	0x80

/* SRRCTL bits */
#define E1000_SRRCTL_DESCTYPE_ONEBUF	0x02000000

/* TX descriptor bits */
#define E1000_TXD_DTYP_DATA	0x00300000
#define E1000_TXD_DCMD_EOP	0x01000000
#define E1000_TXD_DCMD_IFCS	0x02000000
#define E1000_TXD_DCMD_RS	0x08000000
#define E1000_TXD_DCMD_DEXT	0x20000000
#define E1000_TXD_PAYLEN_SHIFT	14
#define E1000_TXD_STAT_DD	0x00000001

/* RX descriptor bits */
#define E1000_RXD_STAT_DD	0x00000001

/* EEPROM */
#define E1000_EERD		0x00014
#define E1000_EERD_START	0x00000001
#define E1000_EERD_DONE		0x00000002
#define E1000_EERD_ADDR_SHIFT	2
#define E1000_EERD_DATA_SHIFT	16
#define E1000_RAH_AV		0x80000000

/* PHY (MDIC) */
#define E1000_MDIC		0x00020
#define E1000_MDIC_DATA_MASK	0x0000FFFF
#define E1000_MDIC_REG_SHIFT	16
#define E1000_MDIC_PHY_SHIFT	21
#define E1000_MDIC_OP_READ	0x08000000
#define E1000_MDIC_OP_WRITE	0x04000000
#define E1000_MDIC_READY	0x10000000
#define E1000_MDIC_ERROR	0x40000000
#define E1000_PHY_ADDR		1

#define MII_BMCR		0
#define BMCR_RESET		0x8000
#define BMCR_ANENABLE		0x1000
#define BMCR_ANRESTART		0x0200


/* ---- Probe ---- */

int igb_probe(struct igb *igb, const char *bdf)
{
	unsigned int vendor = vfio_pci_vendor(bdf);
	unsigned int device = vfio_pci_device(bdf);

	if (vendor != IGB_PCI_VENDOR_ID)
		goto skip;

	switch (device) {
	case IGB_PCI_DEVICE_ID_82576:
	case IGB_PCI_DEVICE_ID_I350:
		break;
	default:
		goto skip;
	}

	if (vfio_dev_open(&igb->dev, bdf))
		return 1;

	if (vfio_dev_map_bar(&igb->dev, 0))
		return 1;

	return 0;

skip:
	fprintf(stderr, "%s: not an igb device (%04x:%04x), skipping\n",
		bdf, vendor, device);
	return EXIT_SKIP;
}

/* ---- EEPROM ---- */

static int igb_read_nvm_word(struct igb *igb, uint8_t offset, uint16_t *data)
{
	uint32_t eerd;
	int i;

	vfio_dev_reg_write(&igb->dev, E1000_EERD,
			   ((uint32_t)offset << E1000_EERD_ADDR_SHIFT) |
			   E1000_EERD_START);

	for (i = 0; i < 1000; i++) {
		eerd = vfio_dev_reg_read(&igb->dev, E1000_EERD);

		if (eerd & E1000_EERD_DONE)
			break;

		usleep(10);
	}

	if (!(eerd & E1000_EERD_DONE)) {
		fprintf(stderr, "%s: offset=%u EERD=%08x\n", __func__,
			offset, eerd);
		return -1;
	}

	*data = (uint16_t)(eerd >> E1000_EERD_DATA_SHIFT);
	return 0;
}

/*
 * On real HW, reset zeroes RAL/RAH. The MAC must be read from EEPROM
 * and programmed back. QEMU restores RAL/RAH from its config
 * automatically (e1000x_reset_mac_addr), so this is a no-op there.
 *
 * On a dual-port 82576, EEPROM words 0-2 hold the base MAC. Each port
 * adds its function ID (STATUS[3:2]) to the last byte.
 */
static int igb_load_mac(struct igb *igb)
{
	uint16_t nvm_word;
	uint32_t status, func_id;
	uint32_t ral, rah;
	int i;

	for (i = 0; i < 3; i++) {
		if (igb_read_nvm_word(igb, i, &nvm_word))
			return -1;

		igb->mac[i * 2]     = nvm_word & 0xff;
		igb->mac[i * 2 + 1] = (nvm_word >> 8) & 0xff;
	}

	status = vfio_dev_reg_read(&igb->dev, E1000_STATUS);
	func_id = (status & E1000_STATUS_FUNC_MASK) >> E1000_STATUS_FUNC_SHIFT;
	igb->mac[5] += func_id;

	ral = igb->mac[0] | ((uint32_t)igb->mac[1] << 8) |
	      ((uint32_t)igb->mac[2] << 16) | ((uint32_t)igb->mac[3] << 24);
	rah = igb->mac[4] | ((uint32_t)igb->mac[5] << 8) | E1000_RAH_AV;

	vfio_dev_reg_write(&igb->dev, E1000_RAL(0), ral);
	vfio_dev_reg_write(&igb->dev, E1000_RAH(0), rah);

	printf("%s igb: MAC (EEPROM) = %02x:%02x:%02x:%02x:%02x:%02x\n",
	       igb->dev.name, igb->mac[0], igb->mac[1], igb->mac[2],
	       igb->mac[3], igb->mac[4], igb->mac[5]);

	return 0;
}

/* ---- PHY ---- */

static int igb_read_phy_reg(struct igb *igb, uint8_t reg, uint16_t *data)
{
	uint32_t mdic;
	int i;

	mdic = E1000_MDIC_OP_READ |
		((uint32_t)E1000_PHY_ADDR << E1000_MDIC_PHY_SHIFT) |
		((uint32_t)reg << E1000_MDIC_REG_SHIFT);
	vfio_dev_reg_write(&igb->dev, E1000_MDIC, mdic);

	for (i = 0; i < 1000; i++) {
		mdic = vfio_dev_reg_read(&igb->dev, E1000_MDIC);

		if (mdic & E1000_MDIC_READY)
			break;

		usleep(10);
	}

	if (!(mdic & E1000_MDIC_READY)) {
		fprintf(stderr, "%s: reg=%u MDIC=%08x\n", __func__, reg, mdic);
		return -1;
	}

	if (mdic & E1000_MDIC_ERROR) {
		fprintf(stderr, "%s: reg=%u MDIC=%08x\n", __func__, reg, mdic);
		return -1;
	}

	*data = mdic & E1000_MDIC_DATA_MASK;
	return 0;
}

static int igb_write_phy_reg(struct igb *igb, uint8_t reg, uint16_t data)
{
	uint32_t mdic;
	int i;

	mdic = ((reg << E1000_MDIC_REG_SHIFT) |
		(E1000_PHY_ADDR << E1000_MDIC_PHY_SHIFT) |
		E1000_MDIC_OP_WRITE |
		data);

	vfio_dev_reg_write(&igb->dev, E1000_MDIC, mdic);

	for (i = 0; i < 1000; i++) {
		usleep(10);

		mdic = vfio_dev_reg_read(&igb->dev, E1000_MDIC);

		if (mdic & E1000_MDIC_READY)
			break;
	}

	if (!(mdic & E1000_MDIC_READY)) {
		fprintf(stderr, "%s: reg=%u data=%04x MDIC=%08x\n",
		        __func__, reg, data, mdic);
		return -1;
	}

	if (mdic & E1000_MDIC_ERROR) {
		fprintf(stderr, "%s: reg=%u data=%04x MDIC=%08x\n",
		        __func__, reg, data, mdic);
		return -1;
	}

	return 0;
}

static void igb_dump_phy(struct igb *igb)
{
	uint16_t v;

	printf("%s igb/phy: registers:\n", igb->dev.name);

	for (int reg = 0; reg <= 9; reg++) {
		if (igb_read_phy_reg(igb, reg, &v) == 0)
			printf("  PHY[%02d] = %04x\n", reg, v);
	}

	for (int reg = 16; reg <= 19; reg++) {
		if (igb_read_phy_reg(igb, reg, &v) == 0)
			printf("  PHY[%02d] = %04x\n", reg, v);
	}
}

static int igb_phy_reset(struct igb *igb)
{
	uint16_t bmcr;
	int i;

	if (igb_read_phy_reg(igb, MII_BMCR, &bmcr))
		return -1;

	bmcr |= BMCR_RESET;

	if (igb_write_phy_reg(igb, MII_BMCR, bmcr))
		return -1;

	for (i = 0; i < 1000; i++) {
		usleep(100);

		if (igb_read_phy_reg(igb, MII_BMCR, &bmcr))
			return -1;

		if (!(bmcr & BMCR_RESET))
			break;
	}

	if (bmcr & BMCR_RESET) {
		fprintf(stderr, "%s: BMCR=%04x\n", __func__, bmcr);
		return -1;
	}

	printf("%s igb/phy: reset complete BMCR=%04x\n", igb->dev.name, bmcr);

	if (verbose)
		igb_dump_phy(igb);

	return 0;
}

static int igb_phy_restart_autoneg(struct igb *igb)
{
	uint16_t bmcr;

	if (igb_read_phy_reg(igb, MII_BMCR, &bmcr))
		return -1;

	bmcr |= BMCR_ANENABLE | BMCR_ANRESTART;

	if (igb_write_phy_reg(igb, MII_BMCR, bmcr))
		return -1;

	printf("%s igb/phy: autoneg triggered BMCR=%04x\n", igb->dev.name, bmcr);
	return 0;
}

/* ---- HW control ---- */

/*
 * On real HW, DRV_LOAD tells the management firmware that a driver
 * is active. Without it, the firmware may interfere with TX/RX.
 * QEMU has no management firmware, so this is a no-op there.
 */
static void igb_get_hw_control(struct igb *igb)
{
	uint32_t ctrl_ext;

	ctrl_ext = vfio_dev_reg_read(&igb->dev, E1000_CTRL_EXT);
	ctrl_ext |= E1000_CTRL_EXT_DRV_LOAD;
	vfio_dev_reg_write(&igb->dev, E1000_CTRL_EXT, ctrl_ext);
}

/* ---- Debug dumps ---- */

static void igb_dump_tx_queue(struct igb *igb, unsigned int q)
{
	printf("%s igb: TX queue %u:\n", igb->dev.name, q);
	printf("  TDBAL     = %08x\n", vfio_dev_reg_read(&igb->dev, E1000_TDBAL(q)));
	printf("  TDBAH     = %08x\n", vfio_dev_reg_read(&igb->dev, E1000_TDBAH(q)));
	printf("  TDLEN     = %08x\n", vfio_dev_reg_read(&igb->dev, E1000_TDLEN(q)));
	printf("  TDH       = %08x\n", vfio_dev_reg_read(&igb->dev, E1000_TDH(q)));
	printf("  TDT       = %08x\n", vfio_dev_reg_read(&igb->dev, E1000_TDT(q)));
	printf("  TXDCTL    = %08x\n", vfio_dev_reg_read(&igb->dev, E1000_TXDCTL(q)));
}

static void igb_dump_tx_descriptor(struct igb_tx_desc *d)
{
	printf("TX descriptor:\n");
	printf("  buffer = 0x%016" PRIx64 "\n", d->buffer_addr);
	printf("  cmd    = 0x%08x\n", d->cmd_type_len);
	printf("  len    = %u\n", d->cmd_type_len & 0xffff);
	printf("  olinfo = 0x%08x\n", d->olinfo_status);
}

static void igb_dump_rx_queue(struct igb *igb, unsigned int q)
{
	printf("%s igb: RX queue %u:\n", igb->dev.name, q);
	printf("  RDBAL     = %08x\n", vfio_dev_reg_read(&igb->dev, E1000_RDBAL(q)));
	printf("  RDBAH     = %08x\n", vfio_dev_reg_read(&igb->dev, E1000_RDBAH(q)));
	printf("  RDLEN     = %08x\n", vfio_dev_reg_read(&igb->dev, E1000_RDLEN(q)));
	printf("  RDH       = %08x\n", vfio_dev_reg_read(&igb->dev, E1000_RDH(q)));
	printf("  RDT       = %08x\n", vfio_dev_reg_read(&igb->dev, E1000_RDT(q)));
	printf("  RXDCTL    = %08x\n", vfio_dev_reg_read(&igb->dev, E1000_RXDCTL(q)));
	printf("  SRRCTL    = %08x\n", vfio_dev_reg_read(&igb->dev, E1000_SRRCTL(q)));
}

void igb_dump_regs(struct igb *igb, unsigned int q)
{
	printf("%s igb: registers:\n", igb->dev.name);
	printf("  CTRL      = %08x\n", vfio_dev_reg_read(&igb->dev, E1000_CTRL));
	printf("  STATUS    = %08x\n", vfio_dev_reg_read(&igb->dev, E1000_STATUS));
	printf("  CTRL_EXT  = %08x\n", vfio_dev_reg_read(&igb->dev, E1000_CTRL_EXT));
	printf("  TCTL      = %08x\n", vfio_dev_reg_read(&igb->dev, E1000_TCTL));
	printf("  RCTL      = %08x\n", vfio_dev_reg_read(&igb->dev, E1000_RCTL));
	igb_dump_tx_queue(igb, q);
	igb_dump_rx_queue(igb, q);
}

/* ---- Reset / link ---- */

int igb_reset(struct igb *igb, unsigned int timeout_ms)
{
	uint32_t ctrl;
	unsigned int i;

	printf("%s igb: resetting device\n", igb->dev.name);

	ctrl = vfio_dev_reg_read(&igb->dev, E1000_CTRL);
	ctrl |= E1000_CTRL_RST;
	vfio_dev_reg_write(&igb->dev, E1000_CTRL, ctrl);

	for (i = 0; i < timeout_ms; i++) {
		ctrl = vfio_dev_reg_read(&igb->dev, E1000_CTRL);

		if (!(ctrl & E1000_CTRL_RST))
			break;

		usleep(1000);
	}

	if (ctrl & E1000_CTRL_RST) {
		fprintf(stderr, "%s: timeout CTRL=%08x\n", __func__, ctrl);
		return -1;
	}

	printf("%s igb: reset complete CTRL=%08x STATUS=%08x CTRL_EXT=%08x\n",
	       igb->dev.name,
	       vfio_dev_reg_read(&igb->dev, E1000_CTRL),
	       vfio_dev_reg_read(&igb->dev, E1000_STATUS),
	       vfio_dev_reg_read(&igb->dev, E1000_CTRL_EXT));

	/*
	 * Disable PCIe completion timeout resend. Without this, a DMA
	 * read to an unreachable IOVA (e.g. failed P2P) retries
	 * forever and blocks FLR on device close.
	 */
	ctrl = vfio_dev_reg_read(&igb->dev, E1000_GCR);
	ctrl &= ~E1000_GCR_CMPL_TMOUT_RESEND;
	vfio_dev_reg_write(&igb->dev, E1000_GCR, ctrl);

	if (igb_load_mac(igb))
		return -1;

	igb_get_hw_control(igb);

	if (igb_phy_reset(igb))
		return -1;

	if (igb_phy_restart_autoneg(igb))
		return -1;

	return 0;
}

int igb_wait_link(struct igb *igb, unsigned int timeout_ms)
{
	uint32_t status;
	unsigned int i;
	unsigned int intervals = timeout_ms / 100;

	printf("%s igb: waiting for link ", igb->dev.name);
	fflush(stdout);

	for (i = 0; i < intervals; i++) {
		status = vfio_dev_reg_read(&igb->dev, E1000_STATUS);

		if (status & E1000_STATUS_LU) {
			printf("up (%u ms) STATUS=%08x\n", i * 100, status);
			return 0;
		}

		printf(".");
		fflush(stdout);
		usleep(100000);
	}

	printf("\n");
	status = vfio_dev_reg_read(&igb->dev, E1000_STATUS);
	fprintf(stderr, "%s: timeout STATUS=%08x\n", __func__, status);
	return -1;
}

int igb_alloc_dma(struct igb *igb, uint64_t iova)
{
	if (vfio_dev_dma_alloc(&igb->dev, sizeof(struct igb_dma_region), iova))
		return -1;

	igb->dma = igb->dev.dma_va;

	return 0;
}

/* ---- TX ---- */

static void igb_clear_tx_stats(struct igb *igb)
{
	vfio_dev_reg_read(&igb->dev, E1000_GPTC);
	vfio_dev_reg_read(&igb->dev, E1000_GOTCL);
	vfio_dev_reg_read(&igb->dev, E1000_GOTCH);
}

void igb_check_tx_stats(struct igb *igb)
{
	uint32_t gptc, gotcl, gotch;
	uint64_t gotc;

	gptc = vfio_dev_reg_read(&igb->dev, E1000_GPTC);
	gotcl = vfio_dev_reg_read(&igb->dev, E1000_GOTCL);
	gotch = vfio_dev_reg_read(&igb->dev, E1000_GOTCH);
	gotc = ((uint64_t)gotch << 32) | gotcl;

	printf("%s igb: TX stats GPTC=%u GOTC=%" PRIu64 "\n", igb->dev.name, gptc, gotc);

	if (verbose)
		printf("TX errors: ECOL=%u LATECOL=%u COLC=%u DC=%u TNCRS=%u CEXTERR=%u\n",
		       vfio_dev_reg_read(&igb->dev, E1000_ECOL),
		       vfio_dev_reg_read(&igb->dev, E1000_LATECOL),
		       vfio_dev_reg_read(&igb->dev, E1000_COLC),
		       vfio_dev_reg_read(&igb->dev, E1000_DC),
		       vfio_dev_reg_read(&igb->dev, E1000_TNCRS),
		       vfio_dev_reg_read(&igb->dev, E1000_CEXTERR));

	if (gptc == 0)
		fprintf(stderr, "%s: no good packets (no link?)\n", __func__);
}

static int igb_setup_tx(struct igb *igb, unsigned int q)
{
	size_t ring_size = sizeof(igb->dma->tx_ring);
	uint64_t ring_iova = vfio_dev_to_iova(&igb->dev, igb->dma->tx_ring);
	uint32_t txdctl;

	memset(igb->dma->tx_ring, 0, ring_size);

	vfio_dev_reg_write(&igb->dev, E1000_TXDCTL(q), 0);
	vfio_dev_reg_read(&igb->dev, E1000_STATUS);

	/* Let the queue drain any in-flight DMA before reprogramming */
	usleep(10000);

	vfio_dev_reg_write(&igb->dev, E1000_TDBAL(q),
		  (uint32_t)(ring_iova & 0xffffffffULL));
	vfio_dev_reg_write(&igb->dev, E1000_TDBAH(q),
		  (uint32_t)(ring_iova >> 32));
	vfio_dev_reg_write(&igb->dev, E1000_TDLEN(q), ring_size);
	vfio_dev_reg_write(&igb->dev, E1000_TDH(q), 0);
	vfio_dev_reg_write(&igb->dev, E1000_TDT(q), 0);

	vfio_dev_reg_read(&igb->dev, E1000_STATUS);

	txdctl = vfio_dev_reg_read(&igb->dev, E1000_TXDCTL(q));
	txdctl |= E1000_TXDCTL_PTHRESH(IGB_TX_PTHRESH) |
		  E1000_TXDCTL_HTHRESH(IGB_TX_HTHRESH) |
		  E1000_TXDCTL_WTHRESH(IGB_TX_WTHRESH) |
		  E1000_TXDCTL_QUEUE_ENABLE;

	vfio_dev_reg_write(&igb->dev, E1000_TXDCTL(q), txdctl);
	vfio_dev_reg_read(&igb->dev, E1000_STATUS);

	txdctl = vfio_dev_reg_read(&igb->dev, E1000_TXDCTL(q));

	if (!(txdctl & E1000_TXDCTL_QUEUE_ENABLE)) {
		fprintf(stderr, "%s: TXDCTL=%08x\n", __func__, txdctl);
		return -1;
	}

	printf("%s igb: TX queue %u enabled TXDCTL=%08x (%u descriptors)\n",
	       igb->dev.name, q, txdctl, TX_DESC_COUNT);

	if (verbose)
		igb_dump_tx_queue(igb, q);

	return 0;
}

int igb_init_tx(struct igb *igb, unsigned int q)
{
	uint32_t ctrl, tctl;

	assert(q == 0);

	tctl = vfio_dev_reg_read(&igb->dev, E1000_TCTL);

	tctl &= ~E1000_TCTL_CT;
	tctl &= ~E1000_TCTL_COLD;

	tctl |= E1000_TCTL_PSP;
	tctl |= E1000_TCTL_RTLC;
	tctl |= E1000_COLLISION_THRESHOLD << E1000_CT_SHIFT;
	tctl |= E1000_COLLISION_DISTANCE << E1000_COLD_SHIFT;

	tctl |= E1000_TCTL_EN;

	vfio_dev_reg_write(&igb->dev, E1000_TCTL, tctl);
	vfio_dev_reg_read(&igb->dev, E1000_STATUS);

	ctrl = vfio_dev_reg_read(&igb->dev, E1000_CTRL);
	ctrl |= E1000_CTRL_FD;
	vfio_dev_reg_write(&igb->dev, E1000_CTRL, ctrl);

	printf("%s igb: MAC initialized TCTL=%08x\n", igb->dev.name, tctl);

	igb_clear_tx_stats(igb);

	if (igb_setup_tx(igb, q))
		return -1;

	if (verbose)
		igb_dump_regs(igb, q);

	return 0;
}

static int igb_transmit(struct igb *igb, unsigned int q,
		 unsigned int timeout_ms)
{
	unsigned int idx = igb->tx_idx;
	unsigned int next = (idx + 1) % TX_DESC_COUNT;
	unsigned int elapsed;
	uint32_t status;

	if (!(vfio_dev_reg_read(&igb->dev, E1000_STATUS) & E1000_STATUS_LU)) {
		fprintf(stderr, "%s: link is down\n", __func__);
		return -1;
	}

	__sync_synchronize();

	vfio_dev_reg_write(&igb->dev, E1000_TDT(q), next);
	vfio_dev_reg_read(&igb->dev, E1000_STATUS);

	for (elapsed = 0; elapsed < timeout_ms; elapsed++) {
		status = igb->dma->tx_ring[idx].olinfo_status;

		if (verbose && elapsed % 100 == 0)
			printf("waiting: tx_idx=%u olinfo_status=%08x\n",
			       idx, status);

		if (status & E1000_TXD_STAT_DD)
			break;

		usleep(1000);
	}

	status = igb->dma->tx_ring[idx].olinfo_status;

	if (!(status & E1000_TXD_STAT_DD)) {
		fprintf(stderr, "%s: TX completion timeout\n", __func__);
		igb_dump_tx_queue(igb, q);
		igb_dump_tx_descriptor(&igb->dma->tx_ring[idx]);
		return -1;
	}

	igb->tx_idx = next;

	printf("%s igb: TX complete tx_idx=%u status=%08x\n",
	       igb->dev.name, idx, status);
	return 0;
}

static void igb_prepare_tx_descriptor(struct igb *igb, unsigned int q,
			       uint64_t iova, size_t len)
{
	struct igb_tx_desc *d = &igb->dma->tx_ring[igb->tx_idx];

	memset(d, 0, sizeof(*d));

	d->buffer_addr = iova;

	d->cmd_type_len =
		(uint32_t)len |
		E1000_TXD_DTYP_DATA |
		E1000_TXD_DCMD_DEXT |
		E1000_TXD_DCMD_IFCS |
		E1000_TXD_DCMD_RS |
		E1000_TXD_DCMD_EOP;
	d->olinfo_status = (uint32_t)len << E1000_TXD_PAYLEN_SHIFT;

	if (verbose)
		igb_dump_tx_descriptor(d);
}

/* caller is responsible for ensuring len fits the buffer at iova */
int igb_send_iova(struct igb *igb, unsigned int q,
		  uint64_t iova, size_t len, unsigned int timeout_ms)
{
	assert(q == 0);

	igb_prepare_tx_descriptor(igb, q, iova, len);

	return igb_transmit(igb, q, timeout_ms);
}

int igb_send_frame(struct igb *igb, unsigned int q,
		   const void *frame, size_t len, unsigned int timeout_ms)
{
	uint8_t *buf = igb->dma->tx_buf[igb->tx_idx];
	uint64_t iova = vfio_dev_to_iova(&igb->dev, buf);

	if (len < ETH_FRAME_MIN)
		len = ETH_FRAME_MIN;
	if (len > TX_BUF_SIZE)
		return -1;

	memcpy(buf, frame, len);

	return igb_send_iova(igb, q, iova, len, timeout_ms);
}

/* ---- RX ---- */

static void igb_clear_rx_stats(struct igb *igb)
{
	vfio_dev_reg_read(&igb->dev, E1000_GPRC);
	vfio_dev_reg_read(&igb->dev, E1000_GORCL);
	vfio_dev_reg_read(&igb->dev, E1000_GORCH);
}

void igb_check_rx_stats(struct igb *igb)
{
	uint32_t gprc, gorcl, gorch;
	uint64_t gorc;

	gprc = vfio_dev_reg_read(&igb->dev, E1000_GPRC);
	gorcl = vfio_dev_reg_read(&igb->dev, E1000_GORCL);
	gorch = vfio_dev_reg_read(&igb->dev, E1000_GORCH);
	gorc = ((uint64_t)gorch << 32) | gorcl;

	printf("%s igb: RX stats GPRC=%u GORC=%" PRIu64 "\n", igb->dev.name, gprc, gorc);

	if (gprc == 0)
		fprintf(stderr, "%s: no good packets received\n", __func__);
}

static int igb_setup_rx(struct igb *igb, unsigned int q)
{
	size_t ring_size = sizeof(igb->dma->rx_ring);
	uint64_t ring_iova = vfio_dev_to_iova(&igb->dev, igb->dma->rx_ring);
	uint32_t rxdctl;
	unsigned int i;

	memset(igb->dma->rx_ring, 0, ring_size);
	memset(igb->dma->rx_buf, 0, sizeof(igb->dma->rx_buf));

	for (i = 0; i < RX_DESC_COUNT; i++) {
		igb->dma->rx_ring[i].read.pkt_addr =
			vfio_dev_to_iova(&igb->dev, igb->dma->rx_buf[i]);
		igb->dma->rx_ring[i].read.hdr_addr = 0;
	}

	vfio_dev_reg_write(&igb->dev, E1000_RXDCTL(q), 0);
	vfio_dev_reg_read(&igb->dev, E1000_STATUS);

	/* Let the queue drain any in-flight DMA before reprogramming */
	usleep(10000);

	vfio_dev_reg_write(&igb->dev, E1000_RDBAL(q),
		  (uint32_t)(ring_iova & 0xffffffffULL));
	vfio_dev_reg_write(&igb->dev, E1000_RDBAH(q),
		  (uint32_t)(ring_iova >> 32));
	vfio_dev_reg_write(&igb->dev, E1000_RDLEN(q), ring_size);
	vfio_dev_reg_write(&igb->dev, E1000_RDH(q), 0);
	vfio_dev_reg_write(&igb->dev, E1000_RDT(q), 0);

	vfio_dev_reg_read(&igb->dev, E1000_STATUS);

	rxdctl = vfio_dev_reg_read(&igb->dev, E1000_RXDCTL(q));
	rxdctl |= E1000_RXDCTL_QUEUE_ENABLE;
	vfio_dev_reg_write(&igb->dev, E1000_RXDCTL(q), rxdctl);
	vfio_dev_reg_read(&igb->dev, E1000_STATUS);

	rxdctl = vfio_dev_reg_read(&igb->dev, E1000_RXDCTL(q));

	if (!(rxdctl & E1000_RXDCTL_QUEUE_ENABLE)) {
		fprintf(stderr, "%s: RXDCTL=%08x\n", __func__, rxdctl);
		return -1;
	}

	vfio_dev_reg_write(&igb->dev, E1000_RDT(q), RX_DESC_COUNT - 1);
	vfio_dev_reg_read(&igb->dev, E1000_STATUS);

	printf("%s igb: RX queue %u enabled RXDCTL=%08x (%u descriptors)\n",
	       igb->dev.name, q, rxdctl, RX_DESC_COUNT);

	if (verbose)
		igb_dump_rx_queue(igb, q);

	return 0;
}

int igb_init_rx(struct igb *igb, unsigned int q)
{
	uint32_t rctl, srrctl, vmolr;
	unsigned int pool = q & 0x7;

	assert(q == 0);

	srrctl = (RX_BUF_SIZE >> 10) | E1000_SRRCTL_DESCTYPE_ONEBUF;
	vfio_dev_reg_write(&igb->dev, E1000_SRRCTL(q), srrctl);
	vfio_dev_reg_read(&igb->dev, E1000_STATUS);

	rctl = vfio_dev_reg_read(&igb->dev, E1000_RCTL);
	rctl |= E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC;
	vfio_dev_reg_write(&igb->dev, E1000_RCTL, rctl);
	vfio_dev_reg_read(&igb->dev, E1000_STATUS);

	/*
	 * 82576+ per-pool RX filter: RCTL.BAM alone is not enough on
	 * i350, VMOLR.BAM must also be set. AUPE accepts untagged
	 * (non-VLAN) frames which the i350 drops by default.  The
	 * 82576 resets VMOLR with these bits already set, so this is
	 * only strictly needed on i350+, but harmless on 82576.
	 */
	vmolr = vfio_dev_reg_read(&igb->dev, E1000_VMOLR(pool));
	vmolr |= E1000_VMOLR_BAM | E1000_VMOLR_AUPE;
	vfio_dev_reg_write(&igb->dev, E1000_VMOLR(pool), vmolr);

	printf("%s igb: RX initialized RCTL=%08x SRRCTL=%08x VMOLR=%08x\n",
	       igb->dev.name, rctl, srrctl, vmolr);

	igb_clear_rx_stats(igb);

	if (igb_setup_rx(igb, q))
		return -1;

	if (verbose)
		igb_dump_regs(igb, q);

	return 0;
}

void igb_set_loopback(struct igb *igb)
{
	uint32_t rctl;

	rctl = vfio_dev_reg_read(&igb->dev, E1000_RCTL);
	rctl |= E1000_RCTL_LBM_MAC;
	vfio_dev_reg_write(&igb->dev, E1000_RCTL, rctl);

	printf("%s igb: MAC loopback enabled RCTL=%08x\n",
	       igb->dev.name, rctl);
}

static union igb_rx_desc *igb_rx_next(struct igb *igb, unsigned int q)
{
	union igb_rx_desc *d = &igb->dma->rx_ring[igb->rx_idx];

	__sync_synchronize();

	if (!(d->wb.status_error & E1000_RXD_STAT_DD))
		return NULL;

	return d;
}

static void igb_rx_recycle(struct igb *igb, unsigned int q, unsigned int idx)
{
	union igb_rx_desc *d = &igb->dma->rx_ring[idx];

	d->read.pkt_addr = vfio_dev_to_iova(&igb->dev, igb->dma->rx_buf[idx]);
	d->read.hdr_addr = 0;
	__sync_synchronize();

	vfio_dev_reg_write(&igb->dev, E1000_RDT(q), idx);
}

static int igb_msix_wait(struct igb *igb, unsigned long deadline)
{
	unsigned long now = now_nsec();
	int remaining_ms;

	if (now >= deadline)
		return -1;

	remaining_ms = (int)((deadline - now) / NSEC_PER_MSEC);
	if (remaining_ms <= 0)
		return -1;

	igb_irq_enable(igb, 0);
	eventfd_check(igb->dev.msix_fd, remaining_ms);
	return 0;
}

int igb_rx_poll(struct igb *igb, unsigned int q, unsigned int timeout_ms,
		igb_rx_handler_t handler, void *ctx)
{
	unsigned long deadline = now_nsec() + (unsigned long)timeout_ms * NSEC_PER_MSEC;

	assert(q == 0);

	do {
		union igb_rx_desc *d;

		while ((d = igb_rx_next(igb, q)) != NULL) {
			uint8_t *buf = igb->dma->rx_buf[igb->rx_idx];
			uint16_t length = d->wb.length;
			int ret = handler(igb, igb->rx_idx, buf, length, ctx);

			igb_rx_recycle(igb, q, igb->rx_idx);
			igb->rx_idx = (igb->rx_idx + 1) % RX_DESC_COUNT;

			if (ret == 0)
				return 0;
		}

		if (igb->dev.msix_fd >= 0) {
			if (igb_msix_wait(igb, deadline))
				break;
		} else {
			usleep(10);
		}
	} while (now_nsec() < deadline);

	return -1;
}

/* ---- MSI-X ---- */

int igb_setup_msix(struct igb *igb, unsigned int vector)
{
	uint32_t mask = 1 << vector;

	/* MSI-X mode with auto-mask on interrupt delivery */
	vfio_dev_reg_write(&igb->dev, E1000_GPIE,
			   E1000_GPIE_MSIX_MODE | E1000_GPIE_EIAME);

	/* Auto-clear EICR and auto-mask EIMS on MSI-X assertion */
	vfio_dev_reg_write(&igb->dev, E1000_EIAC, mask);
	vfio_dev_reg_write(&igb->dev, E1000_EIAM, mask);

	/* Map RX queue 0 to MSI-X vector, set valid bit */
	vfio_dev_reg_write(&igb->dev, E1000_IVAR0,
			   (vector & 0x7f) | E1000_IVAR_VALID);

	/* Enable interrupts */
	vfio_dev_reg_write(&igb->dev, E1000_EIMS, mask);

	printf("%s igb: MSI-X vector %u configured (GPIE=%08x)\n",
	       igb->dev.name, vector,
	       vfio_dev_reg_read(&igb->dev, E1000_GPIE));

	return 0;
}

void igb_irq_enable(struct igb *igb, unsigned int vector)
{
	vfio_dev_reg_write(&igb->dev, E1000_EIMS, 1 << vector);
}
