/* packet-bmp.c
 * Routines for BMP packet dissection
 * (c) Copyright Ebben Aries <exa@fb.com>
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
/*
 * Supports:
 * draft-ietf-grow-bmp-07 BGP Monitoring Protocol
 *
 */

#include "config.h"

#include <epan/packet.h>
#include <epan/prefs.h>

#include "packet-tcp.h"
#include "packet-bgp.h"

void proto_register_bmp(void);
void proto_reg_handoff_bmp(void);

#define FRAME_HEADER_LEN                5

/* BMP Common Header Message Types */
#define BMP_MSG_TYPE_ROUTE_MONITORING   0x00    /* Route Monitoring */
#define BMP_MSG_TYPE_STAT_REPORT        0x01    /* Statistics Report */
#define BMP_MSG_TYPE_PEER_DOWN          0x02    /* Peer Down Notification */
#define BMP_MSG_TYPE_PEER_UP            0x03    /* Peer Up Notification */
#define BMP_MSG_TYPE_INIT               0x04    /* Initiation Message */
#define BMP_MSG_TYPE_TERM               0x05    /* Termination Message */
#define BMP_MSG_TYPE_ROUTE_MIRRORING    0x06    /* Route Mirroring */
#define BMP_MSG_TYPE__SIZE              0x07

/* BMP Initiation Message Types */
#define BMP_INIT_INFO_STRING            0x00    /* String */
#define BMP_INIT_SYSTEM_DESCRIPTION     0x01    /* sysDescr */
#define BMP_INIT_SYSTEM_NAME            0x02    /* sysName  */

/* BMP Per Peer Types */
#define BMP_PEER_GLOBAL_INSTANCE        0x00    /* Global Instance Peer */
#define BMP_PEER_L3VPN_INSTANCE         0x01    /* L3VPN Instance Peer */

/* BMP Per Peer Header Flags */
#define BMP_PEER_FLAG_IPV6              0x80    /* V Flag: IPv6 */
#define BMP_PEER_FLAG_POST_POLICY       0x40    /* L Flag: Post-policy */
#define BMP_PEER_FLAG_AS2               0x20    /* A Flag: legacy AS2 */
#define BMP_PEER_FLAG_RES               0x1F    /* Reserved */
#define BMP_PEER_FLAG_MASK              0xFF

/* BMP Stat Types */
#define BMP_STAT_PREFIX_REJ             0x00    /* Number of prefixes rejected by inbound policy */
#define BMP_STAT_PREFIX_DUP             0x01    /* Number of (known) duplicate prefix advertisements */
#define BMP_STAT_WITHDRAW_DUP           0x02    /* Number of (known) duplicate withdraws */
#define BMP_STAT_CLUSTER_LOOP           0x03    /* Number of updates invalidated due to CLUSTER_LIST loop */
#define BMP_STAT_AS_LOOP                0x04    /* Number of updates invalidated due to AS_PATH loop */
#define BMP_STAT_INV_ORIGINATOR         0x05    /* Number of updates invalidated due to ORIGINATOR_ID loop */
#define BMP_STAT_AS_CONFED_LOOP         0x06    /* Number of updates invalidated due to AS_CONFED loop */
#define BMP_STAT_ROUTES_ADJ_RIB_IN      0x07    /* Number of routes in Adj-RIBs-In */
#define BMP_STAT_ROUTES_LOC_RIB         0x08    /* Number of routes in Loc-RIB */

/* BMP Peer Down Reason Codes */
#define BMP_PEER_DOWN_LOCAL_NOTIFY      0x1     /* Local system closed the session with notification */
#define BMP_PEER_DOWN_LOCAL_NO_NOTIFY   0x2     /* Local system closed the session with FSM code */
#define BMP_PEER_DOWN_REMOTE_NOTIFY     0x3     /* Remote system closed the session with notification */
#define BMP_PEER_DOWN_REMOTE_NO_NOTIFY  0x4     /* Remote system closed the session without notification */

/* BMP Termination Message Types */
#define BMP_TERM_TYPE_STRING            0x00    /* String */
#define BMP_TERM_TYPE_REASON            0x01    /* Reason */

/* BMP Termination Reason Codes */
#define BMP_TERM_REASON_ADMIN_CLOSE     0x00    /* Session administratively closed */
#define BMP_TERM_REASON_UNSPECIFIED     0x01    /* Unspecified reason */
#define BMP_TERM_REASON_RESOURCES       0x02    /* Out of resources */
#define BMP_TERM_REASON_REDUNDANT       0x03    /* Redundant connection */

/* BMP Mirroring Information Codes */
#define BMP_MIRROR_CODE_ERRORPDU        0x00    /* Errored PDU */
#define BMP_MIRROR_CODE_MSGLOST         0x01    /* Lost Route Mirroring messages */

static const value_string bmp_typevals[] = {
    { BMP_MSG_TYPE_ROUTE_MONITORING,    "Route Monitoring" },
    { BMP_MSG_TYPE_STAT_REPORT,         "Statistics Report" },
    { BMP_MSG_TYPE_PEER_DOWN,           "Peer Down Notification" },
    { BMP_MSG_TYPE_PEER_UP,             "Peer Up Notification" },
    { BMP_MSG_TYPE_INIT,                "Initiation Message" },
    { BMP_MSG_TYPE_TERM,                "Termination Message" },
    { BMP_MSG_TYPE_ROUTE_MIRRORING,     "Route Mirroring" },
    { 0, NULL }
};

static const value_string init_typevals[] = {
    { BMP_INIT_INFO_STRING,             "String" },
    { BMP_INIT_SYSTEM_DESCRIPTION,      "sysDescr" },
    { BMP_INIT_SYSTEM_NAME,             "sysName" },
    { 0, NULL }
};

static const value_string peer_typevals[] = {
    { BMP_PEER_GLOBAL_INSTANCE,         "Global Instance Peer" },
    { BMP_PEER_L3VPN_INSTANCE,          "L3VPN Instance Peer" },
    { 0, NULL }
};

static const value_string down_reason_typevals[] = {
    { BMP_PEER_DOWN_LOCAL_NOTIFY,       "Local System, Notification" },
    { BMP_PEER_DOWN_LOCAL_NO_NOTIFY,    "Local System, No Notification" },
    { BMP_PEER_DOWN_REMOTE_NOTIFY,      "Remote System, Notification" },
    { BMP_PEER_DOWN_REMOTE_NO_NOTIFY,   "Remote System, No Notification" },
    { 0, NULL }
};

static const value_string term_typevals[] = {
    { BMP_TERM_TYPE_STRING,             "String" },
    { BMP_TERM_TYPE_REASON,             "Reason" },
    { 0, NULL }
};

static const value_string term_reason_typevals[] = {
    { BMP_TERM_REASON_ADMIN_CLOSE,      "Session administratively closed" },
    { BMP_TERM_REASON_UNSPECIFIED,      "Unspecified reason" },
    { BMP_TERM_REASON_RESOURCES,        "Out of resources" },
    { BMP_TERM_REASON_REDUNDANT,        "Redundant connection" },
    { 0, NULL }
};

static const value_string stat_typevals[] = {
    { BMP_STAT_PREFIX_REJ,              "Rejected Prefixes" },
    { BMP_STAT_PREFIX_DUP,              "Duplicate Prefixes" },
    { BMP_STAT_WITHDRAW_DUP,            "Duplicate Withdraws" },
    { BMP_STAT_CLUSTER_LOOP,            "Invalid CLUSTER_LIST Loop" },
    { BMP_STAT_AS_LOOP,                 "Invalid AS_PATH Loop" },
    { BMP_STAT_INV_ORIGINATOR,          "Invalid ORIGINATOR_ID" },
    { BMP_STAT_AS_CONFED_LOOP,          "Invalid AS_CONFED Loop" },
    { BMP_STAT_ROUTES_ADJ_RIB_IN,       "Routes in Adj-RIB-In" },
    { BMP_STAT_ROUTES_LOC_RIB,          "Routes in Loc-RIB" },
    { 0, NULL }
};

static const value_string mirror_typevals[] = {
    { 0,                                "BGP Message" },
    { 1,                                "Information" },
    { 0, NULL }
};

static const value_string mirror_codevals[] = {
    { BMP_MIRROR_CODE_ERRORPDU,         "Errored PDU" },
    { BMP_MIRROR_CODE_MSGLOST,          "Mirroring messages lost" },
    { 0, NULL }
};

static int proto_bmp = -1;

/* BMP Common Header filed */
static int hf_bmp_version = -1;
static int hf_bmp_length = -1;
static int hf_bmp_type = -1;

/* BMP Unused Bytes filed */
static int hf_bmp_unused = -1;

/* BMP Initiation Header filed */
static int hf_init_types = -1;
static int hf_init_type = -1;
static int hf_init_length = -1;
static int hf_init_info = -1;

/* BMP Per Peer Header filed */
static int hf_peer_header = -1;
static int hf_peer_type = -1;
static int hf_peer_flags = -1;
static int hf_peer_flags_ipv6 = -1;
static int hf_peer_flags_post_policy = -1;
static int hf_peer_flags_as2 = -1;
static int hf_peer_flags_res = -1;
static int hf_peer_distinguisher = -1;
static int hf_peer_ipv4_address = -1;
static int hf_peer_ipv6_address = -1;
static int hf_peer_asn = -1;
static int hf_peer_bgp_id = -1;
static int hf_peer_timestamp_sec = -1;
static int hf_peer_timestamp_msec = -1;

/* BMP Peer Up Notification filed */
static int hf_peer_up_ipv4_address = -1;
static int hf_peer_up_ipv6_address = -1;
static int hf_peer_up_local_port = -1;
static int hf_peer_up_remote_port = -1;
static int hf_peer_up_sent_open = -1;
static int hf_peer_up_rcvd_open = -1;
static int hf_peer_up_info = -1;

/* BMP Peer Down Notification filed */
static int hf_peer_down_reason = -1;
static int hf_peer_down_data = -1;

/* BMP Stat Reports filed */
static int hf_stats_count = -1;
static int hf_stat_type = -1;
static int hf_stat_len = -1;
static int hf_stat_data_4 = -1;
static int hf_stat_data_8 = -1;

/* BMP Termination filed */
static int hf_term_types = -1;
static int hf_term_type = -1;
static int hf_term_len = -1;
static int hf_term_info = -1;
static int hf_term_reason = -1;

static int hf_mirror_type = -1;
static int hf_mirror_length = -1;
static int hf_mirror_code = -1;

static gint ett_bmp = -1;
static gint ett_bmp_route_monitoring = -1;
static gint ett_bmp_stat_report = -1;
static gint ett_bmp_stat_type = -1;
static gint ett_bmp_peer_down = -1;
static gint ett_bmp_peer_up = -1;
static gint ett_bmp_peer_up_sent_open = -1;
static gint ett_bmp_peer_up_rcvd_open = -1;
static gint ett_bmp_peer_up_info = -1;
static gint ett_bmp_peer_header = -1;
static gint ett_bmp_peer_flags = -1;
static gint ett_bmp_init = -1;
static gint ett_bmp_init_types = -1;
static gint ett_bmp_init_type = -1;
static gint ett_bmp_term = -1;
static gint ett_bmp_term_type = -1;
static gint ett_bmp_term_types = -1;
static gint ett_bmp_route_mirroring = -1;
static gint ett_bmp_route_mirror_type = -1;
static gint ett_bmp_route_mirror_code = -1;

static dissector_handle_t dissector_bgp;

/* desegmentation */
static gboolean bmp_desegment = TRUE;


static void
dissect_bmp_info_tlvs(tvbuff_t *tvb, proto_tree *parent_tree, proto_tree *pti, packet_info *pinfo _U_, int offset)
{
    guint16 init_type;
    guint16 init_len;

    while (tvb_reported_length_remaining(tvb, offset) > 0) {
        proto_tree *ti;
        proto_tree *subtree;

        init_type = tvb_get_ntohs(tvb, offset);
        proto_item_append_text(pti, ", Type %s",
                val_to_str(init_type, init_typevals, "Unknown (0x%02x)"));

        ti = proto_tree_add_item(parent_tree, hf_init_type, tvb, offset, 2, ENC_BIG_ENDIAN);
        subtree = proto_item_add_subtree(ti, ett_bmp_init_type);
        offset += 2;

        init_len = tvb_get_ntohs(tvb, offset);
        proto_tree_add_item(subtree, hf_init_length, tvb, offset, 2, ENC_BIG_ENDIAN);
        offset += 2;

        proto_tree_add_item(subtree, hf_init_info, tvb, offset, init_len, ENC_ASCII|ENC_NA);
        offset += init_len;
    }
}

/*
 * Dissect BMP Peer Down Notification
 *
 *   0 1 2 3 4 5 6 7 8
 *   +-+-+-+-+-+-+-+-+
 *   |    Reason     | 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |            Data (present if Reason = 1, 2 or 3)               |
 *   ~                                                               ~
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 */
static void
dissect_bmp_peer_down_notification(tvbuff_t *tvb, proto_tree *tree, packet_info *pinfo, int offset, gint8 flags _U_)
{
    guint8 down_reason;

    down_reason = tvb_get_guint8(tvb, offset);
    proto_tree_add_item(tree, hf_peer_down_reason, tvb, offset, 1, ENC_BIG_ENDIAN);
    offset += 1;

    if (down_reason != BMP_PEER_DOWN_REMOTE_NO_NOTIFY) {
        if (down_reason == BMP_PEER_DOWN_LOCAL_NO_NOTIFY) {
            proto_tree_add_item(tree, hf_peer_down_data, tvb, offset, 2, ENC_NA);
        } else {
            call_dissector(dissector_bgp, tvb_new_subset_remaining(tvb, offset), pinfo, tree);
        }
    }
}

/*
 * Dissect BMP Peer Up Notification
 *
 *   0 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                 Local Address (16 bytes)                      |
 *   ~                                                               ~
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |         Local Port            |        Remote Port            |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                    Sent OPEN Message                          |
 *   ~                                                               ~
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                  Received OPEN Message                        |
 *   ~                                                               ~
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 */
static void
dissect_bmp_peer_up_notification(tvbuff_t *tvb, proto_tree *tree, packet_info *pinfo, int offset, gint8 flags)
{
    guint16 open_len;
    proto_item *ti;
    proto_item *subtree;

    if (flags & BMP_PEER_FLAG_IPV6) {
        proto_tree_add_item(tree, hf_peer_up_ipv6_address, tvb, offset, 16, ENC_NA);
        offset += 16;
    } else {
        proto_tree_add_item(tree, hf_bmp_unused, tvb, offset, 12, ENC_NA);
        offset += 12;
        proto_tree_add_item(tree, hf_peer_up_ipv4_address, tvb, offset, 4, ENC_BIG_ENDIAN);
        offset += 4;
    }

    proto_tree_add_item(tree, hf_peer_up_local_port, tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    proto_tree_add_item(tree, hf_peer_up_remote_port, tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    open_len = tvb_get_ntohs(tvb, offset + 16);
    if (!open_len)
        return;

    ti = proto_tree_add_item(tree, hf_peer_up_sent_open, tvb, offset, open_len, ENC_NA);
    subtree = proto_item_add_subtree(ti, ett_bmp_peer_up_sent_open);

    call_dissector(dissector_bgp, tvb_new_subset_length(tvb, offset, open_len), pinfo, subtree);
    offset += open_len;

    open_len = tvb_get_ntohs(tvb, offset + 16);
    if (!open_len)
        return;

    ti = proto_tree_add_item(tree, hf_peer_up_rcvd_open, tvb, offset, open_len, ENC_NA);
    subtree = proto_item_add_subtree(ti, ett_bmp_peer_up_rcvd_open);

    call_dissector(dissector_bgp, tvb_new_subset_length(tvb, offset, open_len), pinfo, subtree);
    offset += open_len;

    ti = proto_tree_add_item(tree, hf_peer_up_info, tvb, offset, tvb_reported_length_remaining(tvb, offset), ENC_NA);
    subtree = proto_item_add_subtree(ti, ett_bmp_peer_up_info);

    dissect_bmp_info_tlvs(tvb, subtree, ti, pinfo, offset);
}

/*
 * Dissect BMP Stats Report
 *
 *   0 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                        Stats Count                            |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 *
 *   0 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |         Stat Type             |          Stat Len             |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                        Stat Data                              |
 *   ~                                                               ~
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 */
static void
dissect_bmp_stat_report(tvbuff_t *tvb, proto_tree *tree, packet_info *pinfo _U_, int offset, gint8 flags _U_)
{
    guint16 stat_len;
    guint32 i;

    guint32 stats_count = tvb_get_ntohl(tvb, offset);

    proto_tree_add_item(tree, hf_stats_count, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    for (i = 0; i < stats_count; i++) {
        proto_item *ti;
        proto_item *subtree;

        ti = proto_tree_add_item(tree, hf_stat_type, tvb, offset, 2, ENC_BIG_ENDIAN);
        subtree = proto_item_add_subtree(ti, ett_bmp_stat_type);
        offset += 2;

        stat_len = tvb_get_ntohs(tvb, offset);
        proto_tree_add_item(subtree, hf_stat_len, tvb, offset, 2, ENC_BIG_ENDIAN);
        offset += 2;

        if (stat_len == 4) {
            proto_tree_add_item(subtree, hf_stat_data_4, tvb, offset, stat_len, ENC_BIG_ENDIAN);
        } else {
            proto_tree_add_item(subtree, hf_stat_data_8, tvb, offset, stat_len, ENC_BIG_ENDIAN);
        }
        offset += stat_len;
    }
}

/*
 * Dissect BMP Termination Message
 *
 *   0 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |          Information Type     |       Information Length      |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                 Information (variable)                        |
 *   ~                                                               ~
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 */
static void
dissect_bmp_termination(tvbuff_t *tvb, proto_tree *tree, packet_info *pinfo _U_, int offset, guint8 bmp_type _U_, guint16 len)
{
    guint16 term_type;
    guint16 term_len;

    proto_item *ti;
    proto_item *subtree;

    ti = proto_tree_add_item(tree, hf_term_types, tvb, offset, len, ENC_NA);
    subtree = proto_item_add_subtree(ti, ett_bmp_term_types);

    term_type = tvb_get_ntohs(tvb, offset);
    proto_item_append_text(subtree, ", Type %s",
            val_to_str(term_type, term_typevals, "Unknown (0x%02x)"));

    proto_tree_add_item(subtree, hf_term_type, tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    term_len = tvb_get_ntohs(tvb, offset);
    proto_tree_add_item(subtree, hf_term_len, tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    if (term_type == BMP_TERM_TYPE_STRING) {
        proto_tree_add_item(subtree, hf_term_info, tvb, offset, term_len, ENC_ASCII|ENC_NA);
    } else {
        proto_tree_add_item(subtree, hf_term_reason, tvb, offset, term_len, ENC_BIG_ENDIAN);
    }
    /*offset += term_len;*/
}


/*
 *
 */
static void
dissect_bmp_mirror(tvbuff_t *tvb, proto_tree *tree, packet_info *pinfo, int offset, gint8 flags _U_)
{
    guint16 tlv_type;
    guint16 tlv_len;
/*
    proto_tree *pti;
    proto_tree *parent_tree;

    pti = proto_tree_add_item(tree, hf_init_types, tvb, offset, len, ENC_NA);
    parent_tree = proto_item_add_subtree(pti, ett_bmp_init_types);
*/
    while (tvb_reported_length_remaining(tvb, offset) > 0) {
        proto_tree *ti;
        proto_tree *subtree;

        tlv_type = tvb_get_ntohs(tvb, offset);

        ti = proto_tree_add_item(tree, hf_mirror_type, tvb, offset, 2, ENC_BIG_ENDIAN);
        subtree = proto_item_add_subtree(ti, ett_bmp_route_mirror_type);
        offset += 2;

        tlv_len = tvb_get_ntohs(tvb, offset);
        proto_tree_add_item(subtree, hf_mirror_length, tvb, offset, 2, ENC_BIG_ENDIAN);
        offset += 2;

        switch (tlv_type) {
            case 0:
                call_dissector(dissector_bgp, tvb_new_subset_remaining(tvb, offset), pinfo, ti);
                break;
            case 1:
                proto_tree_add_item(subtree, hf_mirror_code, tvb, offset, 2, ENC_BIG_ENDIAN);
                break;
            default:
                break;
        }
        offset += tlv_len;
    }

}

/*
 * Dissect BMP Per-Peer Header
 *
 *   0 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |   Peer Type   |  Peer Flags   |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |         Peer Distinguisher (present based on peer type)       |
 *   |                                                               |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                 Peer Address (16 bytes)                       |
 *   ~                                                               ~
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                           Peer AS                             |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                         Peer BGP ID                           |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                    Timestamp (seconds)                        |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                  Timestamp (microseconds)                     |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 */
static void
dissect_bmp_peer_header(tvbuff_t *tvb, proto_tree *tree, packet_info *pinfo, int offset, guint8 bmp_type, guint16 len _U_)
{
    guint8  flags;
    proto_item *item;
    proto_item *ti;
    proto_item *subtree;

    static const int * peer_flags[] = {
        &hf_peer_flags_ipv6,
        &hf_peer_flags_post_policy,
        &hf_peer_flags_as2,
        &hf_peer_flags_res,
        NULL
    };

    ti = proto_tree_add_item(tree, hf_peer_header, tvb, offset, 42, ENC_NA);
    subtree = proto_item_add_subtree(ti, ett_bmp_peer_header);

    proto_tree_add_item(subtree, hf_peer_type, tvb, offset, 1, ENC_BIG_ENDIAN);
    offset += 1;

    flags = tvb_get_guint8(tvb, offset);

    proto_tree_add_bitmask(subtree, tvb, offset, hf_peer_flags, ett_bmp_peer_flags, peer_flags, ENC_NA);
    offset += 1;

    item = proto_tree_add_item(subtree, hf_peer_distinguisher, tvb, offset, 8, ENC_NA);
    proto_item_set_text(item, "Peer Distinguisher: %s", decode_bgp_rd(tvb, offset));
    offset += 8;

    if (flags & BMP_PEER_FLAG_IPV6) {
        proto_tree_add_item(subtree, hf_peer_ipv6_address, tvb, offset, 16, ENC_NA);
        offset += 16;
    } else {
        proto_tree_add_item(subtree, hf_bmp_unused, tvb, offset, 12, ENC_NA);
        offset += 12;
        proto_tree_add_item(subtree, hf_peer_ipv4_address, tvb, offset, 4, ENC_BIG_ENDIAN);
        offset += 4;
    }

    proto_tree_add_item(subtree, hf_peer_asn, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(subtree, hf_peer_bgp_id, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(subtree, hf_peer_timestamp_sec, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(subtree, hf_peer_timestamp_msec, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    switch (bmp_type) {
        case BMP_MSG_TYPE_ROUTE_MONITORING:
            call_dissector(dissector_bgp, tvb_new_subset_remaining(tvb, offset), pinfo, tree);
            break;
        case BMP_MSG_TYPE_STAT_REPORT:
            dissect_bmp_stat_report(tvb, tree, pinfo, offset, flags);
            break;
        case BMP_MSG_TYPE_PEER_DOWN:
            dissect_bmp_peer_down_notification(tvb, tree, pinfo, offset, flags);
            break;
        case BMP_MSG_TYPE_PEER_UP:
            dissect_bmp_peer_up_notification(tvb, tree, pinfo, offset, flags);
            break;
        case BMP_MSG_TYPE_ROUTE_MIRRORING:
            dissect_bmp_mirror(tvb, tree, pinfo, offset, flags);
            break;
        case BMP_MSG_TYPE_INIT:
        case BMP_MSG_TYPE_TERM:
        default:
            DISSECTOR_ASSERT_NOT_REACHED();
            break;
    }
}



/*
 * Dissect BMP Initiation Message
 *
 *   0 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |          Information Type     |       Information Length      |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                 Information (variable)                        |
 *   ~                                                               ~
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 */
static void
dissect_bmp_init(tvbuff_t *tvb, proto_tree *tree, packet_info *pinfo, int offset, guint8 bmp_type _U_, guint16 len)
{
    proto_tree *pti;
    proto_tree *parent_tree;

    pti = proto_tree_add_item(tree, hf_init_types, tvb, offset, len - offset, ENC_NA);
    parent_tree = proto_item_add_subtree(pti, ett_bmp_init_types);

    dissect_bmp_info_tlvs(tvb, parent_tree, pti, pinfo, offset);
}

/*
 * Dissect BMP PDU and Common Header
 *
 *   0 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8
 *   +-+-+-+-+-+-+-+-+
 *   |    Version    |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                        Message Length                         |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |   Msg. Type   |
 *   +---------------+
 *
 */
static guint
get_bmp_pdu_len(packet_info *pinfo _U_, tvbuff_t *tvb, int offset, void *data _U_)
{
    return tvb_get_ntohl(tvb, offset + 1);
}

static int
dissect_bmp_pdu(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data _U_)
{
    int         offset = 0;
    guint8      bmp_type;
    guint16     len;
    gint        arg;
    proto_item  *ti;
    proto_item  *bmp_tree;
    size_t      *items = (size_t *)data;

    bmp_type = tvb_get_guint8(tvb, 5);

    ti = proto_tree_add_item(tree, proto_bmp, tvb, 0, -1, ENC_NA);
    proto_item_append_text(ti, ", Type %s",
            val_to_str(bmp_type, bmp_typevals, "Unknown (0x%02x)"));

    if (bmp_type < BMP_MSG_TYPE__SIZE)
        items[bmp_type]++;
    else
        items[BMP_MSG_TYPE__SIZE]++;

    switch (bmp_type) {
        case BMP_MSG_TYPE_ROUTE_MONITORING:
            arg = ett_bmp_route_monitoring;
            break;
        case BMP_MSG_TYPE_STAT_REPORT:
            arg = ett_bmp_stat_report;
            break;
        case BMP_MSG_TYPE_PEER_DOWN:
            arg = ett_bmp_peer_down;
            break;
        case BMP_MSG_TYPE_PEER_UP:
            arg = ett_bmp_peer_up;
            break;
        case BMP_MSG_TYPE_INIT:
            arg = ett_bmp_init;
            break;
        case BMP_MSG_TYPE_TERM:
            arg = ett_bmp_term;
            break;
        case BMP_MSG_TYPE_ROUTE_MIRRORING:
            arg = ett_bmp_route_mirroring;
            break;
        default:
            arg = ett_bmp;
            break;
    }

    bmp_tree = proto_item_add_subtree(ti, arg);

    proto_tree_add_item(bmp_tree, hf_bmp_version, tvb, offset, 1, ENC_BIG_ENDIAN);
    offset += 1;
    proto_tree_add_item(bmp_tree, hf_bmp_length, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(bmp_tree, hf_bmp_type, tvb, offset, 1, ENC_BIG_ENDIAN);
    offset += 1;

    len = tvb_get_ntohs(tvb, offset);

    switch (bmp_type) {
        case BMP_MSG_TYPE_INIT:
            dissect_bmp_init(tvb, bmp_tree, pinfo, offset, bmp_type, len);
            break;
        case BMP_MSG_TYPE_ROUTE_MONITORING:
        case BMP_MSG_TYPE_STAT_REPORT:
        case BMP_MSG_TYPE_PEER_DOWN:
        case BMP_MSG_TYPE_PEER_UP:
        case BMP_MSG_TYPE_ROUTE_MIRRORING:
            dissect_bmp_peer_header(tvb, bmp_tree, pinfo, offset, bmp_type, len);
            break;
        case BMP_MSG_TYPE_TERM:
            dissect_bmp_termination(tvb, bmp_tree, pinfo, offset, bmp_type, len);
            break;
        default:
            break;
    }

    return tvb_captured_length(tvb);
}


/* Main dissecting routine */
static int
dissect_bmp(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data _U_)
{
    size_t items[BMP_MSG_TYPE__SIZE + 1] = {};
    static const unsigned print_order[] = {
        BMP_MSG_TYPE_INIT,
        BMP_MSG_TYPE_TERM,
        BMP_MSG_TYPE_PEER_UP,
        BMP_MSG_TYPE_PEER_DOWN,
        BMP_MSG_TYPE_ROUTE_MONITORING,
        BMP_MSG_TYPE_ROUTE_MIRRORING,
        BMP_MSG_TYPE_STAT_REPORT,
        BMP_MSG_TYPE__SIZE,
    };
    gchar buf[512] = "", *b = buf;

    tcp_dissect_pdus(tvb, pinfo, tree, bmp_desegment, FRAME_HEADER_LEN, get_bmp_pdu_len, dissect_bmp_pdu, items);

    for (size_t i = 0; i < sizeof(print_order) / sizeof(print_order[0]); i++) {
        unsigned bmp_type = print_order[i];

        if (items[bmp_type] == 0)
            continue;

        g_snprintf(b, sizeof(buf) - (b - buf), "%s%s",
                (b == buf) ? "" : ", ",
                val_to_str(bmp_type, bmp_typevals, "Unknown"));
        b += strlen(b);
        if (items[bmp_type] > 1) {
            g_snprintf(b, sizeof(buf) - (b - buf), "(%zu)", items[bmp_type]);
            b += strlen(b);
        }
    }
    col_set_str(pinfo->cinfo, COL_PROTOCOL, "BMP");
    col_set_str(pinfo->cinfo, COL_INFO, buf);

    return tvb_captured_length(tvb);
}


void
proto_register_bmp(void)
{
    static hf_register_info hf[] = {
        /* BMP Common Header */
        { &hf_bmp_version,
            { "Version", "bmp.version", FT_UINT8, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_bmp_length,
            { "Length", "bmp.length", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_bmp_type,
            { "Type", "bmp.type", FT_UINT8, BASE_DEC,
                VALS(bmp_typevals), 0x0, "BMP message type", HFILL }},

        /* Unused/Reserved Bytes */
        { &hf_bmp_unused,
            { "Unused", "bmp.unused", FT_BYTES, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},

        /* Initiation Header */
        { &hf_init_types,
            { "Information Types", "bmp.init.types", FT_NONE, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_init_type,
            { "Type", "bmp.init.type", FT_UINT16, BASE_DEC,
                VALS(init_typevals), 0x0, "Initiation type", HFILL }},
        { &hf_init_length,
            { "Length", "bmp.init.length", FT_UINT16, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_init_info,
            { "Information", "bmp.init.info", FT_STRING, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},

        /* Per Peer Header */
        { &hf_peer_header,
            { "Per Peer Header", "bmp.peer.header", FT_NONE, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_type,
            { "Type", "bmp.peer.type", FT_UINT8, BASE_DEC,
                VALS(peer_typevals), 0x0, NULL, HFILL }},
        { &hf_peer_flags,
            { "Flags", "bmp.peer.flags", FT_UINT8, BASE_HEX,
                NULL, BMP_PEER_FLAG_MASK, NULL, HFILL }},
        { &hf_peer_flags_ipv6,
            { "IPv6", "bmp.peer.flags.ipv6", FT_BOOLEAN, 8,
                TFS(&tfs_set_notset), BMP_PEER_FLAG_IPV6, NULL, HFILL }},
        { &hf_peer_flags_post_policy,
            { "Post-policy", "bmp.peer.flags.post_policy", FT_BOOLEAN, 8,
                TFS(&tfs_set_notset), BMP_PEER_FLAG_POST_POLICY, NULL, HFILL }},
        { &hf_peer_flags_as2,
            { "Legacy 2-byte ASN", "bmp.peer.flags.as2", FT_BOOLEAN, 8,
                TFS(&tfs_set_notset), BMP_PEER_FLAG_AS2, NULL, HFILL }},
        { &hf_peer_flags_res,
            { "Reserved", "bmp.peer.flags.reserved", FT_BOOLEAN, 8,
                TFS(&tfs_set_notset), BMP_PEER_FLAG_RES, NULL, HFILL }},
        { &hf_peer_distinguisher,
            { "Peer Distinguisher", "bmp.peer.distinguisher", FT_BYTES, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_ipv4_address,
            { "Address", "bmp.peer.ip.addr", FT_IPv4, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_ipv6_address,
            { "Address", "bmp.peer.ipv6.addr", FT_IPv6, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_asn,
            { "ASN", "bmp.peer.asn", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_bgp_id,
            { "BGP ID", "bmp.peer.id", FT_IPv4, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_timestamp_sec,
            { "Timestamp (sec)", "bmp.peer.timestamp.sec", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_timestamp_msec,
            { "Timestamp (msec)", "bmp.peer.timestamp.msec", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_up_ipv4_address,
            { "Local Address", "bmp.peer.up.ip.addr", FT_IPv4, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_up_ipv6_address,
            { "Local Address", "bmp.peer.up.ipv6.addr", FT_IPv6, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_up_local_port,
            { "Local Port", "bmp.peer.up.port.local", FT_UINT16, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_up_remote_port,
            { "Remote Port", "bmp.peer.up.port.remote", FT_UINT16, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_up_sent_open,
            { "BGP OPEN sent by monitored station", "bmp.peer.up.sent_open", FT_NONE, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_up_rcvd_open,
            { "BGP OPEN rcvd by monitored station", "bmp.peer.up.rcvd_open", FT_NONE, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_peer_up_info,
            { "Information", "bmp.peer.up.info", FT_NONE, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},

        /* Peer Down Notification */
        { &hf_peer_down_reason,
            { "Reason", "bmp.peer.down.reason", FT_UINT8, BASE_DEC,
                VALS(down_reason_typevals), 0x0, NULL, HFILL }},
        { &hf_peer_down_data,
            { "Data", "bmp.peer.down.data", FT_NONE, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},

        /* Stats Report */
        { &hf_stats_count,
            { "Stats Count", "bmp.stats.count", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_type,
            { "Type", "bmp.stats.type", FT_UINT16, BASE_DEC,
                VALS(stat_typevals), 0x0, NULL, HFILL }},
        { &hf_stat_len,
            { "Length", "bmp.stats.length", FT_UINT16, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_4,
            { "Data", "bmp.stats.data.4byte", FT_UINT32, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_stat_data_8,
            { "Data", "bmp.stats.data.8byte", FT_UINT64, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},

        /* Termination Message */
        { &hf_term_types,
            { "Termination Types", "bmp.term.types", FT_NONE, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_term_type,
            { "Type", "bmp.term.type", FT_UINT16, BASE_DEC,
                VALS(term_typevals), 0x0, NULL, HFILL }},
        { &hf_term_len,
            { "Length", "bmp.term.length", FT_UINT16, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_term_info,
            { "Information", "bmp.term.info", FT_STRING, BASE_NONE,
                NULL, 0x0, NULL, HFILL }},
        { &hf_term_reason,
            { "Reason", "bmp.term.reason", FT_UINT16, BASE_DEC,
                VALS(term_reason_typevals), 0x0, NULL, HFILL }},

        /* Mirror Message */
        { &hf_mirror_type,
            { "Type", "bmp.mirror.type", FT_UINT16, BASE_DEC,
                VALS(mirror_typevals), 0x0, "Mirror TLV type", HFILL }},
        { &hf_mirror_length,
            { "Length", "bmp.mirror.length", FT_UINT16, BASE_DEC,
                NULL, 0x0, NULL, HFILL }},
        { &hf_mirror_code,
            { "Code", "bmp.mirror.code", FT_UINT16, BASE_DEC,
                VALS(mirror_codevals), 0x0, "Mirror information code", HFILL }},
    };

    /* Setup protocol subtree array */
    static gint *ett[] = {
        &ett_bmp,
        &ett_bmp_route_monitoring,
        &ett_bmp_stat_report,
        &ett_bmp_stat_type,
        &ett_bmp_peer_down,
        &ett_bmp_peer_up,
        &ett_bmp_peer_up_sent_open,
        &ett_bmp_peer_up_rcvd_open,
        &ett_bmp_peer_up_info,
        &ett_bmp_peer_header,
        &ett_bmp_peer_flags,
        &ett_bmp_init,
        &ett_bmp_init_type,
        &ett_bmp_init_types,
        &ett_bmp_term,
        &ett_bmp_term_type,
        &ett_bmp_term_types,
        &ett_bmp_route_mirroring,
        &ett_bmp_route_mirror_type,
        &ett_bmp_route_mirror_code,
    };

    module_t *bmp_module;

    proto_bmp = proto_register_protocol(
            "BGP Monitoring Protocol", /* name */
            "BMP",                     /* short name */
            "bmp"                      /* abbrev */
            );

    proto_register_field_array(proto_bmp, hf, array_length(hf));
    proto_register_subtree_array(ett, array_length(ett));

    bmp_module = prefs_register_protocol(proto_bmp, NULL);
    prefs_register_bool_preference(bmp_module, "desegment",
            "Reassemble BMP messages spanning multiple TCP segments",
            "Whether the BMP dissector should reassemble messages spanning multiple TCP segments."
            " To use this option, you must also enable \"Allow subdissectors to reassemble TCP streams\" in the TCP protocol settings.",
            &bmp_desegment);
}


void
proto_reg_handoff_bmp(void)
{
    static dissector_handle_t bmp_handle;

    bmp_handle = create_dissector_handle(dissect_bmp, proto_bmp);
    dissector_add_for_decode_as_with_preference("tcp.port", bmp_handle);
    dissector_bgp = find_dissector_add_dependency("bgp", proto_bmp);
}
/*
* Editor modelines - http://www.wireshark.org/tools/modelines.html
*
* Local variables:
* c-basic-offset: 4
* tab-width: 8
* indent-tabs-mode: nil
* End:
*
* ex: set shiftwidth=4 tabstop=8 expandtab:
* :indentSize=4:tabSize=8:noTabs=true:
*/
