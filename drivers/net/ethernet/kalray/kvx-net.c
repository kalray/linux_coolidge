// SPDX-License-Identifier: GPL-2.0
/**
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2017-2023 Kalray Inc.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/ethtool.h>
#include <linux/etherdevice.h>
#include <linux/nvmem-consumer.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/iommu.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/of_dma.h>
#include <linux/of_platform.h>
#include <net/checksum.h>
#include <linux/ti-retimer.h>
#include <linux/rtnetlink.h>
#include <linux/hash.h>
#include <linux/firmware.h>

#include "kvx-net.h"
#include "kvx-net-hw.h"
#include "v1/kvx-net-regs.h"
#include "v2/kvx-ethrx-regs-cv2.h"
#include "v2/kvx-phy-hw-cv2.h"
#include "kvx-net-hdr.h"
#include "kvx-mac-regs.h"
#include "kvx-qsfp.h"

#define KVX_PHY_FW_NAME_CV1 "dwc_phy_cv1.bin"
#define KVX_PHY_FW_NAME_CV2 "dwc_phy_cv2.bin"

#define KVX_RX_HEADROOM  (NET_IP_ALIGN + NET_SKB_PAD)
#define KVX_SKB_PAD      (SKB_DATA_ALIGN(sizeof(struct skb_shared_info) + \
					KVX_RX_HEADROOM))
#define KVX_SKB_SIZE(len)       (SKB_DATA_ALIGN(len) + KVX_SKB_PAD)
#define KVX_MAX_RX_BUF_SIZE     (PAGE_SIZE - KVX_SKB_PAD)

/* Min/max constraints on last segment for skbuff data */
#define KVX_MIN_LAST_SEG_SIZE     32
#define KVX_MAX_LAST_SEG_SIZE     220
/* Max segment size sent to DMA */
#define KVX_SEG_SIZE              1024
/* **Sensitive** delay between link cfg done and actual link up status from HW */
#define LINK_POLL_DELAY_IN_MS     1000
#define POST_LINK_UP_DELAY_IN_MS  3000

#define KVX_TEST_BIT(name, bitmap)  test_bit(ETHTOOL_LINK_MODE_ ## name ## _BIT, bitmap)

static bool load_phy_fw = true;
module_param(load_phy_fw, bool, 0);
MODULE_PARM_DESC(load_phy_fw, "Update PHY firmware ("KVX_PHY_FW_NAME_CV1", "KVX_PHY_FW_NAME_CV2")");

static bool autoneg_en = true;
module_param(autoneg_en, bool, 0);
MODULE_PARM_DESC(autoneg_en, "Enable auto-negotiation");

/* Device tree related entries */
static const char *rtm_prop_name[RTM_NB] = {
	[RTM_RX] = "kalray,rtmrx",
	[RTM_TX] = "kalray,rtmtx",
};

static const char *rtm_channels_prop_name[RTM_NB] = {
	[RTM_RX] = "kalray,rtmrx-channels",
	[RTM_TX] = "kalray,rtmtx-channels",
};

static int rx_jobq_prio[] = {
	[DDR_POOL] = 1,
};

static void kvx_eth_alloc_rx_buffers(struct kvx_eth_ring *ring, int count);
static int kvx_eth_phy_fw_update(struct platform_device *pdev,  const char *filename);

enum kvx_eth_speed_units_idx {
	KVX_ETH_UNITS_GBPS,
	KVX_ETH_UNITS_MBPS,
	KVX_ETH_UNITS_NB,
};

char *kvx_eth_speed_units[KVX_ETH_UNITS_NB] = {
	[KVX_ETH_UNITS_GBPS] = "Gbps",
	[KVX_ETH_UNITS_MBPS] = "Mbps",
};

static const char *kvx_eth_res_names_cv1[KVX_ETH_NUM_RES_CV1] = {
	"phy", "phymac", "mac", "eth"};

static const char *kvx_eth_res_names_cv2[KVX_ETH_NUM_RES_CV2] = {
	"phy", "phyctl", "mac", "ethrx", "ethrx_lb_analyzer", "ethrx_lb_deliver", "ethrx_lb_rfs", "ethtx"};

static const struct kvx_eth_type kvx_haps_data = {
	.phy_init = kvx_eth_haps_phy_init,
	.mac_link_status_supported = false,
	.support_1000baseT_only = true,
	.phy_lane_rx_serdes_data_en_supported = false
};

static struct kvx_eth_type kvx_eth_data = {
	.phy_init = kvx_eth_phy_init,
	.phy_cfg = kvx_eth_phy_cfg,
	.phy_fw_update = kvx_eth_phy_fw_update,
	.phy_lane_rx_serdes_data_en_supported = true,
	.phy_rx_adaptation = kvx_eth_phy_rx_adaptation,
	.mac_link_status_supported = true,
	.support_1000baseT_only = false
};

/* kvx_eth_get_formated_speed() - Convert int speed to a displayable format
 * @speed: the speed to parse in mbps
 * @speed_fmt: formatted speed value
 * @unit: matching unit string
 */

void kvx_eth_get_formated_speed(int speed, int *speed_fmt,
		char **unit)
{
	if (speed > 1000) {
		*speed_fmt = speed / 1000;
		*unit = kvx_eth_speed_units[KVX_ETH_UNITS_GBPS];
	} else {
		*speed_fmt = speed;
		*unit = kvx_eth_speed_units[KVX_ETH_UNITS_MBPS];
	}
}

static const char *pause_to_str(int pause)
{
	switch (pause & MLO_PAUSE_TXRX_MASK) {
	case MLO_PAUSE_TX | MLO_PAUSE_RX:
		return "rx/tx";
	case MLO_PAUSE_TX:
		return "tx";
	case MLO_PAUSE_RX:
		return "rx";
	default:
		return "off";
	}
}

static void RING_INC(struct kvx_eth_ring *r, unsigned int *v)
{
	/* Barrier for concurrent accesses */
	unsigned int val = smp_load_acquire(v);
	unsigned int ret, new, max = r->count - 1;

	do {
		new = ((val == max) ? 0 : val + 1);
		ret = arch_cmpxchg(v, val, new);
	} while (ret != val);
}

/* kvx_eth_desc_unused() - Gets the number of remaining unused buffers in ring
 * @r: Current ring
 *
 * Return: number of usable buffers
 */
int kvx_eth_desc_unused(struct kvx_eth_ring *r)
{
	/* Potential concurrent access (completion handler vs rx buffer refill) */
	unsigned int clean = smp_load_acquire(&r->next_to_clean);
	unsigned int use = READ_ONCE(r->next_to_use);

	if (clean > use)
		return clean - use - 1;
	return (r->count - (use - clean + 1));
}

static void kvx_eth_reset_ring(struct kvx_eth_ring *r)
{
	WRITE_ONCE(r->next_to_use, 0);
	WRITE_ONCE(r->next_to_clean, 0);
}

static struct netdev_queue *get_txq(const struct kvx_eth_ring *ring)
{
	return netdev_get_tx_queue(ring->netdev, ring->qidx);
}

static void kvx_eth_reset_tx(struct kvx_eth_netdev *ndev)
{
	struct kvx_eth_ring *txr;
	unsigned long t;
	int qidx;

	for (qidx = 0; qidx < ndev->dma_cfg.tx_chan_id.nb; qidx++) {
		txr = &ndev->tx_ring[qidx];
		t = jiffies + msecs_to_jiffies(10);
		/* Wait for pending descriptors */
		while (time_before(jiffies, t)) {
			if (kvx_eth_desc_unused(txr) == txr->count - 1)
				break;
			usleep_range(10, 20);
		}
		netif_tx_stop_queue(get_txq(txr));
		kvx_eth_reset_ring(txr);
	}
}

static bool kvx_eth_rtm_cdr_lock(struct kvx_eth_netdev *ndev)
{
	struct kvx_eth_hw *hw = ndev->hw;
	int i, nb_lanes = kvx_eth_speed_to_nb_lanes(ndev->cfg.speed, NULL);
	u8 lane;

	for (i = ndev->cfg.id; i < nb_lanes; i++) {
		lane = (u8)hw->rtm_params->channels[i];

		if (!ti_retimer_get_cdr_lock(hw->rtm_params[RTM_RX].rtm, BIT(lane)))
			return false;
	}

	return true;
}

static void kvx_eth_rtm_tx_enable(struct kvx_eth_netdev *ndev)
{
	struct kvx_eth_hw *hw = ndev->hw;
	int i, nb_lanes = kvx_eth_speed_to_nb_lanes(ndev->cfg.speed, NULL);
	u8 lane;

	for (i = ndev->cfg.id; i < ndev->cfg.id + nb_lanes; i++) {
		lane = (u8)hw->rtm_params[RTM_TX].channels[i];
		ti_retimer_tx_enable(hw->rtm_params[RTM_TX].rtm, BIT(lane));
	}
}

static void kvx_eth_rtm_tx_disable(struct kvx_eth_netdev *ndev)
{
	struct kvx_eth_hw *hw = ndev->hw;
	int i;
	u8 lane;

	for (i = ndev->cfg.id; i < KVX_ETH_LANE_NB; i++) {
		lane = (u8)hw->rtm_params[RTM_TX].channels[i];
		ti_retimer_tx_disable(hw->rtm_params[RTM_TX].rtm, BIT(lane));
	}
}

static void kvx_eth_update_carrier(struct kvx_eth_netdev *ndev, bool en)
{
	if (en) {
		netif_carrier_on(ndev->netdev);
		netdev_info(ndev->netdev, "Link is Up - %s/%s - flow control %s\n",
			    phy_speed_to_str(ndev->cfg.speed),
			    phy_duplex_to_str(ndev->cfg.duplex),
			    pause_to_str(ndev->hw->lb_f[ndev->cfg.id].pfc_f.global_pause_en));
	} else {
		netif_carrier_off(ndev->netdev);
		netdev_info(ndev->netdev, "Link is down\n");
	}
}

void kvx_eth_setup_link(struct kvx_eth_netdev *ndev, bool restart_serdes)
{
	/* if ndev->cfg.restart_serdes is true, avoid setting it to false.
	 * This way, we avoid the following scenario:
	 *     kvx_eth_setup_link(restart_serdes=true)
	 *       ndev->cfg.restart_serdes = true
	 *       queue_work(link_cfg)
	 *     kvx_eth_setup_link(restart_serdes=false)
	 *       ndev->cfg.restart_serdes = false
	 *     kvx_eth_link_cfg()
	 *       kvx_eth_link_configure(restart_serdes=false)  <-- we need true
	 * Note: the reverse "false then true" is ok.
	 */
	if (!ndev->cfg.restart_serdes)
		ndev->cfg.restart_serdes = restart_serdes;

	/* if work is pending or running, no need to queue it  */
	if (atomic_read(&ndev->link_cfg_running) || work_pending(&ndev->link_cfg))
		return;

	queue_work(system_power_efficient_wq, &ndev->link_cfg);
}

/* kvx_eth_lane_mask() - provide mask of the lanes hold by the given netdev
 * @ndev: netdev
 *
 * Return: lane bitmask (1:hold, 0:not hold)
 */
static u32 kvx_eth_lane_mask(struct kvx_eth_netdev *ndev)
{
	int lane_nb = kvx_eth_speed_to_nb_lanes(ndev->cfg.speed, NULL);
	u32 msk = GENMASK(ndev->cfg.id + lane_nb - 1, ndev->cfg.id);

	return msk;
}

static int kvx_eth_link_configure(struct kvx_eth_netdev *ndev)
{
	struct kvx_eth_hw *hw = ndev->hw;
	bool link;
	const struct kvx_eth_chip_rev_data *rev_d = kvx_eth_get_rev_data(hw);
	enum coolidge_rev chip_rev =  rev_d->revision;
	struct kvx_eth_dev *dev = KVX_HW2DEV(hw);
	u32 msk;
	unsigned long flags;

	netdev_dbg(ndev->netdev, "%s speed: %d autoneg: %d\n", __func__,
		   ndev->cfg.speed, ndev->cfg.autoneg_en);

	if (rev_d->phy_rx_adapt == NULL) {
		/* as rx_adapt not supported: autoneg not supported */
		ndev->cfg.autoneg_en = false;
	}

	if (dev->type->support_1000baseT_only == true) {
		ndev->cfg.autoneg_en = false;
		ndev->cfg.speed = SPEED_1000;
		ndev->cfg.duplex = DUPLEX_FULL;
	}

	/* Enable the TX retimer when bringing interface up */
	if (hw->rtm_params[RTM_TX].rtm)
		kvx_eth_rtm_tx_enable(ndev);

	if (ndev->cfg.autoneg_en && hw->rxtx_crossed) {
		netdev_err(ndev->netdev, "Autonegotiation is not supported with inverted lanes\n");
		return -EOPNOTSUPP;
	}

	if (ndev->cfg.speed == SPEED_UNKNOWN)
		ndev->cfg.speed = SPEED_100000;
	if (ndev->cfg.duplex == DUPLEX_UNKNOWN)
		ndev->cfg.duplex = DUPLEX_FULL;

	if (!ndev->cfg.mac_f.loopback_mode) {
		/* configure speed and set the link up - do the autoneg/link training if enabled */
		if (kvx_eth_mac_setup_link(ndev->hw, &ndev->cfg))
			return -EAGAIN; /* kvx_eth_link_cfg() will retry link config */
	}

	if (chip_rev == COOLIDGE_V2)
		kvx_eth_tx_cfg_speed_settings(hw, &ndev->cfg);

	/* avoid false detection: check mac link over a period of 10ms */
	read_poll_timeout(kvx_eth_mac_getlink, link, link == true, 1000, 10000, false,
			  hw, &ndev->cfg);
	if (!link)
		return -ENOLINK;

	netif_tx_start_all_queues(ndev->netdev);
	kvx_eth_update_carrier(ndev, true);

	/* link is up: start polling link status after 3s */
	if (ndev->link_poll_en)
		queue_delayed_work(system_power_efficient_wq, &ndev->link_poll,
				   msecs_to_jiffies(POST_LINK_UP_DELAY_IN_MS));
	if (dev->chip_rev_data->lnk_dwn_it_support) {
		/* enable downlink ITs */
		msk = kvx_eth_lane_mask(ndev);
		spin_lock_irqsave(&hw->link_down_lock, flags);
		updatel_bits(hw, MAC, MAC_LINK_DOWN_IT_EN_OFFSET, msk, msk)
		spin_unlock_irqrestore(&hw->link_down_lock, flags);
	}

	return 0;
}

void kvx_net_cancel_link_cfg(struct kvx_eth_netdev *ndev)
{
	atomic_set(&ndev->link_cfg_running, 0);
	cancel_work_sync(&ndev->link_cfg);
}

static void kvx_eth_link_cfg(struct work_struct *w)
{
	struct kvx_eth_netdev *ndev = container_of(w, struct kvx_eth_netdev, link_cfg);
	const struct kvx_eth_chip_rev_data *rev_d = kvx_eth_get_rev_data_of_netdev(ndev->netdev);

	atomic_set(&ndev->link_cfg_running, 1);

	/* if no cable, qsfp connect callback will restart link cfg */
	if (ndev->qsfp && !is_cable_connected(ndev->qsfp))
		goto bail;

	/* make sure carrier is off before starting link config  */
	if (netif_carrier_ok(ndev->netdev)) {
		kvx_eth_reset_tx(ndev);
		netif_tx_stop_all_queues(ndev->netdev);
		if (ndev->hw->rtm_params[RTM_TX].rtm)
			kvx_eth_rtm_tx_disable(ndev);
		kvx_eth_update_carrier(ndev, false);
	}

	/* keep looping while link config is not successful
	 * however, link_cfg_running can be set to 0 remotely to stop
	 * the work, followed by cancel_work_sync()
	 */
	while (atomic_read(&ndev->link_cfg_running)) {
		if (!kvx_eth_link_configure(ndev))
			break;

		msleep(LINK_POLL_DELAY_IN_MS);
		if (rev_d->phy_rx_adapt == NULL) {
			/* restart from scratch when RxAdapt not fonctionnal */
			ndev->cfg.restart_serdes = true;
		}
	}
bail:
	ndev->cfg.restart_serdes = false;
	atomic_set(&ndev->link_cfg_running, 0); /* in case of break */
}

static void kvx_eth_poll_link(struct work_struct *w)
{
	struct kvx_eth_netdev *ndev = container_of(w, struct kvx_eth_netdev, link_poll.work);

	/* sanity check: no link status polling while link is down  */
	if (!netif_carrier_ok(ndev->netdev))
		goto link_cfg;

	if (kvx_eth_phy_is_bert_en(ndev->hw))
		goto bail;

	if (ndev->hw->phy_f.loopback_mode == PHY_PMA_LOOPBACK) {
		netdev_warn(ndev->netdev, "%s PHY loopback is enabled\n", __func__);
		goto bail;
	}

	if (kvx_eth_mac_getlink(ndev->hw, &ndev->cfg)) {
		if (!ndev->hw->rtm_params[RTM_RX].rtm)
			goto bail; /* no retimer, skip CDR lock check */

		if (kvx_eth_rtm_cdr_lock(ndev))
			goto bail;
	}

link_cfg:
	kvx_eth_setup_link(ndev, false);
	return;
bail:
	if (ndev->link_poll_en)
		queue_delayed_work(system_power_efficient_wq, &ndev->link_poll,
				   msecs_to_jiffies(LINK_POLL_DELAY_IN_MS));
}

/* kvx_eth_netdev_init() - Init netdev (called once)
 * @netdev: Current netdev
 */
static int kvx_eth_netdev_init(struct net_device *netdev)
{
	netif_carrier_off(netdev);
	return 0;
}

/* kvx_eth_netdev_uninit() - Stop all netdev queues
 * @netdev: Current netdev
 */
static void kvx_eth_netdev_uninit(struct net_device *netdev)
{
}

/* kvx_eth_up() - Interface up
 * @netdev: Current netdev
 */
void kvx_eth_up(struct net_device *netdev)
{
	struct kvx_eth_netdev *ndev = netdev_priv(netdev);

	kvx_eth_setup_link(ndev, true);
}

/* kvx_eth_netdev_open() - Open ops
 * @netdev: Current netdev
 */
static int kvx_eth_netdev_open(struct net_device *netdev)
{
	struct kvx_eth_netdev *ndev = netdev_priv(netdev);
	struct kvx_eth_ring *r;
	int i;

	for (i = 0; i < NB_RX_RING; i++) {
		r = &ndev->rx_ring[i];
		kvx_eth_alloc_rx_buffers(r, kvx_eth_desc_unused(r));
		napi_enable(&r->napi);
	}

	if (!netif_carrier_ok(netdev))
		kvx_eth_up(netdev);

	return 0;
}

/* kvx_eth_down() - Interface down
 * @netdev: Current netdev
 */
void kvx_eth_down(struct net_device *netdev)
{
	struct kvx_eth_netdev *ndev = netdev_priv(netdev);
	struct kvx_eth_hw *hw = ndev->hw;
	struct kvx_eth_dev *dev = KVX_HW2DEV(hw);
	u32 msk;
	unsigned long flags;

	if (dev->chip_rev_data->lnk_dwn_it_support) {
		msk = kvx_eth_lane_mask(ndev);
		spin_lock_irqsave(&hw->link_down_lock, flags);
		updatel_bits(hw, MAC, MAC_LINK_DOWN_IT_EN_OFFSET, msk, 0)
		spin_unlock_irqrestore(&hw->link_down_lock, flags);
	}

	cancel_delayed_work(&ndev->link_poll);
	kvx_net_cancel_link_cfg(ndev);

	kvx_eth_reset_tx(ndev);

	/* Disable the TX retimer when bringing port down */
	if (hw->rtm_params[RTM_TX].rtm)
		kvx_eth_rtm_tx_disable(ndev);

	netif_tx_stop_all_queues(ndev->netdev);
	kvx_eth_update_carrier(ndev, false);
}

/* kvx_eth_netdev_stop() - Stop all netdev queues
 * @netdev: Current netdev
 */
static int kvx_eth_netdev_stop(struct net_device *netdev)
{
	struct kvx_eth_netdev *ndev = netdev_priv(netdev);
	int qidx;

	for (qidx = 0; qidx < ndev->dma_cfg.rx_chan_id.nb; qidx++)
		napi_disable(&ndev->rx_ring[qidx].napi);

	kvx_eth_down(netdev);

	return 0;
}

static void kvx_eth_init_netdev_hdw_cv1(struct kvx_eth_netdev *ndev)
{
	kvx_eth_dt_f_init(ndev->hw, &ndev->cfg);
	kvx_eth_lb_f_init(ndev->hw, &ndev->cfg);
	kvx_eth_pfc_f_init(ndev->hw, &ndev->cfg);
	kvx_eth_parser_f_init(ndev->hw, &ndev->cfg);
	kvx_net_init_dcb(ndev->netdev);
}
static void kvx_eth_init_netdev_hdw_cv2(struct kvx_eth_netdev *ndev)
{
	kvx_eth_lb_cv2_f_init(ndev->hw, &ndev->cfg);
	kvx_eth_parser_cv2_f_init(ndev->hw, &ndev->cfg);
	kvx_eth_tx_f_init(ndev->hw);
}
/* kvx_eth_init_netdev() - Init netdev generic settings
 * @ndev: Current kvx_eth_netdev
 *
 * Return: 0 - OK
 */
static int kvx_eth_init_netdev(struct kvx_eth_netdev *ndev)
{
	struct kvx_eth_hw *hw = ndev->hw;
	const struct kvx_eth_chip_rev_data *rev_d = kvx_eth_get_rev_data(hw);

	ndev->hw->max_frame_size = ndev->netdev->mtu +
		(2 * KVX_ETH_HEADER_SIZE);
	/* Takes into account alignement offsets (footers) */
	ndev->rx_buffer_len = ALIGN(hw->max_frame_size,
				    KVX_ETH_PKT_ALIGN);

	ndev->cfg.autoneg_en = autoneg_en;
	ndev->cfg.speed = SPEED_UNKNOWN;
	ndev->cfg.duplex = DUPLEX_FULL;
	ndev->cfg.fec = 0;
	kvx_eth_mac_f_init(hw, &ndev->cfg);
	rev_d->eth_init_netdev_hdw(ndev);
	return 0;
}

/* kvx_eth_unmap_skb() - Unmap skb
 * @dev: Current device (with dma settings)
 * @tx: Tx ring descriptor
 */
static void kvx_eth_unmap_skb(struct device *dev,
			      const struct kvx_eth_netdev_tx *tx)
{
	const skb_frag_t *fp, *end;
	const struct skb_shared_info *si;
	int count = 1;

	dma_unmap_single(dev, sg_dma_address(&tx->sg[0]),
			 skb_headlen(tx->skb), DMA_TO_DEVICE);

	si = skb_shinfo(tx->skb);
	if (si) {
		end = &si->frags[si->nr_frags];
		for (fp = si->frags; fp < end; fp++, count++) {
			dma_unmap_page(dev, sg_dma_address(&tx->sg[count]),
				sg_dma_len(&tx->sg[count]), DMA_TO_DEVICE);
		}
	}
}

/* kvx_eth_skb_split() - build dma segments within boundaries
 *
 * Return: number of segments actually built
 */
static int kvx_eth_skb_split(struct device *dev, struct scatterlist *sg,
			     dma_addr_t dma_addr, size_t len)
{
	u8 *buf = (u8 *)dma_addr;
	int i = 0, s, l = len;

	do {
		if (l > KVX_SEG_SIZE + KVX_MIN_LAST_SEG_SIZE)
			s = KVX_SEG_SIZE;
		else if (l > KVX_SEG_SIZE)
			s = l + KVX_MAX_LAST_SEG_SIZE - KVX_SEG_SIZE;
		else if (l > KVX_MAX_LAST_SEG_SIZE)
			s = l - KVX_MAX_LAST_SEG_SIZE + KVX_MIN_LAST_SEG_SIZE;
		else
			s = l;

		if (s < KVX_MIN_LAST_SEG_SIZE) {
			dev_err(dev, "Segment size %d < %d\n", s, KVX_MIN_LAST_SEG_SIZE);
			break;
		}
		sg_dma_address(&sg[i]) = (dma_addr_t)buf;
		sg_dma_len(&sg[i]) = s;
		l -= s;
		buf += s;
		i++;
	} while (l > 0 && i <= MAX_SKB_FRAGS);
	return i;
}

/* kvx_eth_map_skb() - Map skb (build sg with corresponding IOVA)
 * @dev: Current device (with dma settings)
 * @tx: Tx ring descriptor
 *
 * Return: 0 on success, -ENOMEM on error.
 */
static int kvx_eth_map_skb(struct device *dev, struct kvx_eth_netdev_tx *tx)
{
	const skb_frag_t *fp, *end;
	const struct skb_shared_info *si;
	dma_addr_t handler;
	int len, count;

	sg_init_table(tx->sg, MAX_SKB_FRAGS + 1);
	handler = dma_map_single(dev, tx->skb->data,
				 skb_headlen(tx->skb), DMA_TO_DEVICE);
	if (dma_mapping_error(dev, handler))
		goto out_err;

	count = kvx_eth_skb_split(dev, tx->sg, handler, skb_headlen(tx->skb));
	tx->len = skb_headlen(tx->skb);

	si = skb_shinfo(tx->skb);
	end = &si->frags[si->nr_frags];
	for (fp = si->frags; fp < end; fp++) {
		len = skb_frag_size(fp);
		handler = skb_frag_dma_map(dev, fp, 0, len, DMA_TO_DEVICE);
		if (dma_mapping_error(dev, handler))
			goto unwind;

		count += kvx_eth_skb_split(dev, &tx->sg[count], handler, len);
		if (count >= MAX_SKB_FRAGS + 1) {
			dev_warn(dev, "Too many skb segments\n");
			goto unwind;
		}
		tx->len += len;
	}
	sg_mark_end(&tx->sg[count - 1]);
	tx->sg_len = count;
	dev_dbg(dev, "%s tx->len=%d - skblen %d sg_len:%ld si->nr_frags: %d\n", __func__,
		(int)tx->len, tx->skb->len, tx->sg_len, si->nr_frags);
	return 0;

unwind:
	while (fp-- > si->frags)
		dma_unmap_page(dev, sg_dma_address(&tx->sg[--count]),
			       skb_frag_size(fp), DMA_TO_DEVICE);
	dma_unmap_single(dev, sg_dma_address(&tx->sg[0]),
			 skb_headlen(tx->skb), DMA_TO_DEVICE);
	tx->len = 0;

out_err:
	return -ENOMEM;
}

/* kvx_eth_clean_tx_irq() - Clears completed tx skb
 * @txr: Current TX ring
 *
 * Return: 0 on success
 */
static int kvx_eth_clean_tx_irq(struct kvx_eth_ring *txr)
{
	struct net_device *netdev = txr->netdev;
	struct kvx_eth_netdev *ndev = netdev_priv(netdev);
	struct kvx_eth_netdev_tx *tx = &txr->tx_buf[txr->next_to_clean];
	u64 comp = kvx_dma_get_tx_completed(ndev->dma_cfg.pdev, txr->dma_chan);
	unsigned int d;

	while (tx->job_idx + tx->sg_len <= comp) {
		if (kvx_eth_desc_unused(txr) == txr->count - 1)
			break;
		if (!tx->sg_len || !tx->skb)
			break;
		netdev_dbg(netdev, "queue[%d] sent skb[%d]: 0x%llx job_idx: %lld sg_len: %ld comp: %lld len: %ld (cnt: %d)\n",
			   txr->qidx, txr->next_to_clean, (u64)tx->skb,
			   tx->job_idx, tx->sg_len, comp, tx->len, d);

		/* consume_skb */
		kvx_eth_unmap_skb(ndev->dev, tx);
		ndev->stats.ring.tx_bytes += tx->len;
		ndev->stats.ring.tx_pkts++;
		dev_consume_skb_any(tx->skb);

		netdev_tx_completed_queue(get_txq(txr), 1, tx->len);
		memset(tx, 0, sizeof(*tx));
		RING_INC(txr, &txr->next_to_clean);

		tx = &txr->tx_buf[txr->next_to_clean];
		comp = kvx_dma_get_tx_completed(ndev->dma_cfg.pdev,
						txr->dma_chan);
	}

	if (netif_carrier_ok(netdev) &&
	    __netif_subqueue_stopped(netdev, txr->qidx))
		if (netif_carrier_ok(netdev) &&
		    (kvx_eth_desc_unused(txr) > (MAX_SKB_FRAGS + 1)))
			netif_wake_subqueue(netdev, txr->qidx);

	return 0;
}

/* kvx_eth_netdev_dma_callback_tx() - tx completion callback
 * @param: Callback dma_noc parameters
 */
static void kvx_eth_netdev_dma_callback_tx(void *param)
{
	struct kvx_eth_ring *txr = param;

	kvx_eth_clean_tx_irq(txr);
}

static u32 align_checksum(u32 cks)
{
	u32 c = cks;

	while (c > 0xffff)
		c = (c >> 16) + (c & 0xffff);
	return c;
}

/* kvx_eth_pseudo_hdr_cks() - Compute pseudo CRC on skb
 * @skb: skb to handle
 *
 * Return: computed crc
 */
static u16 kvx_eth_pseudo_hdr_cks(struct sk_buff *skb)
{
	struct ethhdr *eth_h = eth_hdr(skb);
	struct iphdr *iph = ip_hdr(skb);
	u16 payload_len = skb_tail_pointer(skb) - (unsigned char *)eth_h;
	u32 cks = eth_h->h_proto + payload_len;

	if (eth_h->h_proto == ETH_P_IP)
		cks = csum_partial((void *)&iph->saddr, 8, cks);
	else if (eth_h->h_proto == ETH_P_IPV6)
		cks = csum_partial((void *)&iph->saddr, 32, cks);

	return align_checksum(cks);
}

/* kvx_eth_tx_add_hdr() - Fill tx header for tx ring descriptor
 * @ndev: Current kvx_eth_netdev
 * @tx: Corresponding tx descriptor
 */
static void kvx_eth_fill_tx_hdr_cv1(struct kvx_eth_netdev *ndev,
			       struct kvx_eth_netdev_tx *tx)
{
	struct sk_buff *skb = tx->skb;
	int qidx = skb_get_queue_mapping(skb);
	struct kvx_eth_ring *txr = &ndev->tx_ring[qidx];
	struct ethhdr *eth_h = eth_hdr(skb);
	struct iphdr *iph = ip_hdr(skb);
	enum tx_ip_mode ip_mode = NO_IP_MODE;
	enum tx_crc_mode crc_mode = NO_CRC_MODE;
	struct kvx_eth_lane_cfg *cfg = &ndev->cfg;
	volatile union eth_tx_metadata *h =
		kvx_dma_get_eth_tx_hdr(txr->dma_chan, tx->job_idx);

	h->dword[0] = 0;
	h->dword[1] = 0;
	if (unlikely(!ndev->hw->tx_f[cfg->tx_fifo_id].header_en))
		goto bail;

	/* Packet size without tx metadata */
	h->pkt_size = tx->len;
	h->lane = cfg->id;
	h->nocx_en = ndev->hw->tx_f[cfg->tx_fifo_id].nocx_en;

	if (skb->ip_summed != CHECKSUM_PARTIAL)
		goto bail;

	if (eth_h->h_proto == htons(ETH_P_IP))
		ip_mode = IP_V4_MODE;
	else if (eth_h->h_proto == htons(ETH_P_IPV6))
		ip_mode = IP_V6_MODE;

	if (iph && ndev->hw->tx_f[cfg->tx_fifo_id].crc_en) {
		if (iph->protocol == IPPROTO_TCP)
			crc_mode = TCP_MODE;
		else if ((iph->protocol == IPPROTO_UDP) ||
			 (iph->protocol == IPPROTO_UDPLITE))
			crc_mode = UDP_MODE;
	}

	if (ip_mode && crc_mode) {
		h->ip_mode  = ip_mode;
		h->crc_mode = crc_mode;
		h->index    = (u16)skb->transport_header;
		h->udp_tcp_cksum = kvx_eth_pseudo_hdr_cks(skb);
	} else {
		skb_checksum_help(skb);
	}

bail:
	/* Expect tx hdr has been written */
	wmb();
}

/* kvx_eth_fill_tx_hdr_cv2() - need to be revisited
 * @ndev: Current kvx_eth_netdev
 * @tx: Corresponding tx descriptor
 * * Todo : to be revisited
 */
static void kvx_eth_fill_tx_hdr_cv2(struct kvx_eth_netdev *ndev,
			       struct kvx_eth_netdev_tx *tx)
{
	struct sk_buff *skb = tx->skb;
	int qidx = skb_get_queue_mapping(skb);
	struct kvx_eth_ring *txr = &ndev->tx_ring[qidx];
	struct ethhdr *eth_h = eth_hdr(skb);
	struct iphdr *iph = ip_hdr(skb);
	enum tx_ip_mode ip_mode = NO_IP_MODE;
	enum tx_crc_mode crc_mode = NO_CRC_MODE;
	/* declared as volatile to insure write access order */
	volatile union eth_tx_metadata *h =
		kvx_dma_get_eth_tx_hdr(txr->dma_chan, tx->job_idx);

	h->dword[0] = 0;
	h->dword[1] = 0;

	/* Packet size without tx metadata */
	h->pkt_size = tx->len;

	if (skb->ip_summed != CHECKSUM_PARTIAL)
		goto bail;

	//Seems to never pass here

	if (eth_h->h_proto == htons(ETH_P_IP))
		ip_mode = IP_V4_MODE;
	else if (eth_h->h_proto == htons(ETH_P_IPV6))
		ip_mode = IP_V6_MODE;

	if (iph) {
		if (iph->protocol == IPPROTO_TCP)
			crc_mode = TCP_MODE;
		else if ((iph->protocol == IPPROTO_UDP) ||
			 (iph->protocol == IPPROTO_UDPLITE))
			crc_mode = UDP_MODE;
	}

	if (ip_mode && crc_mode) {
		h->ip_mode  = ip_mode;
		h->crc_mode = crc_mode;
		h->index    = (u16)skb->transport_header;
		h->udp_tcp_cksum = kvx_eth_pseudo_hdr_cks(skb);
	} else {
		skb_checksum_help(skb);
	}

bail:
	/* Expect tx hdr has been written */
	wmb();
}

/* kvx_eth_netdev_start_xmit() - xmit ops
 * @skb: skb to handle
 * @netdev: Current netdev
 *
 * Return: transmit status
 */
static netdev_tx_t kvx_eth_netdev_start_xmit(struct sk_buff *skb,
					     struct net_device *netdev)
{
	struct kvx_eth_netdev *ndev = netdev_priv(netdev);
	struct device *dev = ndev->dev;
	int qidx = skb_get_queue_mapping(skb);
	struct kvx_eth_ring *txr = &ndev->tx_ring[qidx];
	u32 tx_w = txr->next_to_use;
	struct kvx_eth_netdev_tx *tx = &txr->tx_buf[tx_w];
	int unused_tx;
	int ret = 0;
	struct kvx_eth_hw *hw = ndev->hw;
	const struct kvx_eth_chip_rev_data *rev_d = kvx_eth_get_rev_data(hw);

	if (skb_padto(skb, ETH_ZLEN))
		return NETDEV_TX_OK;

	if (skb->len <= 0) {
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	unused_tx = kvx_eth_desc_unused(txr);
	if (unused_tx == 0) {
		netdev_warn(netdev, "Tx ring full\n");
		goto busy;
	}

	tx->skb = skb;

	/* prepare sg */
	if (kvx_eth_map_skb(dev, tx)) {
		net_err_ratelimited("tx[%d]: Map skb failed\n", tx_w);
		goto busy;
	}

	ret = kvx_dma_prepare_pkt(txr->dma_chan, tx->sg, tx->sg_len,
				 txr->param.route_id, &tx->job_idx);
	if (ret) {
		kvx_eth_unmap_skb(dev, tx);
		goto busy;
	}

	rev_d->eth_fill_tx_hdr(ndev, tx);

	netdev_dbg(netdev, "Sending skb[%d]: 0x%llx len: %d/%d qidx: %d job_idx: %lld\n",
		    tx_w, (u64)tx->skb, (int)tx->len, skb->len,
		    txr->qidx, tx->job_idx);

	netdev_tx_sent_queue(get_txq(txr), tx->len);

	skb_tx_timestamp(skb);

	kvx_dma_submit_pkt(txr->dma_chan, tx->job_idx, tx->sg_len);
	RING_INC(txr, &txr->next_to_use);

	unused_tx = kvx_eth_desc_unused(txr);
	if (unlikely(unused_tx == 0))
		netif_tx_stop_queue(get_txq(txr));

	return NETDEV_TX_OK;

busy:
	return NETDEV_TX_BUSY;
}

/* kvx_eth_alloc_rx_buffers() - Allocation rx descriptors
 * @rxr: RX ring
 * @count: number of buffers to allocate
 */
static void kvx_eth_alloc_rx_buffers(struct kvx_eth_ring *rxr, int count)
{
	struct net_device *netdev = rxr->netdev;
	struct kvx_eth_netdev *ndev = netdev_priv(netdev);
	struct kvx_dma_config *dma_cfg = &ndev->dma_cfg;
	u32 rx_w = rxr->next_to_use;
	struct kvx_qdesc *qdesc;
	struct page *p;
	int ret = 0;

	netdev_dbg(netdev, "%s: %d buf (rx_r: %d rx_w: %d\n", __func__,
		    count, rxr->next_to_clean, rxr->next_to_use);
	while (count--) {
		qdesc = &rxr->pool.qdesc[rx_w];

		if (!qdesc->dma_addr) {
			p = page_pool_alloc_pages(rxr->pool.pagepool,
						  GFP_ATOMIC | __GFP_NOWARN);
			if (!p) {
				pr_err("page alloc failed\n");
				break;
			}
			qdesc->va = p;
			qdesc->dma_addr = page_pool_get_dma_addr(p) +
				KVX_RX_HEADROOM;
		} else {
			netdev_err(netdev, "dma_addr != NULL rx chan[%d] desc id: %d\n",
				   dma_cfg->rx_chan_id.start + rxr->qidx, rx_w);
			break;
		}
		ret = kvx_dma_enqueue_rx_buffer(rxr->rx_jobq,
					  qdesc->dma_addr, KVX_MAX_RX_BUF_SIZE);
		if (ret) {
			netdev_err(netdev, "Failed to enqueue buffer in rx chan[%d]: %d\n",
				   dma_cfg->rx_chan_id.start + rxr->qidx, ret);
			break;
		}

		rx_w = ((rx_w >= rxr->count - 1) ? 0 : rx_w + 1);
		if (rx_w == rxr->next_to_clean) {
			netdev_warn(netdev, "!! %s rx_r == rx_w == %d\n", __func__, rx_w);
			break;
		}
	}
	/* Concurrent access */
	smp_store_mb(rxr->next_to_use, rx_w);
	netdev_dbg(netdev, "END %s: %d buf (rx_r: %d rx_w: %d) unused: %d\n", __func__,
		    count, rxr->next_to_clean, rxr->next_to_use, kvx_eth_desc_unused(rxr));
}

/**
 * kvx_eth_rx_hdr_cv1() - Extract hw header (assuming header is always enabled)
 */
static int kvx_eth_rx_hdr_cv1(struct kvx_eth_netdev *ndev, struct sk_buff *skb)
{
	struct rx_metadata *hdr = NULL;
	size_t hdr_size = sizeof(*hdr);

	netdev_dbg(ndev->netdev, "%s header rx (skb->len: %d data_len: %d)\n",
		   __func__, skb->len, skb->data_len);
	hdr = (struct rx_metadata *)skb->data;
	kvx_eth_dump_rx_hdr_cv1(ndev->hw, hdr);

	if (hdr->f.fcs_errors)
		ndev->stats.ring.skb_fcs_err++;

	if (hdr->f.crc_errors)
		ndev->stats.ring.skb_crc_err++;

	skb_pull(skb, hdr_size);
	skb->ip_summed = CHECKSUM_UNNECESSARY;

	return 0;
}

/**
 * kvx_eth_rx_hdr_cv2() - Extract hw header (assuming header is always enabled)
 */
static int kvx_eth_rx_hdr_cv2(struct kvx_eth_netdev *ndev, struct sk_buff *skb)
{
	struct rx_metadata_cv2 *hdr = NULL;
	size_t hdr_size = sizeof(*hdr);

	netdev_dbg(ndev->netdev, "%s header rx (skb->len: %d data_len: %d)\n",
		   __func__, skb->len, skb->data_len);
	hdr = (struct rx_metadata_cv2 *)skb->data;
	kvx_eth_dump_rx_hdr_cv2(ndev->hw, hdr);

	if (hdr->mac_error)
		ndev->stats.ring.skb_mac_err++;

	if (hdr->crc_errors)
		ndev->stats.ring.skb_crc_err++;

	skb_pull(skb, hdr_size);
	skb->ip_summed = CHECKSUM_UNNECESSARY;

	return 0;
}

static int kvx_eth_rx_frame(struct kvx_eth_ring *rxr, u32 qdesc_idx,
			    dma_addr_t buf, size_t len, u64 eop)
{
	struct net_device *netdev = rxr->netdev;
	struct kvx_eth_netdev *ndev = netdev_priv(netdev);
	struct kvx_qdesc *qdesc = &rxr->pool.qdesc[qdesc_idx];
	const struct kvx_eth_chip_rev_data *rev_d = kvx_eth_get_rev_data(ndev->hw);
	enum dma_data_direction dma_dir;
	void *data, *data_end, *va = NULL;
	struct page *page = NULL;
	size_t data_len = len; /* Assuming no FCS fwd from MAC*/
	int ret = 0;

	page = qdesc->va;
	if (KVX_SKB_SIZE(len) > PAGE_SIZE) {
		netdev_err(netdev, "Rx buffer exceeds PAGE_SIZE\n");
		return -ENOBUFS;
	}
	if (!(ndev->rx_dma_cache_shoot_through)) {
		dma_dir = page_pool_get_dma_dir(rxr->pool.pagepool);
		dma_sync_single_for_cpu(ndev->dev, buf, len, dma_dir);
	}

	if (likely(!rxr->skb)) {
		va = page_address(page);
		/* Prefetch header */
		prefetch(va);
		data = va + KVX_RX_HEADROOM;
		data_end = data + data_len;
		rxr->skb = build_skb(va, KVX_SKB_SIZE(data_len));
		if (unlikely(!rxr->skb)) {
			ndev->stats.ring.skb_alloc_err++;
			ret = -ENOMEM;
			goto recycle_page;
		}
		skb_reserve(rxr->skb, data - va);
		skb_put(rxr->skb, data_end - data);
	} else {
		skb_add_rx_frag(rxr->skb, skb_shinfo(rxr->skb)->nr_frags, page,
			KVX_RX_HEADROOM, data_len, data_len);
	}

	if (eop) {
		rev_d->kvx_eth_rx_hdr(ndev, rxr->skb);
		rxr->skb->dev = rxr->napi.dev;
		skb_record_rx_queue(rxr->skb, ndev->dma_cfg.rx_chan_id.start +
				    rxr->qidx);
		rxr->skb->protocol = eth_type_trans(rxr->skb, netdev);
		ndev->stats.ring.rx_pkts++;
		netdev_dbg(ndev->netdev, "%s skb->len: %d data_len: %d\n",
			   __func__, rxr->skb->len, rxr->skb->data_len);
	}
	ndev->stats.ring.rx_bytes += data_len;

	/* Release descriptor */
	page_pool_release_page(rxr->pool.pagepool, page);
	qdesc->va = NULL;
	qdesc->dma_addr = 0;

	return 0;

recycle_page:
	page_pool_recycle_direct(rxr->pool.pagepool, page);
	return ret;
}

/* kvx_eth_clean_rx_irq() - Clears received RX buffers
 *
 * Called from napi poll:
 *  - handles RX metadata
 *  - RX buffer re-allocation if needed
 * @napi: Pointer to napi struct in rx ring
 * @work_done: returned nb of buffers completed
 * @work_left: napi budget
 *
 * Return: 0 on success
 */
static int kvx_eth_clean_rx_irq(struct napi_struct *napi, int work_left)
{
	struct kvx_eth_ring *rxr = container_of(napi, struct kvx_eth_ring,
						napi);
	struct net_device *netdev = rxr->netdev;
	struct kvx_eth_netdev *ndev = netdev_priv(netdev);
	struct kvx_dma_config *dma_cfg = &ndev->dma_cfg;
	struct kvx_dma_pkt_full_desc *pkt;
	u32 rx_r = rxr->next_to_clean;
	int work_done = 0;
	int rx_count = 0;
	int ret = 0;

	if (rx_r ==  rxr->next_to_use) {
		netdev_warn(netdev, "!! %s rx_r == rx_w == %d\n",
			    __func__, rxr->next_to_use);
		return work_done;
	}
	while (!kvx_dma_get_rx_completed(dma_cfg->pdev, rxr->dma_chan, &pkt)) {
		work_done++;

		ret = kvx_eth_rx_frame(rxr, rx_r, (dma_addr_t)pkt->base,
				       (size_t)pkt->byte, pkt->notif);
		/* Still some RX segments pending */
		if (likely(!ret && pkt->notif)) {
			napi_gro_receive(napi, rxr->skb);
			rxr->skb = NULL;
		}

		rx_r = ((rx_r >= rxr->count - 1) ? 0 : rx_r + 1);
		if (work_done >= work_left)
			break;
	}
	/* Concurrent access */
	smp_store_mb(rxr->next_to_clean, rx_r);
	rx_count = kvx_eth_desc_unused(rxr);
	if (rx_count > (2 * rxr->count) / 3)
		kvx_eth_alloc_rx_buffers(rxr, rx_count);

	return work_done;
}

/* kvx_eth_netdev_poll() - NAPI polling callback
 * @napi: NAPI pointer
 * @budget: internal budget def
 *
 * Return: Number of buffers completed
 */
static int kvx_eth_netdev_poll(struct napi_struct *napi, int budget)
{
	int work_done = kvx_eth_clean_rx_irq(napi, budget);

	if (work_done < budget)
		napi_complete_done(napi, work_done);

	return work_done;
}

#ifdef CONFIG_NET_POLL_CONTROLLER
static void kvx_eth_netdev_poll_controller(struct net_device *netdev)
{
	struct kvx_eth_netdev *ndev = netdev_priv(netdev);

	napi_schedule(&ndev->rx_ring[0]->napi);
}
#endif

/* kvx_eth_set_mac_addr() - Sets HW address
 * @netdev: Current netdev
 * @p: HW addr
 *
 * Return: 0 on success, -EADDRNOTAVAIL if mac addr NOK
 */
static int kvx_eth_set_mac_addr(struct net_device *netdev, void *p)
{
	struct kvx_eth_netdev *ndev = netdev_priv(netdev);
	struct sockaddr *addr = p;

	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;

	eth_hw_addr_set(netdev, addr->sa_data);
	memcpy(ndev->cfg.mac_f.addr, addr->sa_data, netdev->addr_len);

	kvx_mac_set_addr(ndev->hw, &ndev->cfg);

	return 0;
}

/* kvx_eth_change_mtu() - Change the Maximum Transfer Unit
 * @netdev: network interface device structure
 * @new_mtu: new value for maximum frame size
 *
 * Return: 0 on success
 */
static int kvx_eth_change_mtu(struct net_device *netdev, int new_mtu)
{
	struct kvx_eth_netdev *ndev = netdev_priv(netdev);
	int ret = 0, max_frame_len = new_mtu + (2 * KVX_ETH_HEADER_SIZE);
	struct kvx_eth_hw *hw = ndev->hw;
	const struct kvx_eth_chip_rev_data *rev_d = kvx_eth_get_rev_data(hw);

	ndev->rx_buffer_len = ALIGN(max_frame_len, KVX_ETH_PKT_ALIGN);
	ndev->hw->max_frame_size = max_frame_len;
	netdev->mtu = new_mtu;

	rev_d->eth_hw_change_mtu(ndev->hw, ndev->cfg.id, max_frame_len);

	if (netif_carrier_ok(netdev)) {
		if (!atomic_read(&ndev->link_cfg_running) && !work_pending(&ndev->link_cfg)) {
			ret = kvx_eth_mac_cfg(hw, &ndev->cfg);
			if (ret)
				dev_warn(hw->dev, "Failed to reconfigure MAC (ret = %d)\n", ret);
		}
	}

	return ret;
}

static void kvx_eth_change_rx_flags(struct net_device *netdev, int flags)
{
	struct kvx_eth_netdev *ndev = netdev_priv(netdev);

	ndev->cfg.mac_f.promisc_mode = (flags & IFF_PROMISC) ? true : false;

	kvx_eth_mac_init(ndev->hw, &ndev->cfg);
}

/* kvx_eth_netdev_get_stats64() - Update stats
 * @netdev: Current netdev
 * @stats: Statistic struct
 */
static void kvx_eth_netdev_get_stats64(struct net_device *netdev,
				       struct rtnl_link_stats64 *stats)
{
	struct kvx_eth_netdev *ndev = netdev_priv(netdev);

	kvx_eth_update_stats64(ndev->hw, ndev->cfg.id, &ndev->stats);

	stats->rx_packets = ndev->stats.ring.rx_pkts;
	stats->tx_packets = ndev->stats.ring.tx_pkts;
	stats->rx_bytes = ndev->stats.ring.rx_bytes;
	stats->tx_bytes = ndev->stats.ring.tx_bytes;
	stats->rx_errors = ndev->stats.rx.ifinerrors;
	stats->tx_errors = ndev->stats.tx.ifouterrors;
	stats->rx_dropped = ndev->stats.rx.etherstatsdropevents;
	stats->multicast = ndev->stats.rx.ifinmulticastpkts;

	stats->rx_length_errors = ndev->stats.rx.inrangelengtherrors;
	stats->rx_crc_errors = ndev->stats.rx.framechecksequenceerrors;
	stats->rx_frame_errors = ndev->stats.rx.alignmenterrors;
}

/* Allow userspace to determine which ethernet controller
 * is behind this netdev, independently of the netdev name
 */
static int
kvx_eth_get_phys_port_name(struct net_device *dev, char *name, size_t len)
{
	struct kvx_eth_netdev *ndev = netdev_priv(dev);
	int n;

	n = snprintf(name, len, "enmppa%d",
		     ndev->hw->eth_id * KVX_ETH_LANE_NB + ndev->cfg.id);

	if (n >= len)
		return -EINVAL;

	return 0;
}

static int
kvx_eth_get_phys_port_id(struct net_device *dev, struct netdev_phys_item_id *id)
{
	struct kvx_eth_netdev *ndev = netdev_priv(dev);

	id->id_len = 1;
	id->id[0] = ndev->hw->eth_id * KVX_ETH_LANE_NB + ndev->cfg.id;

	return 0;
}

/**
 * Note: implementing ndo_tx_timeout may break datapath handled by fw
 **/
static const struct net_device_ops kvx_eth_netdev_ops = {
	.ndo_init               = kvx_eth_netdev_init,
	.ndo_uninit             = kvx_eth_netdev_uninit,
	.ndo_open               = kvx_eth_netdev_open,
	.ndo_stop               = kvx_eth_netdev_stop,
	.ndo_start_xmit         = kvx_eth_netdev_start_xmit,
	.ndo_get_stats64        = kvx_eth_netdev_get_stats64,
	.ndo_validate_addr      = eth_validate_addr,
	.ndo_set_mac_address    = kvx_eth_set_mac_addr,
	.ndo_change_mtu         = kvx_eth_change_mtu,
	.ndo_change_rx_flags    = kvx_eth_change_rx_flags,
	.ndo_get_phys_port_name = kvx_eth_get_phys_port_name,
	.ndo_get_phys_port_id   = kvx_eth_get_phys_port_id,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller    = kvx_eth_netdev_poll_controller,
#endif
};

static void kvx_eth_dma_irq_rx(void *data)
{
	struct kvx_eth_ring *ring = data;

	netdev_dbg(ring->netdev, "%s\n", __func__);
	napi_schedule(&ring->napi);
}

static struct page_pool *kvx_eth_create_rx_pool(struct kvx_eth_netdev *ndev,
						size_t size)
{
	struct kvx_dma_config *dma_cfg = &ndev->dma_cfg;
	struct page_pool *pool = NULL;
	struct page_pool_params pp_params = {
		.order = 0,
		.flags = PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV,
		.pool_size = dma_cfg->rx_chan_id.nb * size,
		.nid = NUMA_NO_NODE,
		.dma_dir = DMA_BIDIRECTIONAL,
		.offset = KVX_RX_HEADROOM,
		.max_len = KVX_MAX_RX_BUF_SIZE,
		/* Device must be the same for dma_sync_single_for_cpu */
		.dev = ndev->dev,
	};

	pool = page_pool_create(&pp_params);
	if (IS_ERR(pool))
		dev_err(ndev->dev, "cannot create rx page pool\n");

	return pool;
}

static int kvx_eth_alloc_rx_pool(struct kvx_eth_netdev *ndev,
			       struct kvx_eth_ring *r)
{
	struct kvx_buf_pool *rx_pool = &r->pool;

	rx_pool->qdesc = kcalloc(r->count, sizeof(*rx_pool->qdesc), GFP_KERNEL);
	if (!rx_pool->qdesc)
		return -ENOMEM;
	rx_pool->pagepool = kvx_eth_create_rx_pool(ndev, r->count);
	if (IS_ERR(rx_pool->pagepool)) {
		kfree(rx_pool->qdesc);
		netdev_err(ndev->netdev, "Unable to allocate page pool\n");
		return -ENOMEM;
	}

	return 0;
}

static void kvx_eth_release_rx_pool(struct kvx_eth_ring *r)
{
	u32 unused_desc = kvx_eth_desc_unused(r);
	u32 rx_r = r->next_to_clean;
	struct kvx_qdesc *qdesc;

	kvx_dma_flush_rx_jobq(r->dma_chan);
	while (--unused_desc) {
		qdesc = &r->pool.qdesc[rx_r];

		if (rx_r == r->next_to_use)
			break;
		if (qdesc)
			page_pool_release_page(r->pool.pagepool,
					       (struct page *)(qdesc->va));
		rx_r = ((rx_r >= r->count - 1) ? 0 : rx_r + 1);
	}
	/* Concurrent access */
	smp_store_mb(r->next_to_clean, rx_r);
	page_pool_destroy(r->pool.pagepool);
	kfree(r->pool.qdesc);
}

int kvx_eth_alloc_rx_ring(struct kvx_eth_netdev *ndev, struct kvx_eth_ring *r)
{
	struct kvx_dma_config *dma_cfg = &ndev->dma_cfg;
	struct kvx_eth_dt_f dt;
	int rx_chan, ret = 0;
	struct kvx_eth_hw *hw =  ndev->hw;
	struct kvx_eth_dev *dev = KVX_HW2DEV(hw);
	const struct kvx_eth_chip_rev_data *rev_d = dev->chip_rev_data;
	int jobq_id, phys_port_id = ndev->hw->eth_id * KVX_ETH_LANE_NB + ndev->cfg.id;

	r->count = kvx_dma_get_max_nb_desc(dma_cfg->pdev);
	kvx_eth_reset_ring(r);

	ret = kvx_eth_alloc_rx_pool(ndev, r);
	if (ret) {
		netdev_err(ndev->netdev, "Failed to get RX pool\n");
		goto exit;
	}

	netif_napi_add(ndev->netdev, &r->napi, kvx_eth_netdev_poll);
	r->netdev = ndev->netdev;

	/* Reserve channel only once */
	if (!r->init_done) {
		memset(&r->param, 0, sizeof(r->param));
		r->param.rx_cache_id = (dma_cfg->rx_cache_id + r->qidx) % RX_CACHE_NB;
		rx_chan = dma_cfg->rx_chan_id.start + r->qidx;

		r->dma_chan = kvx_dma_get_rx_phy(dma_cfg->pdev, rx_chan);
		ret = kvx_dma_reserve_rx_chan(dma_cfg->pdev, r->dma_chan,
					      &r->param, kvx_eth_dma_irq_rx, r);
		if (ret)
			goto chan_failed;

		if (kvx_eth_get_rev_data(ndev->hw)->revision == COOLIDGE_V1)
			jobq_id = rx_chan;
		else
			jobq_id = phys_port_id; //Just need an unused jobq, could be dynamic
		ret = kvx_dma_reserve_rx_jobq(dma_cfg->pdev, &r->rx_jobq,
					      jobq_id, r->param.rx_cache_id,
					      rx_jobq_prio[r->type]);
		if (ret)
			goto jobq_failed;

		dt.cluster_id = kvx_cluster_id();
		dt.rx_channel = rx_chan;
		dt.split_trigger = 0;
		dt.vchan = hw->vchan;
		if (rev_d->eth_add_dispatch_table_entry)
			rev_d->eth_add_dispatch_table_entry(hw, &ndev->cfg, &dt,
				ndev->cfg.default_dispatch_entry + dt.rx_channel);
		r->init_done = true;
	}
	return 0;

jobq_failed:
	kvx_dma_release_chan(dma_cfg->pdev, r->dma_chan, &r->param);
chan_failed:
	netif_napi_del(&r->napi);
	kvx_eth_release_rx_pool(r);
exit:
	return ret;
}

/* kvx_eth_release_rx_ring() - Release RX ring
 *
 * Flush dma rx job queue and release all pending buffers previously allocated
 * @r: Rx ring to be release
 * @keep_dma_chan: do not release dma channel
 */
void kvx_eth_release_rx_ring(struct kvx_eth_ring *r, int keep_dma_chan)
{
	struct kvx_eth_netdev *ndev = netdev_priv(r->netdev);
	struct kvx_dma_config *dma_cfg = &ndev->dma_cfg;

	netif_napi_del(&r->napi);
	kvx_eth_release_rx_pool(r);
	if (!keep_dma_chan) {
		kvx_dma_release_rx_jobq(dma_cfg->pdev, r->rx_jobq);
		kvx_dma_release_chan(dma_cfg->pdev, r->dma_chan, &r->param);
		r->init_done = false;
	}
}

/* kvx_eth_alloc_rx_res() - Allocate RX resources
 * @netdev: Current netdev
 *
 * Return: 0 on success, < 0 on failure
 */
static int kvx_eth_alloc_rx_res(struct net_device *netdev)
{
	struct kvx_eth_netdev *ndev = netdev_priv(netdev);
	int i, qidx, ret = 0;

	for (qidx = 0; qidx < ndev->dma_cfg.rx_chan_id.nb; qidx++) {
		ndev->rx_ring[qidx].qidx = qidx;
		ret = kvx_eth_alloc_rx_ring(ndev, &ndev->rx_ring[qidx]);
		if (ret)
			goto alloc_failed;
	}

	return 0;

alloc_failed:
	for (i = qidx - 1; i >= 0; i--)
		kvx_eth_release_rx_ring(&ndev->rx_ring[i], 0);

	return ret;
}

void kvx_eth_release_rx_res(struct net_device *netdev, int keep_dma_chan)
{
	struct kvx_eth_netdev *ndev = netdev_priv(netdev);
	int qidx;

	for (qidx = 0; qidx < ndev->dma_cfg.rx_chan_id.nb; qidx++)
		kvx_eth_release_rx_ring(&ndev->rx_ring[qidx], keep_dma_chan);
}

int kvx_eth_alloc_tx_ring(struct kvx_eth_netdev *ndev, struct kvx_eth_ring *r)
{
	struct kvx_dma_config *dma_cfg = &ndev->dma_cfg;
	int ret = 0;

	r->netdev = ndev->netdev;
	kvx_eth_reset_ring(r);
	if (r->count == 0)
		r->count = kvx_dma_get_max_nb_desc(dma_cfg->pdev);
	r->tx_buf = kcalloc(r->count, sizeof(*r->tx_buf), GFP_KERNEL);
	if (!r->tx_buf) {
		netdev_err(r->netdev, "TX ring allocation failed\n");
		return -ENOMEM;
	}
	if (!r->init_done) {
		r->dma_chan = kvx_dma_get_tx_phy(dma_cfg->pdev,
				dma_cfg->tx_chan_id.start + r->qidx);
		memset(&r->param, 0, sizeof(r->param));
		r->param.noc_route = noc_route_c2eth(ndev->hw->eth_id,
						      kvx_cluster_id());
		/* rx_tag must refer to tx_fifo_id*/
		r->param.rx_tag = ndev->cfg.tx_fifo_id;
		r->param.qos_id = 0;

		ret = kvx_dma_reserve_tx_chan(dma_cfg->pdev, r->dma_chan,
			&r->param, kvx_eth_netdev_dma_callback_tx, r);
		if (ret)
			goto chan_failed;
		r->init_done = true;
	}

	return 0;

chan_failed:
	kfree(r->tx_buf);
	r->tx_buf = NULL;

	return ret;
}

/* kvx_eth_release_tx_ring() - Release TX resources
 * @r: Ring to be released
 * @keep_dma_chan: do not release dma channel
 */
void kvx_eth_release_tx_ring(struct kvx_eth_ring *r, int keep_dma_chan)
{
	struct kvx_eth_netdev *ndev = netdev_priv(r->netdev);
	struct kvx_eth_tx_f *tx_f;

	if (!keep_dma_chan)
		kvx_dma_release_chan(ndev->dma_cfg.pdev, r->dma_chan, &r->param);

    /* currently unused on cv2 */
	if (!list_empty(&ndev->cfg.tx_fifo_list)) {
		tx_f = &ndev->hw->tx_f[ndev->dma_cfg.tx_chan_id.start + r->qidx];
		list_del_init(&tx_f->node);
	}

	kfree(r->tx_buf);
	r->tx_buf = NULL;
	r->init_done = false;
}

/* kvx_eth_alloc_tx_res() - Allocate TX resources (including dma_noc channel)
 * @netdev: Current netdev
 *
 * Return: 0 on success, < 0 on failure
 */
static int kvx_eth_alloc_tx_res(struct net_device *netdev)
{
	struct kvx_eth_netdev *ndev = netdev_priv(netdev);
	struct kvx_eth_tx_f *tx_f;
	struct kvx_eth_ring *r;
	int i, qidx, ret = 0;
	struct kvx_eth_hw *hw = ndev->hw;
	enum coolidge_rev chip_rev =  kvx_eth_get_rev_data(hw)->revision;

	if (chip_rev == COOLIDGE_V1)
		tx_f = &hw->tx_f[ndev->cfg.tx_fifo_id];
	else
		tx_f = NULL;

	if (tx_f) {
		tx_f->lane_id = ndev->cfg.id;
		list_add_tail(&tx_f->node, &ndev->cfg.tx_fifo_list);
	}
	for (qidx = 0; qidx < ndev->dma_cfg.tx_chan_id.nb; qidx++) {
		r = &ndev->tx_ring[qidx];
		r->qidx = qidx;

		ret = kvx_eth_alloc_tx_ring(ndev, r);
		if (ret)
			goto alloc_failed;
	}

	return 0;

alloc_failed:
	if (tx_f)
		list_del_init(&tx_f->node);
	for (i = qidx - 1; i >= 0; i--)
		kvx_eth_release_tx_ring(&ndev->tx_ring[i], 0);

	return ret;
}

static void kvx_eth_release_tx_res(struct net_device *netdev, int keep_dma_chan)
{
	struct kvx_eth_netdev *ndev = netdev_priv(netdev);
	int qidx;

	for (qidx = 0; qidx < ndev->dma_cfg.tx_chan_id.nb; qidx++)
		kvx_eth_release_tx_ring(&ndev->tx_ring[qidx], keep_dma_chan);
}

static int kvx_eth_get_queue_nb(struct platform_device *pdev,
				struct kvx_eth_node_id *txq,
				struct kvx_eth_node_id *rxq)
{
	struct device_node *np = pdev->dev.of_node;

	if (of_property_read_u32_array(np, "kalray,dma-tx-channel-ids",
					(u32 *)txq, 2)) {
		dev_err(&pdev->dev, "Unable to get dma-tx-channel-ids\n");
		return -EINVAL;
	}
	if (txq->nb > 1) {
		dev_err(&pdev->dev, "TX channels nb (%d) is limited to 1\n",
			txq->nb);
		return -EINVAL;
	}

	if (of_property_read_u32_array(np, "kalray,dma-rx-channel-ids",
				       (u32 *)rxq, 2)) {
		dev_err(&pdev->dev, "Unable to get dma-rx-channel-ids\n");
		return -EINVAL;
	}
	if (rxq->nb > RX_CACHE_NB) {
		dev_warn(&pdev->dev, "Limiting RX queue number to %d\n",
			 RX_CACHE_NB);
		rxq->nb = RX_CACHE_NB;
	}
	if (rxq->start + rxq->nb > KVX_ETH_RX_TAG_NB) {
		dev_err(&pdev->dev, "RX channels (%d) exceeds max value (%d)\n",
			rxq->start + rxq->nb, KVX_ETH_RX_TAG_NB);
		return -EINVAL;
	}
	return 0;
}

/* kvx_eth_check_dma() - Check dma noc driver and device correclty loaded
 *
 * @pdev: netdev platform device pointer
 * @np_dma: dma device node pointer
 * Return: dma platform device on success, NULL on failure
 */
static struct platform_device *kvx_eth_check_dma(struct platform_device *pdev,
						 struct device_node **np_dma)
{
	struct platform_device *dma_pdev;

	*np_dma = of_parse_phandle(pdev->dev.of_node, "dmas", 0);
	if (!(*np_dma)) {
		dev_err(&pdev->dev, "Failed to get dma\n");
		return NULL;
	}
	dma_pdev = of_find_device_by_node(*np_dma);
	if (!dma_pdev || !platform_get_drvdata(dma_pdev)) {
		dev_err(&pdev->dev, "Failed to get dma_noc platform_device\n");
		return NULL;
	}

	return dma_pdev;
}

/* kvx_eth_rtm_parse_dt() - Parse retimer related device tree inputs
 *
 * @pdev: platform device
 * @dev: Current kvx_eth_dev
 * Return: 0 on success, < 0 on failure
 */
int kvx_eth_rtm_parse_dt(struct platform_device *pdev, struct kvx_eth_dev *dev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *rtm_node;
	int rtm, ret;

	for (rtm = 0; rtm < RTM_NB; rtm++) {
		rtm_node = of_parse_phandle(pdev->dev.of_node,
				rtm_prop_name[rtm], 0);
		if (!rtm_node) {
			/* This board is missing retimers, throw an info and
			 * return to stop parsing other retimer parameters
			 */
			dev_info(&pdev->dev, "No node %s found\n",
					rtm_prop_name[rtm]);
			return 0;
		}
		dev->hw.rtm_params[rtm].rtm = of_find_i2c_device_by_node(rtm_node);
		if (!dev->hw.rtm_params[rtm].rtm)
			return -EPROBE_DEFER;
	}

	for (rtm = 0; rtm < RTM_NB; rtm++) {
		ret = of_property_count_u32_elems(np,
				rtm_channels_prop_name[rtm]);
		if (ret < 0) {
			dev_err(&pdev->dev, "Unable to get %s\n",
					rtm_channels_prop_name[rtm]);
			return -EINVAL;
		} else if (ret != KVX_ETH_LANE_NB) {
			dev_err(&pdev->dev, "Incorrect channels number for %s (got %d, want %d)\n",
					rtm_channels_prop_name[rtm], ret,
					KVX_ETH_LANE_NB);
			return -EINVAL;
		}
		ret = of_property_read_u32_array(np,
				rtm_channels_prop_name[rtm],
				dev->hw.rtm_params[rtm].channels,
				KVX_ETH_LANE_NB);
		if (ret) {
			dev_err(&pdev->dev, "Failed to request %s\n",
					rtm_channels_prop_name[rtm]);
			return ret;
		}
	}

	return 0;
}

static irqreturn_t kvx_eth_irq_rx_error(int irq, void *data)
{
	struct kvx_eth_dev *dev = data;

	if (kvx_lbrfs_readl(&dev->hw,
			KVX_ETH_LBR_INTERRUPT_STATUS_OFFSET) & 0x1) {
		kvx_lbrfs_readl(&dev->hw,
			KVX_ETH_LBR_INTERRUPT_STATUS_LAC_OFFSET);
		if (dev->hw.lb_rfs_f.it_tbl_corrupt_cnt < U32_MAX)
			dev->hw.lb_rfs_f.it_tbl_corrupt_cnt++;
	}
	return IRQ_HANDLED;
}

static irqreturn_t kvx_eth_irq_link_down(int irq, void *data)
{
	struct kvx_eth_dev *dev = (struct kvx_eth_dev *)data;
	struct kvx_eth_netdev *ndev;
	struct kvx_eth_hw *hw = &dev->hw;
	u32 msk, v;

	spin_lock(&hw->link_down_lock);
	v = kvx_mac_readl(hw, MAC_LINK_DOWN_IT_LAC_OFFSET);
	v &= kvx_mac_readl(hw, MAC_LINK_DOWN_IT_EN_OFFSET); /* skip disabled ITs */

	/* disable raised ITs */
	updatel_bits(hw, MAC, MAC_LINK_DOWN_IT_EN_OFFSET, v, 0)

	/* trig setup links of proper net dev in the link_polling context */
	list_for_each_entry(ndev, &dev->list, node) {
		msk = kvx_eth_lane_mask(ndev);
		if (msk & v) {
			netdev_dbg(ndev->netdev, "%s netdev[%d}\n", __func__, ndev->cfg.id);
			/* disable ITs linked to the lanes hold by this netdev */
			updatel_bits(hw, MAC, MAC_LINK_DOWN_IT_EN_OFFSET, msk, 0)
			/* serdes restart not requested:
			 * - autoneg disabled: same speed,
			 * - autoneg enabled: restart done in AN procedure
			 */
			kvx_eth_setup_link(ndev, false);
		}
	}
	spin_unlock(&hw->link_down_lock);
	return IRQ_HANDLED;
}

struct irq_s {
	const char *name;
	irqreturn_t (*hdl)(int irq, void *data);
};
const struct irq_s kvx_eth_irq[] = {
	{ .name = "tx_ptp", .hdl = NULL },
	{ .name = "tx_drop", .hdl = NULL },
	{ .name = "rx_error", .hdl = kvx_eth_irq_rx_error},
	{ .name = "rx_event", .hdl = NULL },
	{ .name = "link_down", .hdl = kvx_eth_irq_link_down},
	{ .name = "pps", .hdl = NULL },
};

/* kvx_eth_dev_parse_dt() - Parse eth device tree inputs
 *
 * @pdev: platform device
 * @dev: Current kvx_eth_dev
 * Return: 0 on success, < 0 on failure
 */
int kvx_eth_dev_parse_dt(struct platform_device *pdev, struct kvx_eth_dev *dev)
{
	struct device_node *np = pdev->dev.of_node;
	u32 tmp_rx_polarities[KVX_ETH_LANE_NB] = {0};
	u32 tmp_tx_polarities[KVX_ETH_LANE_NB] = {0};
	struct nvmem_cell *cell;
	int i, ret = 0;
	u8 *cell_data;
	size_t len;
	u32 val;

	if (of_property_read_u32(np, "cell-index", &dev->hw.eth_id)) {
		dev_warn(&pdev->dev, "Default kvx ethernet index to 0\n");
		dev->hw.eth_id = KVX_ETH0;
	}

	if (dev->chip_rev_data->irq) {
		for (i = 0; i < ARRAY_SIZE(kvx_eth_irq); i++) {
			if (!kvx_eth_irq[i].hdl)
				continue;
			ret = platform_get_irq_byname_optional(pdev, kvx_eth_irq[i].name);
			if (ret > 0) {
				if (devm_request_irq(&pdev->dev, ret, kvx_eth_irq[i].hdl, IRQF_TRIGGER_HIGH,
						     dev_name(&pdev->dev), (void *)dev)) {
					dev_err(&pdev->dev, "Failed to request irq %s\n", kvx_eth_irq[i].name);
				}
			}
		}
	}

	if (of_property_read_u32(np, "kalray,rxtx-crossed",
			&dev->hw.rxtx_crossed) != 0)
		dev->hw.rxtx_crossed = 0;
	if (of_property_read_u32(np, "kalray,parsers_tictoc",
			&dev->hw.parsers_tictoc) != 0) {
		dev_err(&pdev->dev, "kalray,parsers_tictoc property not found but required\n");
		return -EINVAL;
	}
	dev_info(&pdev->dev, "parser tictoc (only in aggregated mode): %d\n",
		 dev->hw.parsers_tictoc);

	of_property_read_u32(np, "kalray,limit_rx_pps", &dev->hw.limit_rx_pps);
	if (dev->hw.limit_rx_pps)
		dev_warn(&pdev->dev, "!!LIMIT pps %d\n", dev->hw.limit_rx_pps);

	if (of_property_read_u32(np, "kalray,aggregated_only",
			     &dev->hw.aggregated_only) != 0)
		dev->hw.aggregated_only = 1;

	if (dev->hw.aggregated_only)
		dev_warn(&pdev->dev, "Configs 4x1G/4x10G/4x25G not available\n");

	if (of_property_read_u32_array(np, "kalray,dma-rx-chan-error",
				       (u32 *)&dev->hw.rx_chan_error, 1) != 0)
		dev->hw.rx_chan_error = 0xFF;

	of_property_read_u32_array(np, "kalray,rx-phy-polarities",
			tmp_rx_polarities, KVX_ETH_LANE_NB);
	of_property_read_u32_array(np, "kalray,tx-phy-polarities",
			tmp_tx_polarities, KVX_ETH_LANE_NB);

	for (i = 0; i < KVX_ETH_LANE_NB; i++) {
		dev->hw.phy_f.polarities[i].rx = (bool) tmp_rx_polarities[i];
		dev->hw.phy_f.polarities[i].tx = (bool) tmp_tx_polarities[i];
	}

	cell = nvmem_cell_get(&pdev->dev, "ews_fuse");
	if (!IS_ERR(cell)) {
		cell_data = nvmem_cell_read(cell, &len);
		nvmem_cell_put(cell);
		if (!IS_ERR(cell_data))
			dev->hw.mppa_id = *(u64 *)cell_data;
		kfree(cell_data);
	}

	cell = nvmem_cell_get(&pdev->dev, "ft_fuse");
	if (!IS_ERR(cell)) {
		cell_data = nvmem_cell_read(cell, &len);
		nvmem_cell_put(cell);
		if (!IS_ERR(cell_data)) {
			val = *(u32 *)cell_data;
			dev->hw.dev_id = (val >> 22) & 0x1FF;
		}
		kfree(cell_data);
	}
	ret = kvx_eth_rtm_parse_dt(pdev, dev);

	if (of_property_read_u8(np, "kalray,fom_thres", &dev->hw.fom_thres))
		dev->hw.fom_thres = FOM_THRESHOLD;

	return ret;
}

/* kvx_eth_netdev_set_hw_addr() - Use nvmem to get mac addr
 *
 * @ndev: kvx net device
 */
static void kvx_eth_netdev_set_hw_addr(struct kvx_eth_netdev *ndev)
{
	struct net_device *netdev = ndev->netdev;
	struct kvx_eth_hw *hw = ndev->hw;
	struct kvx_eth_dev *dev = KVX_HW2DEV(hw);
	struct device *d = &dev->pdev->dev;
	u8 addr[ETH_ALEN];
	u8 *a, tmp[6];
	u64 h;
	int err;

	/* use the platform device set by kvx_eth_create_netdev() which
	 * holds the right of_node.
	 */
	err = of_get_mac_address(ndev->dev->of_node, addr);
	if (!err) {
		a = (u8 *)addr;
		/* set local assignment bit (IEEE802) */
		a[0] |= 0x02;
	} else if (false) {
		/* waiting for MAC address in fuse */
	} else if (dev->hw.mppa_id != 0) {
		h = hash_64((dev->hw.mppa_id << 9) | dev->hw.dev_id, 16);
		/* set local assignment bit (IEEE802) */
		tmp[0] = 0xA0 | 0x02;
		tmp[1] = 0x28;
		tmp[2] = 0x33;
		tmp[3] = 0xC0 | (0x0F & h);
		tmp[4] = 0xFF & (h >> 4);
		tmp[5] = (0xF0 & ((h >> 12) << 4)) |
	       ((ndev->hw->eth_id << 2 | ndev->cfg.id) & 0x0F);
	    a = tmp;
	} else {
		dev_warn(d, "Using random hwaddr\n");
		eth_hw_addr_random(netdev);
		ether_addr_copy(ndev->cfg.mac_f.addr, netdev->dev_addr);
		return;
	}

	netdev->addr_assign_type = NET_ADDR_PERM;
	eth_hw_addr_set(netdev, a);
	ether_addr_copy(ndev->cfg.mac_f.addr, a);
}

/* kvx_eth_netdev_parse_dt() - Parse netdev device tree inputs
 *
 * Sets dma properties accordingly (dma_mem and iommu nodes)
 *
 * @pdev: platform device
 * @ndev: Current kvx_eth_netdev
 * Return: 0 on success, < 0 on failure
 */
int kvx_eth_netdev_parse_dt(struct platform_device *pdev,
		struct kvx_eth_netdev *ndev)
{
	struct kvx_dma_config *dma_cfg = &ndev->dma_cfg;
	struct device_node *np = pdev->dev.of_node;
	struct device_node *sfp_node;
	struct platform_device *sfp_pdev;
	struct device_node *np_dma;
	char pname[20];
	int i, ret = 0;

	dma_cfg->pdev = kvx_eth_check_dma(pdev, &np_dma);
	if (!dma_cfg->pdev)
		return -ENODEV;

	ret = of_dma_configure(&pdev->dev, np_dma, true);
	if (ret) {
		dev_err(&pdev->dev, "Failed to configure dma\n");
		return -EINVAL;
	}
	if (iommu_get_domain_for_dev(&pdev->dev)) {
		struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(&pdev->dev);

		if (fwspec && fwspec->num_ids) {
			ndev->hw->asn = fwspec->ids[0];
			dev_dbg(&pdev->dev, "ASN: %d\n", ndev->hw->asn);
		} else {
			dev_err(&pdev->dev, "Unable to get ASN property\n");
			return -ENODEV;
		}
	}

	of_property_read_u32(np_dma, "kalray,dma-noc-vchan", &ndev->hw->vchan);
	if (of_property_read_u32(np, "kalray,dma-rx-cache-id",
				 &dma_cfg->rx_cache_id)) {
		dev_err(ndev->dev, "Unable to get dma-rx-cache-id\n");
		return -EINVAL;
	}
	if (dma_cfg->rx_cache_id >= RX_CACHE_NB) {
		dev_err(ndev->dev, "dma-rx-cache-id >= %d\n", RX_CACHE_NB);
		return -EINVAL;
	}
	ret = kvx_eth_get_queue_nb(pdev, &dma_cfg->tx_chan_id,
				   &dma_cfg->rx_chan_id);
	if (ret)
		return ret;

	if (of_property_read_u32_array(np, "kalray,dma-rx-comp-queue-ids",
			       (u32 *)&dma_cfg->rx_compq_id, 2) != 0) {
		dev_err(ndev->dev, "Unable to get dma-rx-comp-queue-ids\n");
		return -EINVAL;
	}

	if (dma_cfg->rx_chan_id.start != dma_cfg->rx_compq_id.start ||
	    dma_cfg->rx_chan_id.nb != dma_cfg->rx_compq_id.nb) {
		dev_err(ndev->dev, "rx_chan_id(%d,%d) != rx_compq_id(%d,%d)\n",
			dma_cfg->rx_chan_id.start, dma_cfg->rx_chan_id.nb,
			dma_cfg->rx_compq_id.start, dma_cfg->rx_compq_id.nb);
		return -EINVAL;
	}

	if (of_property_read_u32_array(np, "kalray,default-dispatch-entry",
				(u32 *)&ndev->cfg.default_dispatch_entry, 1))
		ndev->cfg.default_dispatch_entry = KVX_ETH_DEFAULT_RULE_DTABLE_IDX;

	if (of_property_read_u32(np, "kalray,lane", &ndev->cfg.id)) {
		dev_err(ndev->dev, "Unable to get lane\n");
		return -EINVAL;
	}
	if (ndev->cfg.id >= KVX_ETH_LANE_NB) {
		dev_err(ndev->dev, "lane >= %d\n", KVX_ETH_LANE_NB);
		return -EINVAL;
	}

	if (kvx_eth_get_rev_data(ndev->hw)->revision == COOLIDGE_V1) {
		/* Always the case (means that netdev can share tx dma jobq) */
		ndev->cfg.tx_fifo_id = dma_cfg->tx_chan_id.start;
		if (ndev->cfg.tx_fifo_id >= TX_FIFO_NB) {
			dev_err(ndev->dev, "tx_fifo >= %d\n", TX_FIFO_NB);
			return -EINVAL;
		}
	}

	/* Default tx eq. parameter tuning */
	if (!of_property_read_u32_array(np, "kalray,phy-param",
			(u32 *)&ndev->hw->phy_f.param[ndev->cfg.id], 3))
		ndev->hw->phy_f.param[ndev->cfg.id].ovrd_en = true;
	/* For aggregated config, allow different params for lanes 1..3 */
	for (i = 1; i < KVX_ETH_LANE_NB; i++) {
		snprintf(pname, 20, "kalray,phy-param%d", i);
		if (!of_property_read_u32_array(np, pname,
					(u32 *)&ndev->hw->phy_f.param[i], 3))
			ndev->hw->phy_f.param[i].ovrd_en = true;
	}

	/* get qsfp platform device */
	sfp_node = of_parse_phandle(np, "sfp", 0);

	if (sfp_node) {
		sfp_pdev = of_find_device_by_node(sfp_node);
		if (!sfp_pdev) {
			dev_err(&pdev->dev, "Failed to find sfp platform device\n");
			return -EINVAL;
		}
		ndev->qsfp = platform_get_drvdata(sfp_pdev);
		if (!ndev->qsfp)
			return -EINVAL;
	}

	return 0;
}

int kvx_eth_speed_to_nb_lanes(unsigned int speed, unsigned int *lane_speed)
{
	int nb_lanes;
	unsigned int tmp_lane_speed;

	switch (speed) {
	case SPEED_100000:
		nb_lanes = KVX_ETH_LANE_NB;
		tmp_lane_speed = SPEED_25000;
		break;
	case SPEED_40000:
		nb_lanes = KVX_ETH_LANE_NB;
		tmp_lane_speed = SPEED_10000;
		break;
	case SPEED_50000:
		nb_lanes = 2;
		tmp_lane_speed = SPEED_25000;
		break;
	case SPEED_25000:
	case SPEED_10000:
		nb_lanes = 1;
		tmp_lane_speed = speed;
		break;
	case SPEED_1000:
		nb_lanes = 1;
		tmp_lane_speed = speed;
		break;
	default:
		return 0;
	}

	if (lane_speed != NULL)
		*lane_speed = tmp_lane_speed;

	return nb_lanes;
}

void kvx_eth_update_cable_modes(struct kvx_eth_netdev *ndev)
{
	if (!bitmap_empty(ndev->cfg.cable_supported, __ETHTOOL_LINK_MODE_MASK_NBITS))
		return;

	if (!ndev->qsfp)
		return;

	if (!is_cable_connected(ndev->qsfp))
		return;

	kvx_set_mode(ndev->cfg.cable_supported, Autoneg);
	kvx_set_mode(ndev->cfg.cable_supported, Pause);
	kvx_set_mode(ndev->cfg.cable_supported, Asym_Pause);
	kvx_set_mode(ndev->cfg.cable_supported, TP);
	kvx_set_mode(ndev->cfg.cable_supported, AUI);
	kvx_set_mode(ndev->cfg.cable_supported, MII);
	kvx_set_mode(ndev->cfg.cable_supported, FIBRE);
	kvx_set_mode(ndev->cfg.cable_supported, BNC);
	kvx_set_mode(ndev->cfg.cable_supported, Backplane);

	kvx_qsfp_parse_support(ndev->qsfp, ndev->cfg.cable_supported);
}

void kvx_eth_qsfp_connect(struct kvx_qsfp *qsfp)
{
	struct kvx_eth_netdev *ndev = kvx_qsfp_to_ops_data(qsfp, struct kvx_eth_netdev);

	bitmap_zero(ndev->cfg.cable_supported, __ETHTOOL_LINK_MODE_MASK_NBITS);

	if (!netif_carrier_ok(ndev->netdev))
		kvx_eth_up(ndev->netdev);
}

void kvx_eth_qsfp_disconnect(struct kvx_qsfp *qsfp)
{
	struct kvx_eth_netdev *ndev = kvx_qsfp_to_ops_data(qsfp, struct kvx_eth_netdev);

	/* Set back the RTM TX FIR */
	if (ndev->hw->rtm_params[RTM_TX].rtm) {
		ndev->hw->rtm_tx_coef.using_alternate_coeffs = false;
		kvx_eth_set_rtm_tx_fir(ndev->hw, &ndev->cfg, &fir_default_param);
	}

	kvx_eth_down(ndev->netdev);
}

/* This callback is usually not supported by passive copper cables.
 * In this case, MAC link status is polled at regular interval.
 */
void kvx_eth_qsfp_cdr_lol(struct kvx_qsfp *qsfp)
{
	struct kvx_eth_netdev *ndev = kvx_qsfp_to_ops_data(qsfp, struct kvx_eth_netdev);

	if (!netif_running(ndev->netdev))
		return;

	/* disable link polling  */
	if (ndev->link_poll_en) {
		ndev->link_poll_en = false;
		if (delayed_work_pending(&ndev->link_poll))
			cancel_delayed_work(&ndev->link_poll);
	}

	if (kvx_eth_mac_getlink(ndev->hw, &ndev->cfg))
		netdev_warn(ndev->netdev, "inconsistency detected: MAC status OK while qsfp lol asserted\n");

	kvx_eth_setup_link(ndev, true);
}

static struct kvx_qsfp_ops qsfp_ops = {
	.connect    = kvx_eth_qsfp_connect,
	.disconnect = kvx_eth_qsfp_disconnect,
	.cdr_lol    = kvx_eth_qsfp_cdr_lol,
};

/* kvx_eth_create_netdev() - Create new netdev
 * @pdev: Platform device
 * @dev: parent device
 * @cfg: configuration (duplicated to kvx_eth_netdev)
 *
 * Return: new kvx_eth_netdev on success, NULL on failure
 */
static struct kvx_eth_netdev*
kvx_eth_create_netdev(struct platform_device *pdev, struct kvx_eth_dev *dev)
{
	struct kvx_eth_netdev *ndev;
	struct net_device *netdev;
	struct kvx_eth_node_id txq, rxq;
	int phy_mode;
	int ret = 0;
	const struct kvx_eth_chip_rev_data *rev_d;

	ret = kvx_eth_get_queue_nb(pdev, &txq, &rxq);
	if (ret)
		return NULL;
	netdev = devm_alloc_etherdev_mqs(&pdev->dev, sizeof(*ndev),
					 txq.nb, rxq.nb);
	if (!netdev) {
		dev_err(&pdev->dev, "Failed to alloc netdev\n");
		return NULL;
	}
	SET_NETDEV_DEV(netdev, &pdev->dev);
	ndev = netdev_priv(netdev);
	memset(ndev, 0, sizeof(*ndev));
	netdev->netdev_ops = &kvx_eth_netdev_ops;
	netdev->mtu = ETH_DATA_LEN;
	netdev->max_mtu = KVX_ETH_MAX_MTU;
	ndev->dev = &pdev->dev;
	ndev->netdev = netdev;
	ndev->hw = &dev->hw;
	ndev->cfg.hw = ndev->hw;
	INIT_LIST_HEAD(&ndev->cfg.tx_fifo_list);
	INIT_DELAYED_WORK(&ndev->link_poll, kvx_eth_poll_link);
	INIT_WORK(&ndev->link_cfg, kvx_eth_link_cfg);
	rev_d = kvx_eth_get_rev_data(ndev->hw);
	ndev->link_poll_en = !(rev_d->lnk_dwn_it_support);

	if (kvx_eth_get_rev_data(ndev->hw)->revision == COOLIDGE_V1) {
		ndev->rx_dma_cache_shoot_through = false;
	} else {
		/*
		 * Ideally, we could check that shoot-through
		 * have not been disabled by checking bits of
		 * smem_psht_rd_dis in global_config field of
		 * mppa secure cluster registers
		 */
		ndev->rx_dma_cache_shoot_through = true;
	}

	phy_mode = fwnode_get_phy_mode(pdev->dev.fwnode);
	if (phy_mode < 0) {
		dev_err(&pdev->dev, "phy mode not set\n");
		return NULL;
	}

	ret = kvx_eth_netdev_parse_dt(pdev, ndev);
	if (ret)
		return NULL;

	if (ndev->qsfp) {
		ret = kvx_qsfp_ops_register(ndev->qsfp, &qsfp_ops, ndev);

		if (ret)
			goto exit;
	}

	kvx_eth_netdev_set_hw_addr(ndev);

	/* Allocate RX/TX rings */
	ret = kvx_eth_alloc_rx_res(netdev);
	if (ret)
		goto exit;

	ret = kvx_eth_alloc_tx_res(netdev);
	if (ret)
		goto tx_chan_failed;

	kvx_set_ethtool_ops(netdev);
	kvx_set_dcb_ops(netdev);

	/* Register the network device */
	ret = register_netdev(netdev);
	if (ret) {
		netdev_err(netdev, "Failed to register netdev (%i)\n", ret);
		goto err;
	}

	/* Populate list of netdev */
	INIT_LIST_HEAD(&ndev->node);
	list_add(&ndev->node, &dev->list);

	return ndev;

err:
	kvx_eth_release_tx_res(netdev, 0);
tx_chan_failed:
	kvx_eth_release_rx_res(netdev, 0);
exit:
	netdev_err(netdev, "Failed to create netdev\n");
	return NULL;
}

/* kvx_eth_free_netdev() - Releases netdev
 * @ndev: Current kvx_eth_netdev
 *
 * Return: 0
 */
static int kvx_eth_free_netdev(struct kvx_eth_netdev *ndev)
{
	cancel_delayed_work_sync(&ndev->link_poll);
	kvx_eth_release_tx_res(ndev->netdev, 0);
	kvx_eth_release_rx_res(ndev->netdev, 0);
	list_del(&ndev->node);

	return 0;
}

static void kvx_netdev_probe_hw_cv1(struct kvx_eth_hw *hw, struct kvx_eth_netdev *ndev)
{
	int i;

	for (i = 0; i < KVX_ETH_LANE_NB; i++)
		kvx_eth_lb_cv1_set_default(hw, i);
	kvx_eth_pfc_f_set_default(hw, &ndev->cfg);

	kvx_eth_fill_dispatch_table(hw, &ndev->cfg,
		    ndev->dma_cfg.rx_chan_id.start);
	kvx_eth_tx_fifo_cfg(hw, &ndev->cfg);

	for (i = 0; i < KVX_ETH_LANE_NB; i++)
		kvx_eth_lb_f_cfg(hw, &ndev->hw->lb_f[i]);
}

static void kvx_netdev_probe_hw_cv2(struct kvx_eth_hw *hw, struct kvx_eth_netdev *ndev)
{
	int i;

	kvx_eth_lb_cv2_set_default(hw, ndev->dma_cfg.rx_chan_id.start, ndev->dma_cfg.rx_cache_id);
	for (i = 0; i < KVX_ETH_PHYS_PARSER_NB_CV2; ++i)
		kvx_eth_parser_cv2_f_cfg(hw, &hw->parser_cv2_f[i]);
	for (i = 0; i < RX_LB_LUT_ARRAY_SIZE; ++i)
		kvx_eth_lut_entry_cv2_f_cfg(hw, &hw->lut_entry_cv2_f[i]);
}

/* kvx_netdev_probe() - Probe netdev
 * @pdev: Platform device
 *
 * Return: 0 on success, < 0 on failure
 */
static int kvx_netdev_probe(struct platform_device *pdev)
{
	struct device_node *np_dma, *np_dev = of_get_parent(pdev->dev.of_node);
	struct platform_device *ppdev = of_find_device_by_node(np_dev);
	struct kvx_eth_dev *dev = platform_get_drvdata(ppdev);
	struct kvx_eth_netdev *ndev = NULL;
	struct platform_device *dma_pdev;
	const struct kvx_eth_chip_rev_data *rev_d = dev->chip_rev_data;
	int ret = 0;

	/* Check dma noc probed and available */
	dma_pdev = kvx_eth_check_dma(pdev, &np_dma);
	if (!dma_pdev)
		return -ENODEV;

	/* Config DMA */
	ndev = kvx_eth_create_netdev(pdev, dev);
	if (!ndev) {
		dev_err(&pdev->dev, "Probe defer\n");
		ret = -EPROBE_DEFER;
		goto err;
	}

	platform_set_drvdata(pdev, ndev);
	ret = kvx_eth_init_netdev(ndev);
	if (ret)
		goto err;

	ret = rev_d->ethtx_credit_en_register(pdev);

	if (ret)
		goto err;

	/**
	 * MF 1.3 -> do *NOT* change the following settings
	 * Rx LB ctrl registers for lanes 0/2 must be set the same way
	 * Program all lane LB accordingly
	 */
	rev_d->netdev_probe_hw(&dev->hw, ndev);

	ret = rev_d->eth_netdev_sysfs_init(ndev);
	if (ret)
		netdev_warn(ndev->netdev, "Failed to initialize sysfs\n");
	dev_info(&pdev->dev, "KVX netdev[%d] probed\n", ndev->cfg.id);

	return 0;

err:
	if (ndev)
		kvx_eth_free_netdev(ndev);
	return ret;
}

/* kvx_netdev_remove() - Remove netdev
 * @pdev: Platform device
 *
 * Return: 0
 */
static int kvx_netdev_remove(struct platform_device *pdev)
{
	struct kvx_eth_netdev *ndev = platform_get_drvdata(pdev);
	struct device_node *np_dev = of_get_parent(pdev->dev.of_node);
	struct platform_device *ppdev = of_find_device_by_node(np_dev);
	struct kvx_eth_dev *dev = platform_get_drvdata(ppdev);
	const struct kvx_eth_chip_rev_data *rev_d = dev->chip_rev_data;
	int rtm;

	rev_d->ethtx_credit_en_unregister(pdev);
	rev_d->eth_netdev_sysfs_uninit(ndev);
	for (rtm = 0; rtm < RTM_NB; rtm++) {
		if (ndev->hw->rtm_params[rtm].rtm)
			put_device(&ndev->hw->rtm_params[rtm].rtm->dev);
	}
	if (netif_running(ndev->netdev))
		kvx_eth_netdev_stop(ndev->netdev);
	kvx_eth_free_netdev(ndev);

	return 0;
}

static const struct of_device_id kvx_netdev_match[] = {
	{ .compatible = "kalray,coolidge-net" },
	{ .compatible = "kalray,coolidge-v2-net" },
	{ }
};
MODULE_DEVICE_TABLE(of, kvx_netdev_match);

static struct platform_driver kvx_netdev_driver = {
	.probe = kvx_netdev_probe,
	.remove = kvx_netdev_remove,
	.driver = {
		.name = KVX_NETDEV_NAME,
		.of_match_table = kvx_netdev_match,
	},
};

static int kvx_eth_phy_fw_update(struct platform_device *pdev, const char *filename)
{
	struct kvx_eth_dev *dev = platform_get_drvdata(pdev);
	const struct firmware *fw = NULL;
	int ret = 0;

	if (!load_phy_fw)
		return -EINVAL;

	dev_info(&pdev->dev, "Requesting phy firmware %s\n", filename);

	ret = request_firmware(&fw, filename, &pdev->dev);
	if (ret < 0 || fw->size == 0) {
		fw = NULL;
		dev_warn(&pdev->dev, "Unable to load firmware %s\n",
			filename);
	}

	/* Update parameters according to probbed fw informations */
	ret = dev->chip_rev_data->kvx_phy_init_sequence(&dev->hw, fw);
	if (fw)
		release_firmware(fw);

	return ret;
}

const struct kvx_eth_chip_rev_data *kvx_eth_get_rev_data(struct kvx_eth_hw *hw)
{
	struct kvx_eth_dev *dev = KVX_HW2DEV(hw);

	return dev->chip_rev_data;
}

inline const struct kvx_eth_chip_rev_data *kvx_eth_get_rev_data_of_netdev(struct net_device *netdev)
{
	struct kvx_eth_netdev *ndev = netdev_priv(netdev);
	struct kvx_eth_hw *hw = ndev->hw;
	const struct kvx_eth_chip_rev_data *rev_d = kvx_eth_get_rev_data(hw);

	return rev_d;
}

/* kvx_eth_probe() - Probe generic device
 * @pdev: Platform device
 *
 * Return: 0 on success, < 0 on failure
 */
static int kvx_eth_probe(struct platform_device *pdev)
{
	struct kvx_eth_dev *dev;
	struct resource *res = NULL;
	struct kvx_eth_res *hw_res;
	int i, ret = 0;
	const char *res_name;
	const struct kvx_eth_chip_rev_data *rev_d;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENODEV;
	platform_set_drvdata(pdev, dev);
	dev->pdev = pdev;
	dev->type = &kvx_eth_data;
	dev->chip_rev_data = of_device_get_match_data(&pdev->dev);
	rev_d = dev->chip_rev_data;
	INIT_LIST_HEAD(&dev->list);
	mutex_init(&dev->hw.mac_reset_lock);
	mutex_init(&dev->hw.phy_serdes_reset_lock);
	mutex_init(&dev->hw.advertise_lock);
	spin_lock_init(&dev->hw.link_down_lock);

	if (of_machine_is_compatible("kalray,haps"))
		dev->type = &kvx_haps_data;

	for (i = 0; i < rev_d->num_res; ++i) {
		res_name = rev_d->kvx_eth_res_names[i];
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						   res_name);
		if (!res) {
			dev_err(&pdev->dev, "Failed to get resources\n");
			ret = -ENODEV;
			goto err;
		}
		hw_res = &dev->hw.res[i];
		hw_res->name = res_name;
		hw_res->base = devm_ioremap_resource(&pdev->dev, res);
		if (!hw_res->base) {
			dev_err(&pdev->dev, "Failed to map %s reg\n",
				hw_res->name);
			ret = PTR_ERR(hw_res->base);
			goto err;
		}
		dev_dbg(&pdev->dev, "map[%d] %s @ 0x%llx\n", i, hw_res->name,
			 (u64)hw_res->base);
	}

	ret = kvx_eth_dev_parse_dt(pdev, dev);
	if (ret)
		goto err;

	dev->hw.dev = &pdev->dev;

	if (dev->type->phy_init) {
		ret = dev->type->phy_init(&dev->hw, SPEED_UNKNOWN);
		if (ret) {
			dev_err(&pdev->dev, "Mac/Phy init failed (ret: %d)\n",
				ret);
			goto err;
		}
	}

	/* Try loading phy firmware */
	if (dev->type->phy_fw_update) {
		ret = dev->type->phy_fw_update(pdev, rev_d->fw_filename);
		if (ret && rev_d->phy_dynamic_global_reset)
			rev_d->phy_dynamic_global_reset(&dev->hw);
	}
	if (rev_d->eth_init_dispatch_table)
		rev_d->eth_init_dispatch_table(&dev->hw);
	rev_d->eth_tx_init(&dev->hw);
	kvx_eth_parsers_init(&dev->hw);
	kvx_eth_phy_f_init(&dev->hw);
	rev_d->eth_hw_sysfs_init(&dev->hw);

	dev_info(&pdev->dev, "KVX network driver\n");
	return devm_of_platform_populate(&pdev->dev);

err:
	platform_set_drvdata(pdev, NULL);
	return ret;
}

/* kvx_eth_remove() - Remove generic device
 * @pdev: Platform device
 *
 * Return: 0
 */
static int kvx_eth_remove(struct platform_device *pdev)
{
	struct kvx_eth_dev *dev = platform_get_drvdata(pdev);
	struct kvx_eth_netdev *ndev;

	list_for_each_entry(ndev, &dev->list, node)
		unregister_netdev(ndev->netdev);

	platform_set_drvdata(pdev, NULL);
	return 0;
}

static const struct kvx_eth_chip_rev_data eth_chip_rev_data_cv1 = {
	.revision = COOLIDGE_V1,
	.irq = false,
	.limited_parser_cap = true,
	.kvx_eth_res_names = kvx_eth_res_names_cv1,
	.num_res = KVX_ETH_NUM_RES_CV1,
	.fw_filename = KVX_PHY_FW_NAME_CV1,
	.kvx_phy_init_sequence = kvx_phy_init_sequence_cv1,
	.fill_ipv4_filter = fill_ipv4_filter_cv1,
	.default_mac_filter_param_pfc_etype = 1,
	.lnk_dwn_it_support = false,
	.mac_pfc_cfg = kvx_mac_pfc_cfg_cv1,
	.write_parser_ram_word = write_parser_ram_word_cv1,
	.parser_disable = parser_disable_cv1,
	.eth_init_netdev_hdw = kvx_eth_init_netdev_hdw_cv1,
	.kvx_eth_rx_hdr = kvx_eth_rx_hdr_cv1,
	.eth_fill_tx_hdr = kvx_eth_fill_tx_hdr_cv1,
	.eth_hw_change_mtu = kvx_eth_hw_change_mtu_cv1,
	.netdev_probe_hw = kvx_netdev_probe_hw_cv1,
	.eth_netdev_sysfs_init = kvx_eth_netdev_sysfs_init_cv1,
	.eth_netdev_sysfs_uninit = kvx_eth_netdev_sysfs_uninit_cv1,
	.eth_tx_init = kvx_eth_tx_init_cv1,
	.eth_hw_sysfs_init = kvx_eth_hw_sysfs_init_cv1,
	.parser_commit_filter = parser_commit_filter_cv1,
	.eth_add_dispatch_table_entry = kvx_eth_add_dispatch_table_entry_cv1,
	.eth_init_dispatch_table = kvx_eth_init_dispatch_table_cv1,
	.eth_mac_f_cfg = kvx_eth_mac_f_cfg_cv1,
	.ethtx_credit_en_register = kvx_ethtx_credit_en_register_cv1,
	.ethtx_credit_en_unregister = kvx_ethtx_credit_en_unregister_cv1,
	.kvx_ethtool_ops = &kvx_ethtool_cv1_ops,
	.kvx_net_dcb_is_pcp_enabled = &kvx_net_dcb_is_pcp_enabled_cv1,
	.kvx_net_dcb_get_pfc = &kvx_net_dcb_get_pfc_cv1,
	.kvx_net_dcb_set_pfc = &kvx_net_dcb_set_pfc_cv1,
	.phy_lane_rx_serdes_data_enable = &kvx_eth_phy_lane_rx_serdes_data_enable_cv1,
	.phy_enable_serdes = &kvx_mac_phy_enable_serdes_cv1,
	.phy_disable_serdes = &kvx_mac_phy_disable_serdes_cv1,
	.phy_pll_serdes_reconf = &kvx_eth_phy_pll_serdes_reconf_cv1,
	.phy_dynamic_global_reset = &kvx_phy_reset,
	.phy_set_vph_indication = &kvx_phy_refclk_cfg,
	.phy_dump_status = &kvx_eth_dump_phy_status,
	.phy_mac_10G_cfg = &kvx_phy_mac_10G_cfg,
	.phy_get_tx_eq_coef = &kvx_phy_get_tx_eq_coef_cv1,
	.phy_set_tx_eq_coef = &kvx_phy_set_tx_eq_coef_cv1,
	.phy_set_tx_default_eq_coef = &kvx_phy_set_tx_default_eq_coef_cv1,
	.phy_rx_adapt = &kvx_phy_rx_adapt_cv1,
	.phy_rx_adapt_broadcast = &kvx_phy_rx_adapt_broadcast_cv1,
	.phy_start_rx_adapt = &kvx_phy_start_rx_adapt_cv1,
	.phy_get_result_rx_adapt = &kvx_phy_get_result_rx_adapt_cv1,
	.phy_tx_ber_param_update = &kvx_phy_tx_ber_param_update_cv1,
	.phy_rx_ber_param_update = &kvx_phy_rx_ber_param_update_cv1,
	.phy_tx_bert_param_cfg = &kvx_phy_tx_bert_param_cfg_cv1,
	.phy_rx_bert_param_cfg = &kvx_phy_rx_bert_param_cfg_cv1,
	.update_parser_desc = &kvx_update_parser_desc,
};

static const struct kvx_eth_chip_rev_data eth_chip_rev_data_cv2 = {
	.revision = COOLIDGE_V2,
	.irq = true,
	.limited_parser_cap = false,
	.kvx_eth_res_names = kvx_eth_res_names_cv2,
	.num_res = KVX_ETH_NUM_RES_CV2,
	.fw_filename = KVX_PHY_FW_NAME_CV2,
	.default_mac_filter_param_pfc_etype = 0,
	.lnk_dwn_it_support = true,
	.kvx_phy_init_sequence = kvx_phy_init_sequence_cv2,
	.fill_ipv4_filter = fill_ipv4_filter_cv2,
	.mac_pfc_cfg = kvx_mac_pfc_cfg_cv2,
	.write_parser_ram_word = write_parser_ram_word_cv2,
	.parser_disable = parser_disable_cv2,
	.eth_init_netdev_hdw = kvx_eth_init_netdev_hdw_cv2,
	.kvx_eth_rx_hdr = kvx_eth_rx_hdr_cv2,
	.eth_fill_tx_hdr = kvx_eth_fill_tx_hdr_cv2,
	.eth_hw_change_mtu = kvx_eth_hw_change_mtu_cv2,
	.netdev_probe_hw = kvx_netdev_probe_hw_cv2,
	.eth_netdev_sysfs_init = kvx_eth_netdev_sysfs_init_cv2,
	.eth_netdev_sysfs_uninit = kvx_eth_netdev_sysfs_uninit_cv2,
	.eth_tx_init = kvx_eth_tx_init_cv2,
	.eth_hw_sysfs_init = kvx_eth_hw_sysfs_init_cv2,
	.parser_commit_filter = parser_commit_filter_cv2,
	.eth_mac_f_cfg = kvx_eth_mac_f_cfg_cv2,
	.ethtx_credit_en_register = kvx_ethtx_credit_en_register_cv2,
	.ethtx_credit_en_unregister = kvx_ethtx_credit_en_unregister_cv2,
	.kvx_ethtool_ops = &kvx_ethtool_cv2_ops,
	.kvx_net_dcb_is_pcp_enabled = &kvx_net_dcb_is_pcp_enabled_cv2,
	.kvx_net_dcb_get_pfc = &kvx_net_dcb_get_pfc_cv2,
	.kvx_net_dcb_set_pfc = &kvx_net_dcb_set_pfc_cv2,
	.phy_lane_rx_serdes_data_enable = &kvx_phy_lane_rx_serdes_data_enable_cv2,
	.phy_enable_serdes = &kvx_phy_enable_serdes_cv2,
	.phy_disable_serdes = &kvx_phy_disable_serdes_cv2,
	.phy_pll_serdes_reconf = NULL,
	.phy_dynamic_global_reset = NULL,
	.phy_set_vph_indication = NULL,
	.phy_dump_status = NULL,
	.phy_mac_10G_cfg = NULL,
	.phy_get_tx_eq_coef = &kvx_phy_get_tx_eq_coef_cv2,
	.phy_set_tx_eq_coef = &kvx_phy_set_tx_eq_coef_cv2,
	.phy_set_tx_default_eq_coef = &kvx_phy_set_tx_default_eq_coef_cv2,
	.phy_start_rx_adapt = &kvx_phy_start_rx_adapt_v2_cv2,
	.phy_get_result_rx_adapt = &kvx_phy_get_result_rx_adapt_v2_cv2,
	.phy_tx_ber_param_update = &kvx_phy_tx_ber_param_update_cv2,
	.phy_rx_ber_param_update = &kvx_phy_rx_ber_param_update_cv2,
	.phy_tx_bert_param_cfg = &kvx_phy_tx_bert_param_cfg_cv2,
	.phy_rx_bert_param_cfg = &kvx_phy_rx_bert_param_cfg_cv2,
	.update_parser_desc = &kvx_update_parser_desc_cv2,
};
static const struct of_device_id kvx_eth_match[] = {
	{ .compatible = "kalray,coolidge-eth", .data = &eth_chip_rev_data_cv1 },
	{ .compatible = "kalray,coolidge-v2-eth", .data = &eth_chip_rev_data_cv2 },
	{ },
};
MODULE_DEVICE_TABLE(of, kvx_eth_match);

static struct platform_driver kvx_eth_driver = {
	.probe = kvx_eth_probe,
	.remove = kvx_eth_remove,
	.driver = {
		.name = KVX_NET_DRIVER_NAME,
		.of_match_table = kvx_eth_match
	},
};

static struct platform_driver * const drivers[] = {
	&kvx_netdev_driver,
	&kvx_eth_driver,
};

static int kvx_eth_init(void)
{
	return platform_register_drivers(drivers, ARRAY_SIZE(drivers));
}
module_init(kvx_eth_init);

static void kvx_eth_exit(void)
{
	platform_unregister_drivers(drivers, ARRAY_SIZE(drivers));
}
module_exit(kvx_eth_exit);

/* kvx_eth_get_lut_indir() - Get LUT indirection
 * A LUT entry points to a dispatch entry (dt).
 * This dt entry is a route to a cluster_id / rx_channel pair.
 *
 * @netdev: Current netdev
 * @lut_id: LUT entry to request
 * @cluster_id: return value
 * @rx_channel: return value
 * @return: -1 if error, dispatch table entry otherwise
 */
int kvx_eth_get_lut_indir(struct net_device *netdev, u32 lut_id,
		u32 *cluster_id, u32 *rx_channel)
{
	struct kvx_eth_netdev *ndev;

	if (!netdev)
		return -EINVAL;
	ndev = netdev_priv(netdev);
	return kvx_eth_hw_get_lut_indir(ndev->hw, lut_id, cluster_id, rx_channel);
}
EXPORT_SYMBOL(kvx_eth_get_lut_indir);

MODULE_AUTHOR("Thomas Costis <tcostis@kalray.eu>");
MODULE_AUTHOR("Benjamin Mugnier <bmugnier@kalray.eu>");
MODULE_LICENSE("GPL");
