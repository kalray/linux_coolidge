// SPDX-License-Identifier: GPL-2.0
/**
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2017-2023 Kalray Inc.
 */

#include <linux/module.h>

#include "../kvx-net.h"

DECLARE_SYSFS_ENTRY(lb_f);

FIELD_RW_ENTRY(lb_f, default_dispatch_policy,
	       0, DEFAULT_DISPATCH_POLICY_NB);
FIELD_RW_ENTRY(lb_f, keep_all_crc_error_pkt, 0, 1);
FIELD_RW_ENTRY(lb_f, store_and_forward, 0, 1);
FIELD_RW_ENTRY(lb_f, add_header, 0, 1);
FIELD_RW_ENTRY(lb_f, add_footer, 0, 1);
FIELD_R_ENTRY(lb_f, drop_mtu_cnt, 0, U32_MAX);
FIELD_R_ENTRY(lb_f, drop_fcs_cnt, 0, U32_MAX);
FIELD_R_ENTRY(lb_f, drop_crc_cnt, 0, U32_MAX);
FIELD_R_ENTRY(lb_f, drop_rule_cnt, 0, U32_MAX);
FIELD_R_ENTRY(lb_f, drop_fifo_overflow_cnt, 0, U32_MAX);
FIELD_R_ENTRY(lb_f, drop_total_cnt, 0, U32_MAX);
FIELD_R_ENTRY(lb_f, default_hit_cnt, 0, U32_MAX);
FIELD_R_ENTRY(lb_f, global_drop_cnt, 0, U32_MAX);
FIELD_R_ENTRY(lb_f, global_no_pfc_drop_cnt, 0, U32_MAX);
FIELD_RW_ENTRY(lb_f, pfc_handling_by_quanta, 0, 1);
FIELD_RW_ENTRY(lb_f, global_pfc_config, 0, 1);


static struct attribute *lb_f_attrs[] = {
	&lb_f_default_dispatch_policy_attr.attr,
	&lb_f_keep_all_crc_error_pkt_attr.attr,
	&lb_f_store_and_forward_attr.attr,
	&lb_f_add_header_attr.attr,
	&lb_f_add_footer_attr.attr,
	&lb_f_drop_mtu_cnt_attr.attr,
	&lb_f_drop_fcs_cnt_attr.attr,
	&lb_f_drop_crc_cnt_attr.attr,
	&lb_f_drop_rule_cnt_attr.attr,
	&lb_f_drop_fifo_overflow_cnt_attr.attr,
	&lb_f_drop_total_cnt_attr.attr,
	&lb_f_default_hit_cnt_attr.attr,
	&lb_f_global_drop_cnt_attr.attr,
	&lb_f_global_no_pfc_drop_cnt_attr.attr,
	&lb_f_pfc_handling_by_quanta_attr.attr,
	&lb_f_global_pfc_config_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(lb_f);
SYSFS_TYPES(lb_f);

DECLARE_SYSFS_ENTRY(rx_noc);
FIELD_RW_ENTRY(rx_noc, vchan0_pps_timer, 0, U16_MAX);
FIELD_RW_ENTRY(rx_noc, vchan0_payload_flit_nb, 0, 0xF);
FIELD_RW_ENTRY(rx_noc, vchan1_pps_timer, 0, U16_MAX);
FIELD_RW_ENTRY(rx_noc, vchan1_payload_flit_nb, 0, 0xF);

static struct attribute *rx_noc_attrs[] = {
	&rx_noc_vchan0_pps_timer_attr.attr,
	&rx_noc_vchan0_payload_flit_nb_attr.attr,
	&rx_noc_vchan1_pps_timer_attr.attr,
	&rx_noc_vchan1_payload_flit_nb_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(rx_noc);
SYSFS_TYPES(rx_noc);


DECLARE_SYSFS_ENTRY(lut_f);
FIELD_RW_ENTRY(lut_f, qpn_enable, 0, RX_LB_LUT_QPN_CTRL_QPN_EN_MASK);
FIELD_RW_ENTRY(lut_f, lane_enable, 0, 1);
FIELD_RW_ENTRY(lut_f, rule_enable, 0, 1);
FIELD_RW_ENTRY(lut_f, pfc_enable, 0, 1);

static struct attribute *lut_f_attrs[] = {
	&lut_f_qpn_enable_attr.attr,
	&lut_f_lane_enable_attr.attr,
	&lut_f_rule_enable_attr.attr,
	&lut_f_pfc_enable_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(lut_f);
SYSFS_TYPES(lut_f);

DECLARE_SYSFS_ENTRY(pfc_f);
FIELD_RW_ENTRY(pfc_f, global_release_level, 0,
	       RX_PFC_LANE_GLOBAL_DROP_LEVEL_MASK);
FIELD_RW_ENTRY(pfc_f, global_drop_level, 0, RX_PFC_LANE_GLOBAL_DROP_LEVEL_MASK);
FIELD_RW_ENTRY(pfc_f, global_alert_level, 0,
	       RX_PFC_LANE_GLOBAL_DROP_LEVEL_MASK);
FIELD_RW_ENTRY(pfc_f, global_pfc_en, 0, 1);
FIELD_RW_ENTRY(pfc_f, global_pause_en, 0, 1);
FIELD_R_ENTRY(pfc_f, pause_req_cnt, 0, U32_MAX);
FIELD_R_ENTRY(pfc_f, global_wmark, 0, U32_MAX);
FIELD_R_ENTRY(pfc_f, global_no_pfc_wmark, 0, U32_MAX);

static struct attribute *pfc_f_attrs[] = {
	&pfc_f_global_release_level_attr.attr,
	&pfc_f_global_drop_level_attr.attr,
	&pfc_f_global_alert_level_attr.attr,
	&pfc_f_global_pfc_en_attr.attr,
	&pfc_f_global_pause_en_attr.attr,
	&pfc_f_pause_req_cnt_attr.attr,
	&pfc_f_global_wmark_attr.attr,
	&pfc_f_global_no_pfc_wmark_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(pfc_f);
SYSFS_TYPES(pfc_f);

DECLARE_SYSFS_ENTRY(lut_entry_cv1_f);
FIELD_RW_ENTRY(lut_entry_cv1_f, dt_id, 0, RX_DISPATCH_TABLE_ENTRY_ARRAY_SIZE);
static struct attribute *lut_entry_cv1_f_attrs[] = {
	&lut_entry_cv1_f_dt_id_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(lut_entry_cv1_f);
SYSFS_TYPES(lut_entry_cv1_f);
DECLARE_SYSFS_ENTRY(tx_f);
FIELD_R_ENTRY(tx_f, header_en, 0, 1);
FIELD_R_ENTRY(tx_f, crc_en, 0, 1);
FIELD_RW_ENTRY(tx_f, drop_en, 0, 1);
FIELD_RW_ENTRY(tx_f, nocx_en, 0, 1);
FIELD_RW_ENTRY(tx_f, nocx_pack_en, 0, 1);
FIELD_RW_ENTRY(tx_f, pfc_en, 0, 0xFF);
FIELD_RW_ENTRY(tx_f, pause_en, 0, 1);
FIELD_RW_ENTRY(tx_f, rr_trigger, 0, 0xF);
FIELD_RW_ENTRY(tx_f, lane_id, 0, KVX_ETH_LANE_NB - 1);
FIELD_R_ENTRY(tx_f, drop_cnt, 0, U32_MAX);
FIELD_R_ENTRY(tx_f, fifo_level, 0, U32_MAX);
FIELD_R_ENTRY(tx_f, xoff, 0, 1);

static struct attribute *tx_f_attrs[] = {
	&tx_f_header_en_attr.attr,
	&tx_f_crc_en_attr.attr,
	&tx_f_drop_en_attr.attr,
	&tx_f_nocx_en_attr.attr,
	&tx_f_nocx_pack_en_attr.attr,
	&tx_f_pfc_en_attr.attr,
	&tx_f_pause_en_attr.attr,
	&tx_f_rr_trigger_attr.attr,
	&tx_f_lane_id_attr.attr,
	&tx_f_drop_cnt_attr.attr,
	&tx_f_fifo_level_attr.attr,
	&tx_f_xoff_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(tx_f);
SYSFS_TYPES(tx_f);

DECLARE_SYSFS_ENTRY(tx_noc_f);
FIELD_R_ENTRY(tx_noc_f, fifo_level, 0, U32_MAX);
FIELD_R_ENTRY(tx_noc_f, parity_err, 0, U32_MAX);
FIELD_R_ENTRY(tx_noc_f, crc_err, 0, U32_MAX);
FIELD_R_ENTRY(tx_noc_f, perm_err, 0, U32_MAX);
FIELD_R_ENTRY(tx_noc_f, fifo_err, 0, U32_MAX);
FIELD_R_ENTRY(tx_noc_f, pkt_drop, 0, U32_MAX);

static struct attribute *tx_noc_f_attrs[] = {
	&tx_noc_f_fifo_level_attr.attr,
	&tx_noc_f_parity_err_attr.attr,
	&tx_noc_f_crc_err_attr.attr,
	&tx_noc_f_perm_err_attr.attr,
	&tx_noc_f_fifo_err_attr.attr,
	&tx_noc_f_pkt_drop_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(tx_noc_f);
SYSFS_TYPES(tx_noc_f);

DECLARE_SYSFS_ENTRY(cl_f);
FIELD_RW_ENTRY(cl_f, quanta, 0, U16_MAX);
FIELD_RW_ENTRY(cl_f, quanta_thres, 0, U16_MAX);
FIELD_RW_ENTRY(cl_f, release_level, 0, RX_PFC_LANE_GLOBAL_DROP_LEVEL_MASK);
FIELD_RW_ENTRY(cl_f, drop_level, 0, RX_PFC_LANE_GLOBAL_DROP_LEVEL_MASK);
FIELD_RW_ENTRY(cl_f, alert_level, 0, RX_PFC_LANE_GLOBAL_DROP_LEVEL_MASK);
FIELD_RW_ENTRY(cl_f, pfc_ena, 0, 1);
FIELD_R_ENTRY(cl_f,  pfc_req_cnt, 0, U32_MAX);
FIELD_R_ENTRY(cl_f,  drop_cnt, 0, U32_MAX);

static struct attribute *cl_f_attrs[] = {
	&cl_f_quanta_attr.attr,
	&cl_f_quanta_thres_attr.attr,
	&cl_f_release_level_attr.attr,
	&cl_f_drop_level_attr.attr,
	&cl_f_alert_level_attr.attr,
	&cl_f_pfc_ena_attr.attr,
	&cl_f_pfc_req_cnt_attr.attr,
	&cl_f_drop_cnt_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(cl_f);
SYSFS_TYPES(cl_f);

DECLARE_SYSFS_ENTRY(dt_f);
FIELD_RW_ENTRY(dt_f, cluster_id, 0, 0xFF);
FIELD_RW_ENTRY(dt_f, rx_channel, 0, KVX_ETH_RX_TAG_NB - 1);
FIELD_RW_ENTRY(dt_f, split_trigger, 0, 0x7F);
FIELD_RW_ENTRY(dt_f, vchan, 0, 1);

static struct attribute *dt_f_attrs[] = {
	&dt_f_cluster_id_attr.attr,
	&dt_f_rx_channel_attr.attr,
	&dt_f_split_trigger_attr.attr,
	&dt_f_vchan_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(dt_f);
SYSFS_TYPES(dt_f);

DECLARE_SYSFS_ENTRY(dt_acc_f);
FIELD_R_STRING_ENTRY(dt_acc_f, weights, 0, 0);
FIELD_W_ENTRY(dt_acc_f, reset, 1, 1);

static struct attribute *dt_acc_f_attrs[] = {
	&dt_acc_f_weights_attr.attr,
	&dt_acc_f_reset_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(dt_acc_f);
SYSFS_TYPES(dt_acc_f);

DECLARE_SYSFS_ENTRY(parser_f);

FIELD_R_ENTRY(parser_f, enable, 0, 1);
FIELD_R_ENTRY(parser_f, hit_cnt, 0, U32_MAX);
FIELD_R_ENTRY(parser_f, running, 0, 1);
FIELD_R_ENTRY(parser_f, fifo_overflow, 0, 1);
FIELD_R_STRING_ENTRY(parser_f, desc, 0, 0);
static struct attribute *parser_f_attrs[] = {
	&parser_f_enable_attr.attr,
	&parser_f_desc_attr.attr,
	&parser_f_hit_cnt_attr.attr,
	&parser_f_running_attr.attr,
	&parser_f_fifo_overflow_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(parser_f);
SYSFS_TYPES(parser_f);

DECLARE_SYSFS_ENTRY(rule_f);
FIELD_R_ENTRY(rule_f, enable, 0, 1);
FIELD_R_ENTRY(rule_f, type, 0, 0x1F);
FIELD_R_ENTRY(rule_f, add_metadata_index, 0, 1);
FIELD_R_ENTRY(rule_f, check_header_checksum, 0, 1);

static struct attribute *rule_f_attrs[] = {
	&rule_f_enable_attr.attr,
	&rule_f_type_attr.attr,
	&rule_f_add_metadata_index_attr.attr,
	&rule_f_check_header_checksum_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(rule_f);
SYSFS_TYPES(rule_f);

static struct kset *lut_entry_cv1_kset;
static struct kset *lb_kset;
static struct kset *rx_noc_kset;
static struct kset *tx_kset;
static struct kset *tx_noc_kset;
static struct kset *dt_kset;
static struct kset *parser_kset;
static struct kset *rule_kset;
static struct kset *pfc_cl_kset;

kvx_declare_kset(lut_entry_cv1_f, "lut_entries")
kvx_declare_kset(lb_f, "lb")
kvx_declare_kset(rx_noc, "rx_noc")
kvx_declare_kset(tx_f, "tx")
kvx_declare_kset(tx_noc_f, "tx_noc")
kvx_declare_kset(cl_f, "pfc_cl")
kvx_declare_kset(dt_f, "dispatch_table")
kvx_declare_kset(parser_f, "parser")
kvx_declare_kset(rule_f, "rule")

int kvx_eth_hw_sysfs_init_cv1(struct kvx_eth_hw *hw)
{
	int i, j, ret = 0;

	ret = kvx_eth_hw_sysfs_init(hw);
	for (i = 0; i < RX_LB_LUT_ARRAY_SIZE; i++)
		kobject_init(&hw->lut_entry_cv1_f[i].kobj, &lut_entry_cv1_f_ktype);

	kobject_init(&hw->lut_f.kobj, &lut_f_ktype);

	for (i = 0; i < KVX_ETH_LANE_NB; i++) {
		kobject_init(&hw->lb_f[i].kobj, &lb_f_ktype);
		kobject_init(&hw->lb_f[i].pfc_f.kobj, &pfc_f_ktype);
		for (j = 0; j < KVX_ETH_PFC_CLASS_NB; j++)
			kobject_init(&hw->lb_f[i].cl_f[j].kobj,
					&cl_f_ktype);
		for (j = 0; j < NB_CLUSTER; j++)
			kobject_init(&hw->lb_f[i].rx_noc[j].kobj,
					&rx_noc_ktype);
	}
	for (i = 0; i < TX_FIFO_NB; i++)
		kobject_init(&hw->tx_f[i].kobj, &tx_f_ktype);
	for (j = 0; j < NB_CLUSTER; j++)
		kobject_init(&hw->tx_noc_f[j].kobj, &tx_noc_f_ktype);

	for (i = 0; i < RX_DISPATCH_TABLE_ENTRY_ARRAY_SIZE; i++)
		kobject_init(&hw->dt_f[i].kobj, &dt_f_ktype);
	kobject_init(&hw->dt_acc_f.kobj, &dt_acc_f_ktype);

	for (i = 0; i < ARRAY_SIZE(hw->parser_f); i++) {
		kobject_init(&hw->parser_f[i].kobj, &parser_f_ktype);
		for (j = 0; j < KVX_NET_LAYER_NB; j++) {
			kobject_init(&hw->parser_f[i].rules[j].kobj,
					&rule_f_ktype);
		}
	}

	return ret;
}

int kvx_eth_netdev_sysfs_init_cv1(struct kvx_eth_netdev *ndev)
{
	struct kvx_eth_hw *hw = ndev->hw;
	int lane_id = ndev->cfg.id;
	int p, ret = 0;

	ret = kvx_eth_netdev_sysfs_init(ndev);
	if (ret)
		goto err;

	ret = kobject_add(&hw->lut_f.kobj, &ndev->netdev->dev.kobj, "lut");
	if (ret)
		goto err;

	ret = kvx_kset_lut_entry_cv1_f_create(ndev, &ndev->netdev->dev.kobj, lut_entry_cv1_kset,
			&hw->lut_entry_cv1_f[0], RX_LB_LUT_ARRAY_SIZE);
	if (ret)
		goto err;
	ret = kobject_add(&hw->dt_acc_f.kobj, &ndev->netdev->dev.kobj, "dispatch_table_acc");
	if (ret)
		goto err;

	ret = kvx_kset_lb_f_create(ndev, &ndev->netdev->dev.kobj, lb_kset,
				   &hw->lb_f[lane_id], 1);
	if (ret)
		goto err;

	ret = kobject_add(&hw->lb_f[lane_id].pfc_f.kobj,
			  &hw->lb_f[lane_id].kobj, "pfc");
	if (ret)
		goto err;

	ret = kvx_kset_rx_noc_create(ndev, &hw->lb_f[lane_id].kobj,
				rx_noc_kset, &hw->lb_f[lane_id].rx_noc[0],
				NB_CLUSTER);
	if (ret)
		goto err;

	ret = kvx_kset_cl_f_create(ndev, &hw->lb_f[lane_id].kobj, pfc_cl_kset,
			   &hw->lb_f[lane_id].cl_f[0], KVX_ETH_PFC_CLASS_NB);
	if (ret)
		goto err;

	ret = kvx_kset_tx_f_create(ndev, &ndev->netdev->dev.kobj, tx_kset,
				   &hw->tx_f[0], TX_FIFO_NB);
	if (ret)
		goto err;

	ret = kvx_kset_tx_noc_f_create(ndev, &ndev->netdev->dev.kobj, tx_noc_kset,
				   &hw->tx_noc_f[0], NB_CLUSTER);
	if (ret)
		goto err;

	ret = kvx_kset_dt_f_create(ndev, &ndev->netdev->dev.kobj, dt_kset,
			&hw->dt_f[0], RX_DISPATCH_TABLE_ENTRY_ARRAY_SIZE);
	if (ret)
		goto err;

	ret = kvx_kset_parser_f_create(ndev, &ndev->netdev->dev.kobj,
			parser_kset, &hw->parser_f[0], ARRAY_SIZE(hw->parser_f));
	if (ret)
		goto err;
	for (p = 0; p < ARRAY_SIZE(hw->parser_f); p++) {
		ret = kvx_kset_rule_f_create(ndev, &ndev->hw->parser_f[p].kobj,
				rule_kset, &hw->parser_f[p].rules[0],
				KVX_NET_LAYER_NB);
		if (ret)
			goto err;
	}
	return ret;

err:
	kobject_del(&ndev->hw->lb_f[lane_id].pfc_f.kobj);
	kobject_put(&ndev->hw->lb_f[lane_id].pfc_f.kobj);
	kobject_del(&ndev->hw->lut_f.kobj);
	kobject_put(&ndev->hw->lut_f.kobj);
	kobject_del(&ndev->hw->dt_acc_f.kobj);
	kobject_put(&ndev->hw->dt_acc_f.kobj);
	return ret;
}

void kvx_eth_netdev_sysfs_uninit_cv1(struct kvx_eth_netdev *ndev)
{
	int i;

	kvx_eth_netdev_sysfs_uninit(ndev);

	kvx_kset_lut_entry_cv1_f_remove(ndev, lut_entry_cv1_kset, &ndev->hw->lut_entry_cv1_f[0],
			RX_LB_LUT_ARRAY_SIZE);
	kvx_kset_dt_f_remove(ndev, dt_kset, &ndev->hw->dt_f[0],
			RX_DISPATCH_TABLE_ENTRY_ARRAY_SIZE);
	kvx_kset_tx_noc_f_remove(ndev, tx_noc_kset, &ndev->hw->tx_noc_f[0],
				 NB_CLUSTER);
	kvx_kset_tx_f_remove(ndev, tx_kset, &ndev->hw->tx_f[0], TX_FIFO_NB);
	for (i = 0; i < KVX_ETH_LANE_NB; i++) {
		kvx_kset_rx_noc_remove(ndev, rx_noc_kset,
				&ndev->hw->lb_f[i].rx_noc[0], NB_CLUSTER);
		kvx_kset_cl_f_remove(ndev, pfc_cl_kset,
			     &ndev->hw->lb_f[i].cl_f[0], KVX_ETH_PFC_CLASS_NB);
		kobject_del(&ndev->hw->lb_f[i].pfc_f.kobj);
		kobject_put(&ndev->hw->lb_f[i].pfc_f.kobj);
	}
	kvx_kset_lb_f_remove(ndev, lb_kset, &ndev->hw->lb_f[0],
			KVX_ETH_LANE_NB);
	for (i = 0; i < ARRAY_SIZE(ndev->hw->parser_f); i++)
		kvx_kset_rule_f_remove(ndev, rule_kset,
				&ndev->hw->parser_f[i].rules[0],
				KVX_NET_LAYER_NB);
	kvx_kset_parser_f_remove(ndev, parser_kset, &ndev->hw->parser_f[0],
			ARRAY_SIZE(ndev->hw->parser_f));

	kobject_del(&ndev->hw->lut_f.kobj);
	kobject_put(&ndev->hw->lut_f.kobj);
	kobject_del(&ndev->hw->dt_acc_f.kobj);
	kobject_put(&ndev->hw->dt_acc_f.kobj);
}
