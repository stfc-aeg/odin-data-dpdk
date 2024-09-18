#include <stdint.h>
#include <sys/queue.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <inttypes.h>
#include <getopt.h>
#include <math.h>

#include <stdint.h>
#include <inttypes.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <pthread.h>
#include <string.h>
#include <rte_memzone.h>
#include <rte_errno.h>
#include <rte_hexdump.h>
#include <stdbool.h>
#include <getopt.h>
#include <rte_byteorder_64.h>

struct mercury_hdr {
    rte_be64_t frame_number;
    rte_be64_t padding[6];
    rte_be32_t packet_number;
    uint8_t markers;
    uint8_t _unused_1;
    uint8_t padding_bytes;
    uint8_t readout_lane;
}__rte_packed;

struct mercury_data{
	uint16_t payload;
};


struct config_struct {
    uint64_t buffers; 
    uint64_t frames;
    uint64_t channels;
	uint64_t packets_per_frame;
    uint64_t interval;
	uint64_t starting_frame_number;
	uint64_t number_of_frames;
	char destination_ip_address[20];
	char destination_mac_address[20];
    uint16_t destination_port;
	char source_ip_address[20];
	char source_mac_address[20];
    uint16_t source_port;
	uint64_t test_pattern_mode;
	uint64_t drop_packets;
	uint64_t drop_frames;
} __rte_packed;



// Defaults

#define DEFAULT_INTERVAL 1000
#define DEFAULT_STARTING_FRAME_NUMBER 0
#define DEFAULT_NUMBER_OF_FRAMES 1000
#define DEFAULT_DEST_IP_ADDR "10.100.0.6"
#define DEFAULT_DEST_MAC_ADDR "08:c0:eb:f8:28:7c"
#define DEFAULT_DEST_PORT 1234
#define DEFAULT_SOURCE_IP_ADDR "10.100.0.5"
#define DEFAULT_SOURCE_MAC_ADDR "08:c0:eb:f8:28:6c"
#define DEFAULT_SOURCE_PORT 1234
#define DEFAULT_TEST_PATTERN_MODE 1
#define DEFAULT_DROP_PACKETS 0
#define DEFAULT_DROP_FRAMES 0

#define MAX_BUFFERS 10
#define MAX_FRAMES 10000
#define MAX_PACKETS 10000
#define UDP_OVERHEAD 96
#define FRAMES_PER_BUFFER 400

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 32768

#define JUMBO_FRAME_ELEMENT_SIZE 0x2600
struct config_struct config_;

int l2_len = sizeof(struct rte_ether_hdr);
int l3_len = sizeof(struct rte_ipv4_hdr);
int len_4 = sizeof (struct rte_udp_hdr);


struct rte_ring *buffer_to_update[MAX_BUFFERS];
struct rte_ring *buffer_updated[MAX_BUFFERS];

uint64_t counter = 0;

static const struct rte_eth_conf port_conf_default = {
	.rxmode = { .max_lro_pkt_size = JUMBO_FRAME_ELEMENT_SIZE,
				.offloads =  RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
						RTE_ETH_TX_OFFLOAD_UDP_CKSUM }	
};

uint64_t GbToDelay(double x) {
    return (uint64_t)round(15868.90 / x - 0.0762 * x + 4.56);
}

uint32_t roundUpTo2PowerMinus1(uint32_t n) {
    if (n == 0) {
        return 0;
    }

    // Find the position of the most significant bit set
    uint32_t msb_pos = 31 - rte_bsf32(~n);

    // Construct the number 2^(msb_pos + 1) - 1
    return (1U << (msb_pos + 1)) - 1;
}


static inline int
port_init(struct rte_mempool *mbuf_pool)
{
	struct rte_eth_conf port_conf = port_conf_default;
	const uint16_t rx_rings = 1, tx_rings = 1;
	int retval;
	uint16_t q;


	retval = rte_eth_dev_configure(0, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	/* Allocate and set up 1 RX queue per Ethernet port. */
	for (q = 0; q < rx_rings; q++) {
		retval = rte_eth_rx_queue_setup(0, q, RX_RING_SIZE,
				rte_eth_dev_socket_id(0), NULL, mbuf_pool);
		if (retval < 0)
			return retval;
	}

	/* Allocate and set up 1 TX queue per Ethernet port. */
	for (q = 0; q < tx_rings; q++) {
		retval = rte_eth_tx_queue_setup(0, q, TX_RING_SIZE,
				rte_eth_dev_socket_id(0), NULL);
		if (retval < 0)
			return retval;
	}

	/* Start the Ethernet port. */
	retval = rte_eth_dev_start(0);
	if (retval < 0)
		return retval;

	

	struct rte_ether_addr addr;
	retval = rte_eth_macaddr_get(0, &addr);
	if (retval != 0)
		return retval;

	printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
			   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
			0,
			addr.addr_bytes[0], addr.addr_bytes[1],
			addr.addr_bytes[2], addr.addr_bytes[3],
			addr.addr_bytes[4], addr.addr_bytes[5]);

	return 0;
}

int main(int argc, char **argv) {
    // Initialize the Environment Abstraction Layer (EAL)
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");
    }

    argc -= ret;
	argv += ret;

    config_.interval = DEFAULT_INTERVAL;
	config_.starting_frame_number = DEFAULT_STARTING_FRAME_NUMBER;
	config_.number_of_frames = DEFAULT_NUMBER_OF_FRAMES;
	strncpy(config_.destination_ip_address, DEFAULT_DEST_IP_ADDR, sizeof(config_.destination_ip_address));
	strncpy(config_.destination_mac_address, DEFAULT_DEST_MAC_ADDR, sizeof(config_.destination_mac_address));
	config_.destination_port = DEFAULT_DEST_PORT;
	strncpy(config_.source_ip_address, DEFAULT_SOURCE_IP_ADDR, sizeof(config_.source_ip_address));
	strncpy(config_.source_mac_address, DEFAULT_SOURCE_MAC_ADDR, sizeof(config_.source_mac_address));
	config_.source_port = DEFAULT_SOURCE_PORT;
	config_.test_pattern_mode = DEFAULT_TEST_PATTERN_MODE;
	config_.drop_frames = DEFAULT_DROP_FRAMES;
	config_.drop_packets = DEFAULT_DROP_PACKETS;
    config_.packets_per_frame = 250;

    // Parse user config from command line
    int opt;
    int option_index;

    static struct option long_option[] = {
        {"interval", required_argument, NULL, 'i'},
        {"start_frame", required_argument, NULL, 's'},
        {"frames", required_argument, NULL, 'f'},
        {"dest_ip", required_argument, NULL, 'd'},
        {"dest_mac", required_argument, NULL, 'm'},
        {"src_ip", required_argument, NULL, 'x'},
        {"src_mac", required_argument, NULL, 'y'},
        {"drop_packet", required_argument, NULL, 'p'},
        {"drop_frame", required_argument, NULL, 'r'},
        {"src_port", required_argument, NULL, 'u'},
        {"dst_port", required_argument, NULL, 'v'},
        {"test_pattern", required_argument, NULL, 't'},
        {"help", no_argument, NULL, 'h'},
        {"bandwith_test", no_argument, NULL, 'b'},
        {"channels", required_argument, NULL, 'c'},
        {NULL, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "", long_option, &option_index)) != -1) {
        switch (opt) {
            case 'i':
                printf("Found interval argument: %s\n", optarg);
                config_.interval = atoi(optarg);
                break;
            case 's':
                printf("Found start_frame argument: %s\n", optarg);
                config_.starting_frame_number = atoi(optarg);
                break;
            case 'f':
                printf("Found frames argument: %s\n", optarg);
                config_.frames = atoi(optarg);
                break;
            case 'd':
                printf("Found dest_ip argument: %s\n", optarg);
                strncpy(config_.destination_ip_address, optarg, sizeof(config_.destination_ip_address));
                break;
            case 'm':
                printf("Found dest_mac argument: %s\n", optarg);
                strncpy(config_.destination_mac_address, optarg, sizeof(config_.destination_mac_address));
                break;
            case 'x':
                printf("Found src_ip argument: %s\n", optarg);
                strncpy(config_.source_ip_address, optarg, sizeof(config_.source_ip_address));
                break;
            case 'y':
                printf("Found src_mac argument: %s\n", optarg);
                strncpy(config_.source_mac_address, optarg, sizeof(config_.source_mac_address));
                break;
            case 'p':
                printf("Found drop_packet argument: %s\n", optarg);
                config_.drop_packets = atoi(optarg);
                break;
            case 'r':
                printf("Found drop_frame argument: %s\n", optarg);
                config_.drop_frames = atoi(optarg);
                break;
            case 'u':
                printf("Found src_port argument: %s\n", optarg);
                config_.source_port = atoi(optarg);
                break;
            case 'v':
                printf("Found dst_port argument: %s\n", optarg);
                config_.destination_port = atoi(optarg);
                break;
            case 't':
                config_.test_pattern_mode = atoi(optarg);
                printf("Found test pattern argument: %ld\n", config_.test_pattern_mode);
                break;
            case 'b':
                printf("Found bandwith_test argument\n");
                config_.interval = GbToDelay(1);
                break;
            case 'c':
				printf("Found channels argument\n");
                config_.channels = atoi(optarg);
                break;
            case 'h':
                // Display help message
                printf("\n\n\n");
				printf("--interval : Time delay in seconds between frames \n");
				printf("--start_frame : Frame number to start sending from \n");
				printf("--frames : number of frames to send \n");
				printf("--dest_ip : Destination Ip address in the format xxx.xxx.xxx.xxx \n");
				printf("--dest_mac : Destination MAC address in the format XX:XX:XX:XX:XX:XX \n");
				printf("--dest_port : Destination port to use 0 - 65535 \n");
				printf("--src_ip : Source Ip address in the format xxx.xxx.xxx.xxx \n");
				printf("--src_mac : Source MAC address in the format XX:XX:XX:XX:XX:XX \n");
				printf("--src_port : Source port to use 0 - 65535 \n");
				printf("--drop_packet : A value between 0-100 of the chance to drop a packet \n");
				printf("--drop_frame : A value between 0-100 of the chance to drop packets in that frame \n");
				printf("--test_pattern : 1 - repeating test pattern, 0 - simulated beam \n");
				printf("--help : Display this message \n\n\n");

                rte_eal_cleanup();
                exit(0);
            default:
                return -1;
        }
    }

	config_.buffers = 1;

    uint64_t data_len = 8000;

    printf("data len: %ld", data_len);



    struct rte_ether_hdr *eth_hdr;
	struct rte_ipv4_hdr *ip_hdr;
    struct rte_udp_hdr *udp_hdr;
    struct mercury_hdr *mercury_h;
	uint16_t *mercury_data;
    
    struct rte_mempool *mbuf_pool;

	printf("PPF: %ld, buf size: %ld\n", config_.packets_per_frame, config_.buffers * FRAMES_PER_BUFFER * config_.packets_per_frame);
	


    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", 100 * 5000 * 2, RTE_MEMPOOL_CACHE_MAX_SIZE, RTE_MBUF_PRIV_ALIGN, JUMBO_FRAME_ELEMENT_SIZE, rte_socket_id());

    printf("Made mbuf pool\n");

	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create mbuf pool, check permissions\n");

	if (port_init(mbuf_pool) != 0)
		rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu8 "\n", 0);

    printf("Port init\n");

    struct rte_mbuf *PacketBuffs[config_.buffers][FRAMES_PER_BUFFER][config_.packets_per_frame];

    printf("delcare rte_mbuf\n");

    rte_be64_t frame_counter = 0;

    uint32_t buf;

    printf("Allocating pkts: %d \n",rte_pktmbuf_alloc_bulk(mbuf_pool, **PacketBuffs, config_.buffers * FRAMES_PER_BUFFER * config_.packets_per_frame));
    uint16_t packet_data = 0;
    for (uint64_t bufs = 0; bufs < config_.buffers; bufs++)
    {
        for (uint64_t frames = 0; frames < FRAMES_PER_BUFFER; frames++)
        {

            uint16_t pixel_data = 0;
            for (rte_be32_t packets = 0; packets < config_.packets_per_frame; packets++)
            {

                // printf("New packet: size of packet: %d\n", rte_pktmbuf_pkt_len(PacketBuffs[bufs][frames][packets]));

                if( PacketBuffs[bufs][frames][packets] == NULL)
                {
                    printf("ERROR: Failed to allocate in mbuf\n");
                    rte_eal_cleanup();
                    exit(0);
                }
                

                //printf("Packet alloc\n");
                PacketBuffs[bufs][frames][packets]->pkt_len = l2_len + l3_len + len_4 + data_len + 64;

                PacketBuffs[bufs][frames][packets]->data_len = l2_len + l3_len + len_4 + data_len + 64;
                //printf("about to mtod\n");
                eth_hdr = rte_pktmbuf_mtod(PacketBuffs[bufs][frames][packets], struct rte_ether_hdr*);

                // printf("size of eth_hdr: %ld\n", sizeof(eth_hdr));


                //printf("Packet mtod done\n");
                // printf("eth_hdr: %p\n", eth_hdr);

                ip_hdr = (struct rte_ipv4_hdr *) ((char *)eth_hdr + l2_len);
                // printf("size of ip_hdr: %ld\n", sizeof(ip_hdr));
                // printf("ip_hdr: %p\n", ip_hdr);

			    udp_hdr = (struct rte_udp_hdr *) ((char *)ip_hdr + l3_len);
                // printf("size of udp_hdr: %ld\n", sizeof(udp_hdr));
                // printf("udp_hdr: %p\n", udp_hdr);
    
			    mercury_h = (struct mercury_hdr *) ((char *)udp_hdr + len_4);
                // printf("size of mercury_h: %ld\n", sizeof(mercury_h));
                // printf("mercury_h: %p\n", mercury_h);
    
			    mercury_data = (uint16_t *) ((char *)mercury_h + 64);
                // printf("size of mercury_data: %ld\n", sizeof(mercury_data));
                // printf("mercury_data: %p\n", mercury_data);

                // printf("mercury packet: [%ld][%ld][%d] packet pointer: %p header pointer: %p data pointer: %p\n",bufs, frames, packets,PacketBuffs[bufs][frames][packets] , mercury_h, mercury_data);

                //printf("Set frame number: %ld \n", frame_counter);
                mercury_h->frame_number = frame_counter;

                //printf("Frame pointer: %p, frame number: %ld, FN pointer: %p\n",PacketBuffs[bufs][frames][packets], mercury_h->frame_number, &(mercury_h->frame_number));

                //printf("between number and pointer: %ld", (char*) PacketBuffs[bufs][frames][packets] - (char*) mercury_h);

                //rintf("Made headers\n");

                // Set SOF & EOF markers 

                mercury_h->packet_number = rte_bswap32(packets);
                rte_ether_unformat_addr(config_.source_mac_address, (void*) &eth_hdr->src_addr);

                rte_ether_unformat_addr(config_.destination_mac_address, (void*) &eth_hdr->dst_addr);

                inet_pton(AF_INET, config_.destination_ip_address, &buf);

                ip_hdr->dst_addr = buf;

                inet_pton(AF_INET, config_.source_ip_address, &buf);

                ip_hdr->src_addr = buf;


                eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
                udp_hdr->dst_port = rte_bswap16(config_.destination_port);
                udp_hdr->src_port = rte_bswap16(config_.source_port);
                udp_hdr->dgram_len = rte_bswap16(data_len + 72);

                ip_hdr->fragment_offset = 0;
                ip_hdr->ihl = 5;
                ip_hdr->next_proto_id = 17;
                ip_hdr->packet_id = 29692;  // Consider using a counter or random value here
                ip_hdr->time_to_live = 128;
                ip_hdr->total_length = 0;
                ip_hdr->type_of_service = 0;
                ip_hdr->version = 4;
                ip_hdr->version_ihl = RTE_IPV4_VHL_DEF;

                //printf("about to make pixel data\n");

                for(uint64_t pixel_index = 0; pixel_index < (data_len/2); pixel_index++)
                {
                    *mercury_data = packet_data;
                    mercury_data++;
                    packet_data++;
                    if (packet_data == 65535)
                        packet_data = 0;
                
                }

                mercury_data = (uint16_t *) ((char *)mercury_h + 64);
			
                int i;
                if(i == 0)
                {
                    for(int index = 0; index < 80; index ++)
                    {
                        *mercury_data = (uint16_t) frame_counter;
                    
                        mercury_data++;
                    }
                }

                //printf("Made pixel data\n");

                // printf("PACKET INFO: \n");
				// printf("%X\n", ip_hdr->dst_addr);
				// printf("%X\n", ip_hdr->fragment_offset);
				// printf("%X\n", ip_hdr->hdr_checksum);
				// printf("%X\n", ip_hdr->ihl);
				// printf("%X\n", ip_hdr->next_proto_id);
				// printf("%X\n", ip_hdr->packet_id);
				// printf("%X\n", ip_hdr->src_addr);
				// printf("%X\n", ip_hdr->time_to_live);
				// printf("%X\n", ip_hdr->total_length);
				// printf("%X\n", ip_hdr->type_of_service);
				// printf("%X\n", ip_hdr->version);
				// printf("%X\n", ip_hdr->version_ihl);
				// printf("%X\n", udp_hdr->dgram_cksum);
				// printf("%X\n", udp_hdr->dgram_len);
				// printf("%X\n", udp_hdr->dst_port);
				// printf("%X\n", udp_hdr->src_port);
				// printf("%X\n", eth_hdr->ether_type);
				// printf("size of packet: %d\n", rte_pktmbuf_pkt_len(PacketBuffs[bufs][frames][packets]));
                

                
            }
            //printf("finished Frame: %ld", frame_counter);
            frame_counter++;
            
        }
    }



    printf("Press any key to start sending packets...\n");
    getchar();

    
	uint64_t ticks_per_sec = rte_get_tsc_hz();
    uint64_t delayer = rte_get_tsc_hz();

    rte_be64_t total_frames_sent = FRAMES_PER_BUFFER;
    int nb_tx;
    int frame_buffer_index = 0;
    uint64_t packet_send_loops = 0;

    // struct rte_mbuf *PacketBuffs[config_.buffers][config_.frames][config_.packets_per_frame];

    // for(int g = 0; g < config_.buffers; g++)
    // {
    //     printf("pointer for buffer[%d]: %p\n",g, PacketBuffs[g]);
    // }

    struct rte_eth_stats stats;
    uint16_t port_id = 0; // replace with your port ID
    
    uint64_t last = rte_get_tsc_cycles();
    while (true) {
        //printf("Main loop: sending packets\n" );
        // Send all the packets in the current buffer

        uint64_t temp_sent = 0;

        while(temp_sent < FRAMES_PER_BUFFER * config_.packets_per_frame)
        {
            nb_tx = rte_eth_tx_burst(0, 0, *PacketBuffs[counter] + temp_sent, 1);
            //if not all packets got queued then try and resend them
            while(nb_tx != 1){
                
                nb_tx = rte_eth_tx_burst(0, 0, *PacketBuffs[counter] + temp_sent, 1);
                
            }

            temp_sent++;
            //rte_delay_us(1);
            rte_delay_us(config_.interval);
        }

        
        
        //printf("Updating buffer: %ld starting with frame number: %ld\n", (counter + 1) % config_.buffers, total_frames_sent);
        for(uint64_t pkts_update = 0; pkts_update < (FRAMES_PER_BUFFER); pkts_update++)
        {
            for(int pkt_per_frame = 0; pkt_per_frame < config_.packets_per_frame; pkt_per_frame++)
            {
                
                eth_hdr = rte_pktmbuf_mtod(PacketBuffs[(counter + 1) % config_.buffers][pkts_update][pkt_per_frame], struct rte_ether_hdr*);
                mercury_h = (struct mercury_hdr*)((char*)eth_hdr + l2_len + l3_len + len_4);

                mercury_h->frame_number = total_frames_sent;

                //printf("Packet pointer: %p, packet: %ld, frame number: %ld : %ld, FN pointer: %p\n", PacketBuffs[(counter + 1) % config_.buffers][pkts_update][pkt_per_frame],pkt_per_frame, mercury_h->frame_number,pkts_update, &(mercury_h->frame_number));
                mercury_data = (uint16_t *) ((char *)mercury_h + 64);

                for(int index = 0; index < 80; index ++)
                {
                    *mercury_data = (uint16_t) total_frames_sent;
                
                    mercury_data++;
                }

            }
            total_frames_sent++;
            
        }

        //printf("updated buffer %ld\n", (counter + 1) % config_.buffers);


        // Round robin counter
        counter = (counter + 1) % config_.buffers;

        // printf("Main loop: Counter now on: %ld\n", counter );

        // uint64_t delayer = rte_get_tsc_cycles();
        // while(rte_get_tsc_cycles() < delayer + 10000)
        // {
        //     continue;
        // }

        if(total_frames_sent % 1000000 == 0)
        {
            printf("sent frame: %ld\n", total_frames_sent);
        }
        
        
        // Check if the total number of frames has been sent
        if (total_frames_sent >= config_.frames + 5000) {
            total_frames_sent = total_frames_sent - 5000;
            float time_taken = (float)(rte_get_tsc_cycles() - last) / ticks_per_sec;
            printf("Sent %ld frames in %f seconds! (%f frames per second) At data rate of %f Gb/s\n", total_frames_sent, time_taken, (float)total_frames_sent/time_taken, (float)((total_frames_sent) * config_.packets_per_frame * data_len * 8) / (time_taken * 1e9) );

            break;
        }
    }

    rte_eal_mp_wait_lcore();

    return 0;
}
