/**
 * @file dpdk_version_compatibiliy.h
 * @brief DPDK version compatibility macros
 *
 * This header provides compatibility macros to support both old and new DPDK API versions.
 * Include this header before using any DPDK-specific macros that have changed between versions.
 */

#ifndef DPDK_VERSION_COMPATIBILITY_H
#define DPDK_VERSION_COMPATIBILITY_H

#include <rte_common.h>

/*
 * Packed structure compatibility macros
 *
 * Old DPDK (< 21.11) used: struct mystruct { ... } __rte_packed;
 * New DPDK (>= 21.11) uses: struct __rte_packed_begin mystruct { ... } __rte_packed_end;
 *
 * For backwards compatibility, we define macros that work with both styles.
 * Use the new style in code going forward.
 */
#ifndef __rte_packed_begin
#define __rte_packed_begin
#endif

#ifndef __rte_packed_end
#define __rte_packed_end __rte_packed
#endif

#ifndef RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE
#ifdef DEV_TX_OFFLOAD_MBUF_FAST_FREE
#define RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE DEV_TX_OFFLOAD_MBUF_FAST_FREE
#endif
#endif

#ifndef DEV_TX_OFFLOAD_MBUF_FAST_FREE
#ifdef RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE
#define DEV_TX_OFFLOAD_MBUF_FAST_FREE RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE
#endif
#endif

#ifndef RTE_ETH_RX_OFFLOAD_SCATTER
#ifdef DEV_RX_OFFLOAD_SCATTER
#define RTE_ETH_RX_OFFLOAD_SCATTER DEV_RX_OFFLOAD_SCATTER
#endif
#endif

#ifndef DEV_RX_OFFLOAD_SCATTER
#ifdef RTE_ETH_RX_OFFLOAD_SCATTER
#define DEV_RX_OFFLOAD_SCATTER RTE_ETH_RX_OFFLOAD_SCATTER
#endif
#endif

#ifndef RTE_ICMP_TYPE_ECHO_REQUEST
#define RTE_ICMP_TYPE_ECHO_REQUEST 8
#endif

#ifndef RTE_ICMP_TYPE_ECHO_REPLY
#define RTE_ICMP_TYPE_ECHO_REPLY 0
#endif

#endif /* DPDK_VERSION_COMPATIBILITY_H */
