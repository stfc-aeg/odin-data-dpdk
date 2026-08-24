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
#define DEFAULT_PACKETS_PER_FRAME   2
#define DEFAULT_PIXEL_DATA_LEN      6400   /* bytes of pixel data per packet (after 64B mercury hdr) */
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

typedef enum {
    PATTERN_INCR_PACKET = 0,  /* 0..N-1 within each packet (default) */
    PATTERN_INCR_FRAME  = 1,  /* continues across all packets in a frame */
    PATTERN_STATIC      = 2,  /* fixed fill value */
    PATTERN_RANDOM      = 3,  /* random values */
} pattern_t;

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
struct __rte_packed_begin mercury_hdr {
    rte_be64_t frame_number;
    rte_be64_t padding[6];
    rte_be32_t packet_number;
    uint8_t    markers;
    uint8_t    _unused_1;
    uint8_t    padding_bytes;
    uint8_t    readout_lane;
} __rte_packed_end;

/* Global configuration */
struct {
    uint64_t    frames;
    uint64_t    packets_per_frame;
    uint64_t    pixel_data_len;    /* bytes of pixel data per packet (after 64B mercury hdr) */
    uint64_t    interval;
    uint64_t    starting_frame_number;
    uint16_t    destination_port;
    uint16_t    source_port;
    uint64_t    drop_packets;
    uint64_t    drop_frames;

    uint32_t    num_nics;
    rr_mode_t   rr_mode;
    bool        bit_depth_12;
    pattern_t   pattern;
    uint16_t    fill_value;
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
    printf("  --pixel_bytes N    Pixel data bytes per packet, after the 64B mercury header\n");
    printf("                     (default: %d; UDP payload = 64 + N bytes)\n", DEFAULT_PIXEL_DATA_LEN);
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
    printf("  --12bit            Pack pixel payload as 12-bit values (2 pixels per 3 bytes)\n");
    printf("  --pattern MODE     Pixel fill pattern (default: incr_packet):\n");
    printf("                       incr_packet - 0..N-1 within each packet\n");
    printf("                       incr_frame  - incrementing across all packets in a frame\n");
    printf("                       static      - all pixels set to --fill_value\n");
    printf("                       random      - random pixel values\n");
    printf("  --fill_value N     Pixel value used with --pattern static (default: 0)\n");
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

    ret = rte_eth_dev_set_mtu(port_id, 9000);
    if (ret != 0)
        printf("WARN: port %u could not set MTU to 9000: %s\n",
               port_id, rte_strerror(-ret));
    else
        printf("Port %u MTU set to 9000\n", port_id);

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

    /* In 12-bit mode, 2 pixels pack into 3 bytes instead of 4, so the wire
     * payload is 3/4 of the 16-bit data_len. */
    uint64_t wire_len = config.bit_depth_12 ? (data_len / 2) * 3 / 2 : data_len;

    pkt->pkt_len  = l2 + l3 + l4 + sizeof(struct mercury_hdr) + wire_len;
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
    ip->total_length    = rte_cpu_to_be_16((uint16_t)(l3 + l4 + 64 /* mercury_hdr */ + wire_len));
    ip->packet_id       = 0;
    ip->fragment_offset = 0;
    ip->time_to_live    = 128;
    ip->next_proto_id   = IPPROTO_UDP;
    ip->hdr_checksum    = 0;
    ip->hdr_checksum    = rte_ipv4_cksum(ip);

    /* UDP */
    udp->dst_port  = rte_bswap16(config.destination_port);
    udp->src_port  = rte_bswap16(config.source_port);
    udp->dgram_len = rte_bswap16((uint16_t)(l4 + sizeof(struct mercury_hdr) + wire_len));
    udp->dgram_cksum = 0;

    /* Mercury header */
    memset(mhdr, 0, sizeof(*mhdr));
    mhdr->frame_number  = frame_number;
    mhdr->packet_number = packet_number;

    /* Payload: fill according to selected pattern */
    uint8_t *payload = (uint8_t *)((char *)mhdr + sizeof(struct mercury_hdr));
    uint64_t num_pixels = data_len / 2;  /* number of 16-bit pixels that fit in the payload */

    /* Build the 16-bit pixel array for this packet */
    uint16_t pixels[num_pixels];
    uint64_t frame_pixel_offset = packet_number * num_pixels;

    for (uint64_t i = 0; i < num_pixels; i++) {
        switch (config.pattern) {
            case PATTERN_INCR_PACKET: pixels[i] = (uint16_t)i;                          break;
            case PATTERN_INCR_FRAME:  pixels[i] = (uint16_t)(frame_pixel_offset + i);   break;
            case PATTERN_STATIC:      pixels[i] = config.fill_value;                    break;
            case PATTERN_RANDOM:      pixels[i] = (uint16_t)(rand() & 0xFFFF);          break;
        }
    }

    if (config.bit_depth_12) {
        /* Pack pairs of 12-bit pixel values into 3 bytes each.
         * pixel A bits [11:4] -> byte0, A[3:0]|B[11:8] -> byte1, B[7:0] -> byte2 */
        for (uint64_t i = 0; i < num_pixels; i += 2) {
            uint16_t a = pixels[i]       & 0x0FFF;
            uint16_t b = pixels[i + 1]   & 0x0FFF;
            *payload++ = (uint8_t)((a & 0x0FF0) >> 4);
            *payload++ = (uint8_t)(((a & 0x000F) << 4) | ((b & 0x0F00) >> 8));
            *payload++ = (uint8_t)(b & 0x00FF);
        }
    } else {
        memcpy(payload, pixels, num_pixels * sizeof(uint16_t));
    }
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
    mhdr->frame_number = frame_number;
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
    config.pixel_data_len       = DEFAULT_PIXEL_DATA_LEN;
    config.interval             = DEFAULT_INTERVAL;
    config.starting_frame_number = DEFAULT_STARTING_FRAME;
    config.destination_port     = DEFAULT_DEST_PORT;
    config.source_port          = DEFAULT_SRC_PORT;
    config.drop_packets         = 0;
    config.drop_frames          = 0;
    config.num_nics             = 1;
    config.rr_mode              = RR_MODE_FRAME;
    config.bit_depth_12         = false;
    config.pattern              = PATTERN_INCR_PACKET;
    config.fill_value           = 0;
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
        {"pixel_bytes",     required_argument, NULL, 'z'},
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
        {"12bit",           no_argument,       NULL, 'q'},
        {"pattern",         required_argument, NULL, 'P'},
        {"fill_value",      required_argument, NULL, 'F'},
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
            case 'z': config.pixel_data_len         = (uint64_t)atol(optarg); break;
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
            case 'q': config.bit_depth_12 = true; break;
            case 'P':
                if      (strcmp(optarg, "incr_packet") == 0) config.pattern = PATTERN_INCR_PACKET;
                else if (strcmp(optarg, "incr_frame")  == 0) config.pattern = PATTERN_INCR_FRAME;
                else if (strcmp(optarg, "static")       == 0) config.pattern = PATTERN_STATIC;
                else if (strcmp(optarg, "random")       == 0) config.pattern = PATTERN_RANDOM;
                else {
                    fprintf(stderr, "ERROR: --pattern must be incr_packet, incr_frame, static, or random\n");
                    rte_eal_cleanup();
                    return -1;
                }
                break;
            case 'F': config.fill_value = (uint16_t)atoi(optarg); break;
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
    printf("  pixel_bytes:       %lu  (UDP payload = %lu B)\n",
           config.pixel_data_len, (uint64_t)64 + config.pixel_data_len);
    printf("  interval_us:       %lu\n", config.interval);
    printf("  starting_frame:    %lu\n", config.starting_frame_number);
    printf("  num_nics:          %u\n",  config.num_nics);
    printf("  rr_mode:           %s\n",  config.rr_mode == RR_MODE_FRAME ? "frame" : "packet");
    printf("  bit_depth:         %s\n",  config.bit_depth_12 ? "12bit" : "16bit");
    static const char *pattern_names[] = {"incr_packet", "incr_frame", "static", "random"};
    printf("  pattern:           %s\n",  pattern_names[config.pattern]);
    if (config.pattern == PATTERN_STATIC)
        printf("  fill_value:        %u\n", config.fill_value);

    /* --- Memory pool ----------------------------------------------------- */
    const uint64_t data_len = config.pixel_data_len;
    const uint64_t pkt_size = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                              sizeof(struct rte_udp_hdr) + 64 /* mercury_hdr */ + data_len;
    if (pkt_size > JUMBO_FRAME_ELEMENT_SIZE) {
        fprintf(stderr, "ERROR: packet size %lu B exceeds JUMBO_FRAME_ELEMENT_SIZE %u B. "
                "Reduce --pixel_bytes or increase JUMBO_FRAME_ELEMENT_SIZE.\n",
                pkt_size, JUMBO_FRAME_ELEMENT_SIZE);
        rte_eal_cleanup();
        return -1;
    }
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
    printf("\n--- Port initialisation (%u NIC(s)) ---\n", config.num_nics);
    for (uint32_t n = 0; n < config.num_nics; n++) {
        const char *pcie = config.nics[n].pcie_addr;

        if (pcie[0] == '\0') {
            /* No PCIe address: use port n directly (single-NIC fallback) */
            printf("NIC %u: no pcie_addr, using DPDK port %u\n", n, n);
            config.nics[n].port_id = (uint16_t)n;
        } else {
            printf("NIC %u: hotplugging PCIe %s ...\n", n, pcie);
            ret = rte_eal_hotplug_add("pci", pcie, "");
            if (ret < 0 && ret != -EEXIST) {
                rte_exit(EXIT_FAILURE,
                         "Cannot hotplug NIC %u (PCIe %s): %s\n",
                         n, pcie, rte_strerror(-ret));
            }
            if (ret == -EEXIST)
                printf("NIC %u: PCIe %s already attached\n", n, pcie);

            uint16_t port_id;
            ret = rte_eth_dev_get_port_by_name(pcie, &port_id);
            if (ret != 0) {
                rte_exit(EXIT_FAILURE,
                         "Cannot find port for NIC %u (PCIe %s)\n", n, pcie);
            }
            config.nics[n].port_id = port_id;
            printf("NIC %u: PCIe %s -> DPDK port %u\n", n, pcie, port_id);
        }

        printf("NIC %u: initialising port %u ...\n", n, config.nics[n].port_id);
        if (port_init(config.nics[n].port_id, mbuf_pool) != 0)
            rte_exit(EXIT_FAILURE, "Cannot init port %u (NIC %u)\n",
                     config.nics[n].port_id, n);
        printf("NIC %u: port %u ready  src=%s (%s)  dst=%s (%s)\n",
               n, config.nics[n].port_id,
               config.nics[n].src_ip,  config.nics[n].src_mac,
               config.nics[n].dest_ip, config.nics[n].dest_mac);
    }
    printf("--- All ports ready ---\n\n");

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
    printf("Pre-building packet buffers (2 x %d frames x %lu packets) ...\n",
           FRAMES_PER_BUFFER, ppf);
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
    printf("Buffer 0 ready (frames %lu..%lu)\n",
           config.starting_frame_number, frame_counter - 1);

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

    printf("Buffer 1 ready (frames %lu..%lu)\n",
           config.starting_frame_number + FRAMES_PER_BUFFER, frame_counter - 1);

    printf("\nPress Enter to start sending packets...\n");
    getchar();

    /* --- Send loop ------------------------------------------------------- */
    uint64_t ticks_per_sec     = rte_get_tsc_hz();
    uint64_t last_tsc          = rte_get_tsc_cycles();
    uint64_t total_frames_sent = 0;
    uint64_t total_pkts_sent   = 0;
    uint64_t total_pkts_dropped = 0;
    uint64_t tx_retries        = 0;
    int cur_buf                = 0;
    bool first_packet_sent     = false;

#define TX_MAX_RETRIES 100000UL

    printf("Starting send: %lu frames, %lu packets/frame, interval %lu us\n",
           config.frames, ppf, config.interval);

    while (total_frames_sent < config.frames) {
        /* Send all frames in the current buffer */
        for (int fr = 0; fr < FRAMES_PER_BUFFER && total_frames_sent < config.frames; fr++) {

            if (config.drop_frames > 0 && (rand() % 100) < (int)config.drop_frames) {
                total_frames_sent++;
                total_pkts_dropped += ppf;
                continue;
            }

            for (uint64_t pk = 0; pk < ppf; pk++) {
                /* Drop logic */
                if (config.drop_packets > 0 && (rand() % 100) < (int)config.drop_packets) {
                    total_pkts_dropped++;
                    continue;
                }

                uint16_t nic;
                if (config.rr_mode == RR_MODE_FRAME)
                    nic = nic_for_frame(total_frames_sent + config.starting_frame_number);
                else
                    nic = nic_for_packet((uint32_t)pk);

                uint64_t retries = 0;
                int nb_tx;
                do {
                    nb_tx = rte_eth_tx_burst(config.nics[nic].port_id, 0,
                                             &bufs[cur_buf][fr][pk], 1);
                    if (nb_tx != 1) {
                        retries++;
                        tx_retries++;
                        if (retries == 1000)
                            printf("WARN: TX stalled on NIC %u (port %u), "
                                   "frame %lu pkt %lu — retrying...\n",
                                   nic, config.nics[nic].port_id,
                                   total_frames_sent + config.starting_frame_number, pk);
                        if (retries >= TX_MAX_RETRIES) {
                            printf("ERROR: TX gave up after %lu retries on NIC %u (port %u), "
                                   "frame %lu pkt %lu — aborting\n",
                                   retries, nic, config.nics[nic].port_id,
                                   total_frames_sent + config.starting_frame_number, pk);
                            goto send_done;
                        }
                    }
                } while (nb_tx != 1);

                if (!first_packet_sent) {
                    printf("First packet sent: frame %lu pkt 0 via NIC %u (port %u)\n",
                           config.starting_frame_number, nic, config.nics[nic].port_id);
                    first_packet_sent = true;
                }

                total_pkts_sent++;
                rte_delay_us(config.interval);
            }
            total_frames_sent++;

            /* Per-frame progress (first 5 frames, then every 1000) */
            if (total_frames_sent <= 5 || total_frames_sent % 1000 == 0)
                printf("  frame %lu/%lu sent  (pkts sent=%lu dropped=%lu retries=%lu)\n",
                       total_frames_sent, config.frames,
                       total_pkts_sent, total_pkts_dropped, tx_retries);
        }

        /* Refresh the buffer just finished with new frame numbers */
        for (int fr = 0; fr < FRAMES_PER_BUFFER; fr++) {
            for (uint64_t pk = 0; pk < ppf; pk++)
                update_frame_number(bufs[cur_buf][fr][pk], frame_counter);
            frame_counter++;
        }
        printf("  [buf %d refreshed up to frame %lu]\n", cur_buf, frame_counter - 1);

        cur_buf = 1 - cur_buf;
    }

send_done:

    float elapsed = (float)(rte_get_tsc_cycles() - last_tsc) / ticks_per_sec;
    printf("\n--- Done ---\n");
    printf("  Frames sent:    %lu / %lu\n", total_frames_sent, config.frames);
    printf("  Packets sent:   %lu\n", total_pkts_sent);
    printf("  Packets dropped:%lu\n", total_pkts_dropped);
    printf("  TX retries:     %lu\n", tx_retries);
    printf("  Elapsed:        %.2f s\n", elapsed);
    if (elapsed > 0) {
        printf("  Throughput:     %.1f frames/s  %.2f Gbps\n",
               total_frames_sent / elapsed,
               (double)(total_pkts_sent * data_len * 8) / (elapsed * 1e9));
    }

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
