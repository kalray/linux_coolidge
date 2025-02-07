// SPDX-License-Identifier: GPL-2.0
/**
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2023 Kalray Inc.
 */

#include <linux/ethtool.h>
#include <linux/etherdevice.h>

#include "kvx-net-hdr-cv2.h"
#include "../kvx-ethtool.h"
#include "../kvx-net.h"

void fill_ipv4_filter_cv2(struct kvx_eth_netdev *ndev,
		struct ethtool_rx_flow_spec *fs, union filter_desc *flt,
		int ptype_ovrd)
{
	union ipv4_cv2_filter_desc *filter = &flt->cv2_ipv4;
	struct ethtool_usrip4_spec *l3_val  = &fs->h_u.usr_ip4_spec;
	struct ethtool_usrip4_spec *l3_mask  = &fs->m_u.usr_ip4_spec;
	u8 ptype_rule = l3_val->proto;
	u8 ptype = 0;
	int src_ip = ntohl(l3_val->ip4src);
	int src_mask = ntohl(l3_mask->ip4src);
	int dst_ip = ntohl(l3_val->ip4dst);
	int dst_mask = ntohl(l3_mask->ip4dst);
	int tt = flow_type_to_traffic_type(fs->flow_type);
	u8 rx_hash_field;

	memcpy(filter, &ipv4_cv2_filter_default, sizeof(ipv4_cv2_filter_default));

	if (src_mask != 0) {
		filter->sa = src_ip;
		filter->sa_mask = src_mask;
	}

	if (dst_mask != 0) {
		filter->da = dst_ip;
		filter->da_mask = dst_mask;
	}

	if (ptype_ovrd != 0)
		ptype = ptype_ovrd;
	else if (ptype_rule != 0)
		ptype = ptype_rule;

	if (ptype != 0) {
		filter->protocol = ptype;
		filter->protocol_mask = 0xff;
	}

	if (tt == KVX_TT_IP4) {
		netdev_info(ndev->netdev, "Force src/dst hashing for IP4 only rule\n");
		filter->sa_fk_mask = 0xffffffff;
		filter->da_fk_mask = 0xffffffff;
	} else if (traffic_type_is_supported(tt)) {
		rx_hash_field = ndev->hw->parsing.rx_hash_fields[tt];
		if ((rx_hash_field & KVX_HASH_FIELD_SEL_SRC_IP) != 0)
			filter->sa_fk_mask = 0xffffffff;
		if ((rx_hash_field & KVX_HASH_FIELD_SEL_DST_IP) != 0)
			filter->da_fk_mask = 0xffffffff;
		if ((rx_hash_field & KVX_HASH_FIELD_SEL_L3_PROT) != 0)
			filter->protocol_fk_mask = 0xff;
	}

}

void kvx_eth_get_pauseparam_cv2(struct net_device *netdev,
			    struct ethtool_pauseparam *pause)
{
	struct kvx_eth_netdev *ndev = netdev_priv(netdev);
	struct kvx_eth_rx_dlv_pfc_f *rx_dlv_pfc_f = &ndev->hw->rx_dlv_pfc_f[ndev->cfg.id];
	struct kvx_eth_tx_pfc_f *tx_pfc_f = &ndev->hw->tx_pfc_f[ndev->cfg.id];

	pause->rx_pause = !!(rx_dlv_pfc_f->glb_pause_rx_en);
	pause->tx_pause = !!(tx_pfc_f->glb_pause_tx_en);
}

int kvx_eth_set_pauseparam_cv2(struct net_device *netdev,
			   struct ethtool_pauseparam *pause)
{
	struct kvx_eth_netdev *ndev = netdev_priv(netdev);
	struct kvx_eth_rx_dlv_pfc_f *rx_dlv_pfc_f = &ndev->hw->rx_dlv_pfc_f[ndev->cfg.id];
	struct kvx_eth_tx_pfc_f *tx_pfc_f = &ndev->hw->tx_pfc_f[ndev->cfg.id];

	if (rx_dlv_pfc_f->glb_pause_rx_en != pause->rx_pause) {
		rx_dlv_pfc_f->glb_pause_rx_en = pause->rx_pause;
		kvx_eth_rx_dlv_pfc_f_cfg(ndev->hw, rx_dlv_pfc_f);
	}
	if (tx_pfc_f->glb_pause_tx_en != pause->tx_pause) {
		tx_pfc_f->glb_pause_tx_en = pause->tx_pause;
		kvx_eth_tx_pfc_f_cfg(ndev->hw, tx_pfc_f);
	}
	return 0;
}
