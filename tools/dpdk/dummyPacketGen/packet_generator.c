#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <inttypes.h>
#include <getopt.h>
#include <stdbool.h>
#include <arpa/inet.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_errno.h>

#include "json.h"

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define MAX_NICS            8
#define FRAMES_PER_BUFFER   400
#define RX_RING_SIZE        1024
#define TX_RING_SIZE        32768
#define JUMBO_FRAME_ELEMENT_SIZE 0x2600

#define DEFAULT_INTERVAL            1000
#define DEFAULT_STARTING_FRAME      0
#define DEFAULT_FRAMES              1000
#define DEFAULT_PACKETS_PER_FRAME   250
#define DEFAULT_DEST_IP             "10.100.0.6"
#define DEFAULT_DEST_MAC            "08:c0:eb:f8:28:7c"
#define DEFAULT_DEST_PORT           1234
#define DEFAULT_SRC_IP              "10.100.0.5"
#define DEFAULT_SRC_MAC             "08:c0:eb:f8:28:6c"
#define DEFAULT_SRC_PORT            1234

/* -------------------------------------------------------------------------
 * Types
 * ---------------------------------------------------------------------- */

typedef enum {
    RR_MODE_FRAME  = 0,   /* whole frame rotates across NICs */
    RR_MODE_PACKET = 1,   /* packets split by contiguous range across NICs */
} rr_mode_t;

/* Per-NIC network addressing */
typedef struct {
    char     pcie_addr[32];   /* PCIe address, e.g. "0000:2a:00.0" */
    char     dest_ip[20];
    char     dest_mac[20];
    char     src_ip[20];
    char     src_mac[20];
    uint16_t port_id;         /* resolved DPDK port ID after hotplug */
} nic_config_t;

/* Mercury detector packet header */
struct mercury_hdr {
    rte_be64_t frame_number;
    rte_be64_t padding[6];
    rte_be32_t packet_number;
    uint8_t    markers;
    uint8_t    _unused_1;
    uint8_t    padding_bytes;
    uint8_t    readout_lane;
} __rte_packed;

/* Global configuration */
struct {
    uint64_t    frames;
    uint64_t    packets_per_frame;
    uint64_t    interval;
    uint64_t    starting_frame_number;
    uint16_t    destination_port;
    uint16_t    source_port;
    uint64_t    drop_packets;
    uint64_t    drop_frames;

    uint32_t    num_nics;
    rr_mode_t   rr_mode;
    char        nic_config_path[256];
    nic_config_t nics[MAX_NICS];
} config;

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static uint64_t gbps_to_delay_us(double gbps)
{
    return (uint64_t)(15868.90 / gbps - 0.0762 * gbps + 4.56);
}

static void print_help(void)
{
    printf("\nUsage: packet_generator [EAL options] -- [options]\n\n");
    printf("  --interval N       Inter-packet delay in microseconds (default: %d)\n", DEFAULT_INTERVAL);
    printf("  --start_frame N    Starting frame number (default: %d)\n", DEFAULT_STARTING_FRAME);
    printf("  --frames N         Number of frames to send (default: %d)\n", DEFAULT_FRAMES);
    printf("  --packets N        Packets per frame (default: %d)\n", DEFAULT_PACKETS_PER_FRAME);
    printf("  --dst_port N       UDP destination port (default: %d)\n", DEFAULT_DEST_PORT);
    printf("  --src_port N       UDP source port (default: %d)\n", DEFAULT_SRC_PORT);
    printf("  --drop_packet N    Probability 0-100 to drop a packet (default: 0)\n");
    printf("  --drop_frame N     Probability 0-100 to drop a frame (default: 0)\n");
    printf("  --bandwidth_test   Set interval for ~1 Gbps throughput\n");
    printf("  --num_nics N       Number of NICs to use (default: 1)\n");
    printf("  --nic_config PATH  JSON file with per-NIC addressing (see example_nic_config.json)\n");
    printf("  --mode [frame|packet]\n");
    printf("                     Round-robin mode:\n");
    printf("                       frame  - whole frames rotate across NICs (default)\n");
    printf("                       packet - packet ranges split across NICs per frame\n");
    printf("\n");
    printf("Single-NIC fallback options (used when --nic_config is not provided):\n");
    printf("  --dest_ip ADDR     Destination IP (default: %s)\n", DEFAULT_DEST_IP);
    printf("  --dest_mac ADDR    Destination MAC (default: %s)\n", DEFAULT_DEST_MAC);
    printf("  --src_ip ADDR      Source IP (default: %s)\n", DEFAULT_SRC_IP);
    printf("  --src_mac ADDR     Source MAC (default: %s)\n", DEFAULT_SRC_MAC);
    printf("  --help             Show this message\n\n");
}

/* -------------------------------------------------------------------------
 * JSON config loading
 * ---------------------------------------------------------------------- */

static int load_nic_config(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "ERROR: cannot open NIC config file: %s\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, sz, f) != (size_t)sz) {
        fprintf(stderr, "ERROR: failed to read %s\n", path);
        free(buf);
        return -1;
    }
    buf[sz] = '\0';
    fclose(f);

    char *err = NULL;
    json_value *root = json_parse(buf, &err);
    free(buf);

    if (!root) {
        fprintf(stderr, "ERROR: JSON parse error in %s: %s\n", path, err ? err : "unknown");
        free(err);
        return -1;
    }

    json_value *nics_arr = json_object_get(root, "nics");
    if (!nics_arr || nics_arr->type != JSON_ARRAY) {
        fprintf(stderr, "ERROR: JSON config must have a top-level \"nics\" array\n");
        json_free(root);
        return -1;
    }

    uint32_t count = (uint32_t)nics_arr->array_value.count;
    if (count != config.num_nics) {
        fprintf(stderr, "ERROR: --num_nics %u but nic_config has %u entries\n",
                config.num_nics, count);
        json_free(root);
        return -1;
    }
    if (count > MAX_NICS) {
        fprintf(stderr, "ERROR: nic_config has %u entries, max is %d\n", count, MAX_NICS);
        json_free(root);
        return -1;
    }

    for (uint32_t i = 0; i < count; i++) {
        json_value *nic = nics_arr->array_value.values[i];
        if (!nic || nic->type != JSON_OBJECT) {
            fprintf(stderr, "ERROR: nics[%u] is not an object\n", i);
            json_free(root);
            return -1;
        }

        json_value *v;
#define LOAD_STR(field, key) \
        v = json_object_get(nic, key); \
        if (!v || v->type != JSON_STRING) { \
            fprintf(stderr, "ERROR: nics[%u] missing or invalid \"" key "\"\n", i); \
            json_free(root); return -1; \
        } \
        strncpy(config.nics[i].field, v->string_value, sizeof(config.nics[i].field) - 1);

        LOAD_STR(pcie_addr, "pcie_addr");
        LOAD_STR(dest_ip,   "dest_ip");
        LOAD_STR(dest_mac,  "dest_mac");
        LOAD_STR(src_ip,    "src_ip");
        LOAD_STR(src_mac,   "src_mac");
#undef LOAD_STR

        config.nics[i].port_id = UINT16_MAX;  /* resolved later during hotplug */

        printf("NIC %u: pcie=%s dest=%s (%s) src=%s (%s)\n", i,
               config.nics[i].pcie_addr,
               config.nics[i].dest_ip, config.nics[i].dest_mac,
               config.nics[i].src_ip,  config.nics[i].src_mac);
    }

    json_free(root);
    return 0;
}

/* -------------------------------------------------------------------------
 * Port initialisation
 * ---------------------------------------------------------------------- */

static const struct rte_eth_conf port_conf_default = {
    .rxmode = {
        .max_lro_pkt_size = JUMBO_FRAME_ELEMENT_SIZE,
        .offloads = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | RTE_ETH_TX_OFFLOAD_UDP_CKSUM,
    },
};

static int port_init(uint16_t port_id, struct rte_mempool *mbuf_pool)
{
    struct rte_eth_conf port_conf = port_conf_default;
    int ret;

    ret = rte_eth_dev_configure(port_id, 1, 1, &port_conf);
    if (ret != 0) return ret;

    ret = rte_eth_rx_queue_setup(port_id, 0, RX_RING_SIZE,
                                 rte_eth_dev_socket_id(port_id), NULL, mbuf_pool);
    if (ret < 0) return ret;

    ret = rte_eth_tx_queue_setup(port_id, 0, TX_RING_SIZE,
                                 rte_eth_dev_socket_id(port_id), NULL);
    if (ret < 0) return ret;

    ret = rte_eth_dev_start(port_id);
    if (ret < 0) return ret;

    struct rte_ether_addr addr;
    ret = rte_eth_macaddr_get(port_id, &addr);
    if (ret != 0) return ret;

    printf("Port %u MAC: %02"PRIx8":%02"PRIx8":%02"PRIx8
           ":%02"PRIx8":%02"PRIx8":%02"PRIx8"\n", port_id,
           addr.addr_bytes[0], addr.addr_bytes[1], addr.addr_bytes[2],
           addr.addr_bytes[3], addr.addr_bytes[4], addr.addr_bytes[5]);

    return 0;
}

/* -------------------------------------------------------------------------
 * Packet header filling
 * ---------------------------------------------------------------------- */

static void fill_packet_headers(struct rte_mbuf *pkt,
                                 uint32_t nic_idx,
                                 uint64_t frame_number,
                                 uint32_t packet_number,
                                 uint64_t data_len)
{
    const int l2 = sizeof(struct rte_ether_hdr);
    const int l3 = sizeof(struct rte_ipv4_hdr);
    const int l4 = sizeof(struct rte_udp_hdr);

    pkt->pkt_len  = l2 + l3 + l4 + sizeof(struct mercury_hdr) + data_len;
    pkt->data_len = pkt->pkt_len;

    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
    struct rte_ipv4_hdr  *ip  = (struct rte_ipv4_hdr *)((char *)eth + l2);
    struct rte_udp_hdr   *udp = (struct rte_udp_hdr *)((char *)ip  + l3);
    struct mercury_hdr   *mhdr = (struct mercury_hdr *)((char *)udp + l4);

    /* Ethernet */
    rte_ether_unformat_addr(config.nics[nic_idx].src_mac,  &eth->src_addr);
    rte_ether_unformat_addr(config.nics[nic_idx].dest_mac, &eth->dst_addr);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    /* IPv4 */
    uint32_t addr_buf;
    inet_pton(AF_INET, config.nics[nic_idx].dest_ip, &addr_buf);
    ip->dst_addr = addr_buf;
    inet_pton(AF_INET, config.nics[nic_idx].src_ip,  &addr_buf);
    ip->src_addr = addr_buf;
    ip->version_ihl     = RTE_IPV4_VHL_DEF;
    ip->type_of_service = 0;
    ip->total_length    = 0;
    ip->packet_id       = 0;
    ip->fragment_offset = 0;
    ip->time_to_live    = 128;
    ip->next_proto_id   = IPPROTO_UDP;
    ip->hdr_checksum    = 0;

    /* UDP */
    udp->dst_port  = rte_bswap16(config.destination_port);
    udp->src_port  = rte_bswap16(config.source_port);
    udp->dgram_len = rte_bswap16((uint16_t)(l4 + sizeof(struct mercury_hdr) + data_len));
    udp->dgram_cksum = 0;

    /* Mercury header */
    memset(mhdr, 0, sizeof(*mhdr));
    mhdr->frame_number  = rte_cpu_to_be_64(frame_number);
    mhdr->packet_number = rte_bswap32(packet_number);

    /* Payload: incrementing uint16 test pattern */
    uint16_t *payload = (uint16_t *)((char *)mhdr + sizeof(struct mercury_hdr));
    for (uint64_t i = 0; i < data_len / 2; i++)
        payload[i] = (uint16_t)((frame_number & 0xFFFF) + i);
}

/* Update only the frame_number field in a pre-built packet */
static void update_frame_number(struct rte_mbuf *pkt, uint64_t frame_number)
{
    const int l2 = sizeof(struct rte_ether_hdr);
    const int l3 = sizeof(struct rte_ipv4_hdr);
    const int l4 = sizeof(struct rte_udp_hdr);

    struct rte_ether_hdr *eth  = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
    struct mercury_hdr   *mhdr = (struct mercury_hdr *)
        ((char *)eth + l2 + l3 + l4);
    mhdr->frame_number = rte_cpu_to_be_64(frame_number);
}

/* -------------------------------------------------------------------------
 * NIC selection helpers
 * ---------------------------------------------------------------------- */

static inline uint16_t nic_for_frame(uint64_t frame_number)
{
    return (uint16_t)(frame_number % config.num_nics);
}

static inline uint16_t nic_for_packet(uint32_t packet_number)
{
    uint32_t range = (uint32_t)(config.packets_per_frame / config.num_nics);
    uint16_t nic   = (uint16_t)(packet_number / range);
    if (nic >= config.num_nics)
        nic = config.num_nics - 1;
    return nic;
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "EAL initialisation failed\n");

    argc -= ret;
    argv += ret;

    /* --- Defaults -------------------------------------------------------- */
    config.frames               = DEFAULT_FRAMES;
    config.packets_per_frame    = DEFAULT_PACKETS_PER_FRAME;
    config.interval             = DEFAULT_INTERVAL;
    config.starting_frame_number = DEFAULT_STARTING_FRAME;
    config.destination_port     = DEFAULT_DEST_PORT;
    config.source_port          = DEFAULT_SRC_PORT;
    config.drop_packets         = 0;
    config.drop_frames          = 0;
    config.num_nics             = 1;
    config.rr_mode              = RR_MODE_FRAME;
    config.nic_config_path[0]   = '\0';

    /* Single-NIC defaults (used if no --nic_config) */
    strncpy(config.nics[0].dest_ip,  DEFAULT_DEST_IP,  sizeof(config.nics[0].dest_ip)  - 1);
    strncpy(config.nics[0].dest_mac, DEFAULT_DEST_MAC, sizeof(config.nics[0].dest_mac) - 1);
    strncpy(config.nics[0].src_ip,   DEFAULT_SRC_IP,   sizeof(config.nics[0].src_ip)   - 1);
    strncpy(config.nics[0].src_mac,  DEFAULT_SRC_MAC,  sizeof(config.nics[0].src_mac)  - 1);

    /* --- Argument parsing ------------------------------------------------ */
    static struct option long_opts[] = {
        {"interval",        required_argument, NULL, 'i'},
        {"start_frame",     required_argument, NULL, 's'},
        {"frames",          required_argument, NULL, 'f'},
        {"packets",         required_argument, NULL, 'k'},
        {"dest_ip",         required_argument, NULL, 'd'},
        {"dest_mac",        required_argument, NULL, 'm'},
        {"src_ip",          required_argument, NULL, 'x'},
        {"src_mac",         required_argument, NULL, 'y'},
        {"drop_packet",     required_argument, NULL, 'p'},
        {"drop_frame",      required_argument, NULL, 'r'},
        {"src_port",        required_argument, NULL, 'u'},
        {"dst_port",        required_argument, NULL, 'v'},
        {"bandwidth_test",  no_argument,       NULL, 'b'},
        {"num_nics",        required_argument, NULL, 'n'},
        {"nic_config",      required_argument, NULL, 'j'},
        {"mode",            required_argument, NULL, 'o'},
        {"help",            no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt, idx;
    while ((opt = getopt_long(argc, argv, "", long_opts, &idx)) != -1) {
        switch (opt) {
            case 'i': config.interval               = (uint64_t)atol(optarg); break;
            case 's': config.starting_frame_number  = (uint64_t)atol(optarg); break;
            case 'f': config.frames                 = (uint64_t)atol(optarg); break;
            case 'k': config.packets_per_frame      = (uint64_t)atol(optarg); break;
            case 'd': strncpy(config.nics[0].dest_ip,  optarg, sizeof(config.nics[0].dest_ip)  - 1); break;
            case 'm': strncpy(config.nics[0].dest_mac, optarg, sizeof(config.nics[0].dest_mac) - 1); break;
            case 'x': strncpy(config.nics[0].src_ip,   optarg, sizeof(config.nics[0].src_ip)   - 1); break;
            case 'y': strncpy(config.nics[0].src_mac,  optarg, sizeof(config.nics[0].src_mac)  - 1); break;
            case 'p': config.drop_packets           = (uint64_t)atol(optarg); break;
            case 'r': config.drop_frames            = (uint64_t)atol(optarg); break;
            case 'u': config.source_port            = (uint16_t)atoi(optarg); break;
            case 'v': config.destination_port       = (uint16_t)atoi(optarg); break;
            case 'b': config.interval               = gbps_to_delay_us(1.0);  break;
            case 'n': config.num_nics               = (uint32_t)atoi(optarg); break;
            case 'j': strncpy(config.nic_config_path, optarg, sizeof(config.nic_config_path) - 1); break;
            case 'o':
                if (strcmp(optarg, "packet") == 0)
                    config.rr_mode = RR_MODE_PACKET;
                else if (strcmp(optarg, "frame") == 0)
                    config.rr_mode = RR_MODE_FRAME;
                else {
                    fprintf(stderr, "ERROR: --mode must be 'frame' or 'packet'\n");
                    rte_eal_cleanup();
                    return -1;
                }
                break;
            case 'h':
                print_help();
                rte_eal_cleanup();
                return 0;
            default:
                fprintf(stderr, "Unknown option. Use --help for usage.\n");
                rte_eal_cleanup();
                return -1;
        }
    }

    /* Validate num_nics */
    if (config.num_nics == 0 || config.num_nics > MAX_NICS) {
        fprintf(stderr, "ERROR: --num_nics must be 1..%d\n", MAX_NICS);
        rte_eal_cleanup();
        return -1;
    }

    /* Load per-NIC config from JSON if provided */
    if (config.nic_config_path[0] != '\0') {
        if (load_nic_config(config.nic_config_path) != 0) {
            rte_eal_cleanup();
            return -1;
        }
    } else if (config.num_nics > 1) {
        fprintf(stderr, "ERROR: --num_nics > 1 requires --nic_config\n");
        rte_eal_cleanup();
        return -1;
    }

    /* Validate Mode B divisibility */
    if (config.rr_mode == RR_MODE_PACKET &&
        config.packets_per_frame % config.num_nics != 0) {
        fprintf(stderr, "ERROR: packets_per_frame (%lu) must be divisible by num_nics (%u) "
                "for packet round-robin mode\n",
                config.packets_per_frame, config.num_nics);
        rte_eal_cleanup();
        return -1;
    }

    printf("Configuration:\n");
    printf("  frames:            %lu\n", config.frames);
    printf("  packets_per_frame: %lu\n", config.packets_per_frame);
    printf("  interval_us:       %lu\n", config.interval);
    printf("  starting_frame:    %lu\n", config.starting_frame_number);
    printf("  num_nics:          %u\n",  config.num_nics);
    printf("  rr_mode:           %s\n",  config.rr_mode == RR_MODE_FRAME ? "frame" : "packet");

    /* --- Memory pool ----------------------------------------------------- */
    const uint64_t data_len = 8000;
    uint32_t total_pkts = config.num_nics * FRAMES_PER_BUFFER * (uint32_t)config.packets_per_frame;

    struct rte_mempool *mbuf_pool = rte_pktmbuf_pool_create(
        "MBUF_POOL",
        total_pkts * 4,
        RTE_MEMPOOL_CACHE_MAX_SIZE,
        RTE_MBUF_PRIV_ALIGN,
        JUMBO_FRAME_ELEMENT_SIZE,
        rte_socket_id()
    );
    if (!mbuf_pool)
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

    /* --- Port init (hotplug each NIC by PCIe address) -------------------- */
    for (uint32_t n = 0; n < config.num_nics; n++) {
        const char *pcie = config.nics[n].pcie_addr;

        if (pcie[0] == '\0') {
            /* No PCIe address: use port n directly (single-NIC fallback) */
            config.nics[n].port_id = (uint16_t)n;
        } else {
            ret = rte_eal_hotplug_add("pci", pcie, "");
            if (ret < 0 && ret != -EEXIST) {
                rte_exit(EXIT_FAILURE,
                         "Cannot hotplug NIC %u (PCIe %s): %s\n",
                         n, pcie, rte_strerror(-ret));
            }

            uint16_t port_id;
            ret = rte_eth_dev_get_port_by_name(pcie, &port_id);
            if (ret != 0) {
                rte_exit(EXIT_FAILURE,
                         "Cannot find port for NIC %u (PCIe %s)\n", n, pcie);
            }
            config.nics[n].port_id = port_id;
            printf("NIC %u: PCIe %s → port %u\n", n, pcie, port_id);
        }

        if (port_init(config.nics[n].port_id, mbuf_pool) != 0)
            rte_exit(EXIT_FAILURE, "Cannot init port %u (NIC %u)\n",
                     config.nics[n].port_id, n);
    }

    /* --- Pre-build packet buffers ----------------------------------------
     * Layout: [FRAMES_PER_BUFFER][packets_per_frame]
     * Two buffers: one being sent, one being refreshed.
     * NIC selection is baked into the headers at build time.
     * ----------------------------------------------------------------------- */
    uint64_t ppf  = config.packets_per_frame;
    uint64_t range = ppf / config.num_nics;  /* packets per NIC per frame (Mode B) */

    struct rte_mbuf ***bufs[2];
    for (int b = 0; b < 2; b++) {
        bufs[b] = malloc(FRAMES_PER_BUFFER * sizeof(struct rte_mbuf **));
        if (!bufs[b])
            rte_exit(EXIT_FAILURE, "malloc failed\n");
        for (int fr = 0; fr < FRAMES_PER_BUFFER; fr++) {
            bufs[b][fr] = malloc(ppf * sizeof(struct rte_mbuf *));
            if (!bufs[b][fr])
                rte_exit(EXIT_FAILURE, "malloc failed\n");
            for (uint64_t pk = 0; pk < ppf; pk++) {
                bufs[b][fr][pk] = rte_pktmbuf_alloc(mbuf_pool);
                if (!bufs[b][fr][pk])
                    rte_exit(EXIT_FAILURE, "packet alloc failed\n");
            }
        }
    }

    /* Fill initial frame numbers starting from config.starting_frame_number */
    uint64_t frame_counter = config.starting_frame_number;
    for (int fr = 0; fr < FRAMES_PER_BUFFER; fr++) {
        for (uint64_t pk = 0; pk < ppf; pk++) {
            uint16_t nic;
            if (config.rr_mode == RR_MODE_FRAME)
                nic = nic_for_frame(frame_counter);
            else
                nic = nic_for_packet((uint32_t)pk);

            fill_packet_headers(bufs[0][fr][pk], nic, frame_counter, (uint32_t)pk, data_len);
        }
        frame_counter++;
    }
    /* Pre-fill buffer 1 as well */
    for (int fr = 0; fr < FRAMES_PER_BUFFER; fr++) {
        for (uint64_t pk = 0; pk < ppf; pk++) {
            uint16_t nic;
            if (config.rr_mode == RR_MODE_FRAME)
                nic = nic_for_frame(frame_counter);
            else
                nic = nic_for_packet((uint32_t)pk);

            fill_packet_headers(bufs[1][fr][pk], nic, frame_counter, (uint32_t)pk, data_len);
        }
        frame_counter++;
    }

    printf("\nPress Enter to start sending packets...\n");
    getchar();

    /* --- Send loop ------------------------------------------------------- */
    uint64_t ticks_per_sec   = rte_get_tsc_hz();
    uint64_t last_tsc        = rte_get_tsc_cycles();
    uint64_t total_frames_sent = 0;
    int cur_buf              = 0;

    while (total_frames_sent < config.frames) {
        /* Send all frames in the current buffer */
        for (int fr = 0; fr < FRAMES_PER_BUFFER && total_frames_sent < config.frames; fr++) {
            for (uint64_t pk = 0; pk < ppf; pk++) {
                /* Drop logic */
                if (config.drop_packets > 0 && (rand() % 100) < (int)config.drop_packets)
                    continue;

                uint16_t nic;
                if (config.rr_mode == RR_MODE_FRAME)
                    nic = nic_for_frame(total_frames_sent + config.starting_frame_number);
                else
                    nic = nic_for_packet((uint32_t)pk);

                int nb_tx;
                do {
                    nb_tx = rte_eth_tx_burst(config.nics[nic].port_id, 0,
                                             &bufs[cur_buf][fr][pk], 1);
                } while (nb_tx != 1);

                rte_delay_us(config.interval);
            }
            total_frames_sent++;
        }

        /* Refresh the buffer just finished with new frame numbers */
        for (int fr = 0; fr < FRAMES_PER_BUFFER; fr++) {
            for (uint64_t pk = 0; pk < ppf; pk++)
                update_frame_number(bufs[cur_buf][fr][pk], frame_counter);
            frame_counter++;
        }

        cur_buf = 1 - cur_buf;

        if (total_frames_sent % 10000 == 0)
            printf("Sent %lu / %lu frames\n", total_frames_sent, config.frames);
    }

    float elapsed = (float)(rte_get_tsc_cycles() - last_tsc) / ticks_per_sec;
    printf("\nDone. Sent %lu frames in %.2f s (%.1f frames/s, %.2f Gbps)\n",
           total_frames_sent, elapsed,
           total_frames_sent / elapsed,
           (float)(total_frames_sent * ppf * data_len * 8) / (elapsed * 1e9));

    /* Cleanup */
    for (int b = 0; b < 2; b++) {
        for (int fr = 0; fr < FRAMES_PER_BUFFER; fr++)
            free(bufs[b][fr]);
        free(bufs[b]);
    }

    rte_eal_mp_wait_lcore();
    rte_eal_cleanup();
    return 0;
}
