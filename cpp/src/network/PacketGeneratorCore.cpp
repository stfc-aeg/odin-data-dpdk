#include "PacketGeneratorCore.h"
#include "DataSource.h"
#include "DataSourceLoader.h"
#include "DpdkSharedBufferFrame.h"
#include "DpdkUtils.h"
#include "GeneratedDataSource.h"
#include "HDF5DataSource.h"
#include </usr/lib/x86_64-linux-gnu/hdf5/serial/include/hdf5.h>
#include <blosc.h>
#include <chrono>
#include <thread>

namespace FrameProcessor {
static const unsigned int DEFAULT_RING_SIZE = 16384;

// Finds the bytes per pixel for specified data type
size_t get_bytes_per_pixel(FrameProcessor::DataType data_type)
{
    // Map internal enum to actual byte size per pixel
    switch (data_type) {
    case FrameProcessor::DataType::raw_8bit:
        return sizeof(uint8_t);

    case FrameProcessor::DataType::raw_16bit:
        return sizeof(uint16_t);

    case FrameProcessor::DataType::raw_32bit:
        return sizeof(uint32_t);

    case FrameProcessor::DataType::raw_64bit:
        return sizeof(uint64_t);

    default:
        throw std::runtime_error("Unsupported frame data type");
    }
}

RoundRobinMode parse_round_robin_mode(const std::string& mode)
{
    if (mode == "packet") {
        return RoundRobinMode::Packet;
    } else if (mode == "frame") {
        return RoundRobinMode::Frame;
    } else {
        throw std::runtime_error(
            "PacketGeneratorCore: invalid round_robin_mode '" + mode + "' (expected 'frame' or 'packet')"
        );
    }
}

PacketGeneratorCore::PacketGeneratorCore(int fb_idx, int socket_id, DpdkWorkCoreReferences& dpdkWorkCoreReferences) :
    DpdkWorkerCore(socket_id, dpdkWorkCoreReferences.stream_id, dpdkWorkCoreReferences.config_key),
    logger_(Logger::getLogger("FP.PacketGeneratorCore")),
    proc_idx_(fb_idx),
    decoder_(dynamic_cast<PacketProtocolDecoder*>(dpdkWorkCoreReferences.decoder)),
    frame_callback_(dpdkWorkCoreReferences.frame_callback),
    shared_buf_(dpdkWorkCoreReferences.shared_buf),
    packet_tx_(false),
    total_packets_(0),
    packets_sent_(0),
    dropped_packets_(0),
    mbuf_alloc_failures_(0),
    tx_ring_full_waits_(0),
    frames_generated_(0),
    last_frame_number_(0),
    mean_frame_us_(0),
    min_frame_us_(0),
    max_frame_us_(0),
    max_frame_us_all_time_(0),
    packets_per_second_(0),
    frames_per_second_(0),
    busy_percentage_(0.0),
    core_status_("constructing")
{

    // Get the configuration container for this worker
    config_.resolve(dpdkWorkCoreReferences.core_config, dpdkWorkCoreReferences.config_key);
    round_robin_mode_ = parse_round_robin_mode(config_.round_robin_mode);

    // Load the relevant DataSource class
    const FrameProcessor::DataType data_type = decoder_->get_frame_bit_depth();

    const auto& data_source_config = config_.data_source_config();

    if (!data_source_config.HasMember("data_source") || !data_source_config["data_source"].IsString()) {
        throw std::runtime_error("PacketGeneratorCore: data_source_config missing "
                                 "required 'data_source' field");
    }

    data_source_ = FrameProcessor::DataSourceLoader<FrameProcessor::DataSource>::load_class(
        data_source_config["data_source"].GetString(), decoder_, data_source_config
    );

    // data_source_ provides the frame data at runtime
    if (!data_source_) {
        throw std::runtime_error(
            "Failed to load DataSource: " + std::string(data_source_config["data_source"].GetString())
        );
    }

    // Class loading - reference it as base class
    // Base class must have all methods, doesn't care how it is specialised

    LOG4CXX_INFO(
        logger_,
        "FP.PacketGeneratorCore " << proc_idx_ << " Created with config:"
                                  << " | core_name: " << config_.core_name << " | num_cores: " << config_.num_cores
                                  << " | connect: " << config_.connect << " | upstream_core: " << config_.upstream_core
                                  << " | num_downsteam_cores: " << config_.num_downstream_cores
    );

    // Searches for the clear-frames ring and creates it if not existing
    // This ring holds pointers to free frame buffers provided by the SMB
    std::string clear_frames_ring_name = ring_name_clear_frames(socket_id_);
    clear_frames_ring_ = rte_ring_lookup(clear_frames_ring_name.c_str());
    if (clear_frames_ring_ == NULL) {
        unsigned int clear_frames_ring_size = nearest_power_two(shared_buf_->get_num_buffers());
        LOG4CXX_DEBUG_LEVEL(
            2, logger_,
            "Creating frame processed ring name " << clear_frames_ring_name << " of size " << clear_frames_ring_size
                                                  << " numa node: " << socket_id_
        );
        clear_frames_ring_ = rte_ring_create(clear_frames_ring_name.c_str(), clear_frames_ring_size, socket_id_, 0);
        if (clear_frames_ring_ == NULL) {
            LOG4CXX_ERROR(
                logger_,
                "Error creating frame processed ring " << clear_frames_ring_name << " : " << rte_strerror(rte_errno)
            );
        } else {
            // Populate the ring with hugepages memory locations to the SMB
            for (int element = 0; element < shared_buf_->get_num_buffers(); element++) {
                // Push each hugepage buffer address into the ring for reuse
                rte_ring_enqueue(clear_frames_ring_, shared_buf_->get_buffer_address(element));
            }
        }
    }

    core_status_ = "ready";
}

PacketGeneratorCore::~PacketGeneratorCore(void)
{
    LOG4CXX_DEBUG_LEVEL(2, logger_, "PacketGeneratorCore destructor");
    stop();
}

bool PacketGeneratorCore::run(unsigned int lcore_id)
{

    lcore_id_ = lcore_id;
    run_lcore_ = true;
    core_status_ = "starting";

    LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " starting up");

    dimensions_t dims(2);
    uint16_t packets_per_frame;
    uint32_t frame_pixels;

    // Grab expected dataset dimensions from the decoder
    dims[0] = decoder_->get_frame_x_resolution();
    dims[1] = decoder_->get_frame_y_resolution();
    frame_pixels = dims[0] * dims[1];

    // Collect relevant information from the decoder
    packets_per_frame = decoder_->get_packets_per_frame();
    const FrameProcessor::DataType frame_data_type = decoder_->get_frame_bit_depth();
    const size_t bytes_per_pixel = get_bytes_per_pixel(frame_data_type);

    // Initialise pointers to buffers fetched from the clear-frames ring
    void* raw_frame = nullptr;
    while (raw_frame == nullptr) {
        rte_ring_dequeue(clear_frames_ring_, &raw_frame);
    };

    // prepared_frame is a separate buffer where decoder writes packetised data
    void* prepared_frame = nullptr;
    while (prepared_frame == nullptr) {
        rte_ring_dequeue(clear_frames_ring_, &prepared_frame);
    };

    // Calculate pixels and bytes contained in each network packet
    uint32_t pixels_per_packet = frame_pixels / packets_per_frame;
    size_t bytes_per_packet = static_cast<size_t>(pixels_per_packet) * bytes_per_pixel;

    // Calculate total packet length including headers
    int l2_len = sizeof(struct rte_ether_hdr);
    int l3_len = sizeof(struct rte_ipv4_hdr);
    int len_4 = sizeof(struct rte_udp_hdr);
    uint64_t data_len = bytes_per_packet;

    uint64_t total_len_check
        = static_cast<uint64_t>(l2_len) + l3_len + len_4 + data_len + decoder_->get_packet_header_size();

    if (total_len_check > UINT16_MAX) {
        throw std::runtime_error(
            "Calculated packet length " + std::to_string(total_len_check) + " exceeds maximum packet size"
        );
    }

    // Allocate memory for frame data based on above calculations
    uint16_t total_packet_length = static_cast<uint16_t>(total_len_check);
    uint32_t temp_ip_buf;
    uint64_t frame_number = 0;
    uint32_t device_index;

    const uint32_t num_tx_devices = static_cast<uint32_t>(tx_devices_.size());

    if (num_tx_devices == 0) {
        LOG4CXX_ERROR(logger_, "No TX devices configured. Stopping PacketGeneratorCore.");
        core_status_ = "error";
        run_lcore_ = false;
        return false;
    }

    void* frame_data;

    const uint64_t cycles_per_sec = rte_get_tsc_hz();
    uint64_t timing_last = rte_get_tsc_cycles();
    uint64_t frames_this_second = 0;
    uint64_t pkts_this_second = 0;
    uint64_t cycles_this_second = 0;
    uint64_t max_frame_cycles_this_second = 0;
    uint64_t min_frame_cycles_this_second = UINT64_MAX;

    core_status_ = "running";

    // While loop to continuously dequeue frame objects
    while (likely(run_lcore_)) {
        uint64_t timing_now = rte_get_tsc_cycles();
        if (unlikely((timing_now - timing_last) >= cycles_per_sec)) {
            uint64_t elapsed_cycles = timing_now - timing_last;

            packets_per_second_ = (pkts_this_second * cycles_per_sec) / elapsed_cycles;
            frames_per_second_ = (frames_this_second * cycles_per_sec) / elapsed_cycles;
            busy_percentage_ = (static_cast<double>(cycles_this_second) * 100.0) / static_cast<double>(elapsed_cycles);

            if (frames_this_second > 0) {
                mean_frame_us_ = (static_cast<double>(cycles_this_second) * 1000000.0)
                    / (static_cast<double>(frames_this_second) * cycles_per_sec);
                min_frame_us_ = (static_cast<double>(min_frame_cycles_this_second) * 1000000.0)
                    / static_cast<double>(cycles_per_sec);
                max_frame_us_ = (static_cast<double>(max_frame_cycles_this_second) * 1000000.0)
                    / static_cast<double>(cycles_per_sec);
                if (max_frame_us_ > max_frame_us_all_time_)
                    max_frame_us_all_time_ = max_frame_us_;
            } else {
                mean_frame_us_ = min_frame_us_ = max_frame_us_ = 0.0;
            }

            frames_this_second = 0;
            pkts_this_second = 0;
            cycles_this_second = 0;
            max_frame_cycles_this_second = 0;
            min_frame_cycles_this_second = UINT64_MAX;
            timing_last = timing_now;
        }

        // Check if packet tx has been disabled
        if (!packet_tx_) {
            rte_pause();
            continue;
        }

        const uint64_t frame_start_cycles = rte_get_tsc_cycles();
        uint64_t pkts_this_frame = 0;

        // Collect the data from the data source
        data_source_->getData(raw_frame);

        // Encode the data through the decoder method
        frame_data = decoder_->prepare_frame(raw_frame, prepared_frame);

        for (uint32_t packet = 0; packet < packets_per_frame; packet++) {
            total_packets_++;

            // Randomly drop packets based on configuration
            bool drop_packet = (rte_rand() % 1000) < config_.packet_drop;
            if (!drop_packet) {
                // Decide which ring to use depending on round robin every frame or
                // packet
                if (round_robin_mode_ == RoundRobinMode::Packet) {
                    device_index = packet % num_tx_devices; // split frames over rings
                } else {
                    device_index = frame_number % num_tx_devices; // each frame on a different ring
                }

                // Select relevant device and allocate memory for packet
                auto& tx_dev = tx_devices_[device_index];
                struct rte_mbuf* mbuf = rte_pktmbuf_alloc(tx_dev.device->mbuf_pool());

                // Ensure buffer has been allocated
                if (mbuf == nullptr) {
                    mbuf_alloc_failures_++;
                    LOG4CXX_ERROR(logger_, "Failed to allocate mbuf");
                    continue;
                }

                // Initialises packet buffer
                rte_pktmbuf_append(mbuf, total_packet_length);
                mbuf->pkt_len = total_packet_length;
                mbuf->data_len = total_packet_length;

                // Map packet buffer to protocol headers and payload area
                struct rte_ether_hdr* eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr*);
                struct rte_ipv4_hdr* ip_hdr = (struct rte_ipv4_hdr*)((char*)eth_hdr + l2_len);
                struct rte_udp_hdr* udp_hdr = (struct rte_udp_hdr*)((char*)ip_hdr + l3_len);
                struct PacketHeader* packet_header = (struct PacketHeader*)((char*)udp_hdr + len_4);
                void* packet_data = reinterpret_cast<char*>(packet_header) + decoder_->get_packet_header_size();

                // Set packet and frame numbers
                decoder_->set_packet_number(packet_header, packet);
                decoder_->set_packet_frame_number(packet_header, frame_number);

                rte_ether_unformat_addr(config_.source_mac_address[device_index].c_str(), &eth_hdr->src_addr);
                rte_ether_unformat_addr(config_.destination_mac_address[device_index].c_str(), &eth_hdr->dst_addr);

                // Configure ethernet, IPv4 and UDP headers
                inet_pton(AF_INET, config_.destination_ip_address[device_index].c_str(), &temp_ip_buf);
                ip_hdr->dst_addr = temp_ip_buf;
                inet_pton(AF_INET, config_.source_ip_address[device_index].c_str(), &temp_ip_buf);
                ip_hdr->src_addr = temp_ip_buf;

                eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
                udp_hdr->dst_port = rte_bswap16(config_.destination_port);
                udp_hdr->src_port = rte_bswap16(config_.source_port);
                udp_hdr->dgram_len = rte_bswap16(data_len + 8 + decoder_->get_packet_header_size());

                ip_hdr->fragment_offset = 0;
                ip_hdr->next_proto_id = 17;
                ip_hdr->packet_id = (uint16_t)(rand() % (65535 + 1)); // Generate random packet ID
                ip_hdr->time_to_live = 128;
                ip_hdr->total_length = rte_cpu_to_be_16(total_packet_length - l2_len);
                ip_hdr->type_of_service = 0;
                ip_hdr->version_ihl = RTE_IPV4_VHL_DEF;

                // Copy the prepared frame slice for this packet into the packet payload
                rte_memcpy(
                    packet_data, static_cast<uint8_t*>(frame_data) + (static_cast<size_t>(packet) * bytes_per_packet),
                    bytes_per_packet
                );

                // Enqueue the mbuf onto the device-specific TX ring, busy-wait until
                // accepted
                while (rte_ring_enqueue(tx_dev.ring, mbuf) != 0) {
                    tx_ring_full_waits_;
                    rte_pause();
                }
                packets_sent_++;
                pkts_this_frame++;

            } else {
                dropped_packets_++;
                LOG4CXX_INFO(logger_, "Packet dropped!");
            }
        }
        const uint64_t frame_cycles = rte_get_tsc_cycles() - frame_start_cycles;
        frames_this_second++;
        pkts_this_second += pkts_this_frame;
        cycles_this_second += frame_cycles;
        if (frame_cycles > max_frame_cycles_this_second)
            max_frame_cycles_this_second = frame_cycles;
        if (frame_cycles < min_frame_cycles_this_second)
            min_frame_cycles_this_second = frame_cycles;

        frames_generated_++;
        last_frame_number_ = frame_number;
        frame_number++;
    }

    LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " completed");
    return true;
}

void PacketGeneratorCore::stop(void)
{
    if (run_lcore_) {
        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " stopping");
        core_status_ = "stopping";
        run_lcore_ = false;
    } else {
        if (core_status_ != "error")
            core_status_ = "stopped";
        LOG4CXX_DEBUG_LEVEL(2, logger_, "Core " << lcore_id_ << " already stopped");
    }
}

void PacketGeneratorCore::status(OdinData::IpcMessage& status, const std::string& path)
{
    LOG4CXX_DEBUG(logger_, "Status requested for PacketGeneratorCore_" << proc_idx_ << " from the DPDK plugin");

    std::string status_path = path + "/PacketGeneratorCore_" + std::to_string(proc_idx_) + "/";

    // Generic worker core status (same keys as PacketRxCore)
    status.set_param(status_path + "status", core_status_);
    status.set_param(status_path + "packets_sent", packets_sent_);
    status.set_param(status_path + "packets_dropped", dropped_packets_);
    status.set_param(status_path + "packets_per_second", packets_per_second_);
    status.set_param(status_path + "processing_time/average_us", mean_frame_us_);
    status.set_param(status_path + "processing_time/min_us", min_frame_us_);
    status.set_param(status_path + "processing_time/max_us", max_frame_us_);
    status.set_param(status_path + "busy_percentage", busy_percentage_);
    status.set_param(status_path + "last_frame_number", last_frame_number_);

    // Generator-specific
    status.set_param(status_path + "tx_enabled", packet_tx_);
    status.set_param(status_path + "total_packets", total_packets_);
    status.set_param(status_path + "frames_generated", frames_generated_);
    status.set_param(status_path + "frames_per_second", frames_per_second_);
    status.set_param(status_path + "max_frame_us_all_time", max_frame_us_all_time_);
    status.set_param(status_path + "mbuf_alloc_failures", mbuf_alloc_failures_);
    status.set_param(status_path + "tx_ring_full_waits", tx_ring_full_waits_);
    status.set_param(status_path + "num_tx_devices", (uint64_t)tx_devices_.size());

    // Per-TX-device ring / mbuf pool / xstats
    for (const auto& dev : tx_devices_) {
        const std::string dpath = status_path + "tx_device_" + std::to_string(dev.port_id) + "/";

        if (dev.ring) {
            uint64_t cnt = rte_ring_count(dev.ring);
            uint64_t sz = rte_ring_get_size(dev.ring);
            status.set_param(dpath + "tx_ring_count", cnt);
            status.set_param(dpath + "tx_ring_free", (uint64_t)rte_ring_free_count(dev.ring));
            status.set_param(dpath + "tx_ring_size", sz);
            status.set_param(dpath + "tx_ring_utilization_pct", sz > 0 ? (cnt * 100) / sz : (uint64_t)0);
        }

        if (dev.device) {
            struct rte_mempool* pool = dev.device->mbuf_pool();
            if (pool) {
                uint32_t avail = rte_mempool_avail_count(pool);
                uint32_t in_use = rte_mempool_in_use_count(pool);
                uint32_t total = avail + in_use;
                status.set_param(dpath + "mbuf_pool_available", avail);
                status.set_param(dpath + "mbuf_pool_in_use", in_use);
                status.set_param(dpath + "mbuf_pool_total", total);
                status.set_param(dpath + "mbuf_pool_utilization_pct", total > 0 ? (in_use * 100) / total : 0);
            }
        }

        // xstats: same body as PacketRxCore::status(), using dev.port_id and dpath
        // + "port_xstats/"
    }
}

bool PacketGeneratorCore::connect(void)
{
    for (const auto& addr : config_.device_addresses_) {
        if (!add_device(addr)) {
            LOG4CXX_ERROR(logger_, "Failed adding device " << addr);

            return false;
        }
    }

    std::string ring_name = ring_name_str(config_.config_key, socket_id_, proc_idx_);

    upstream_ring_ = rte_ring_lookup(ring_name.c_str());

    if (upstream_ring_ == NULL) {
        uint32_t ring_size = DEFAULT_RING_SIZE;

        upstream_ring_ = rte_ring_create(ring_name.c_str(), ring_size, socket_id_, RING_F_SP_ENQ | RING_F_SC_DEQ);

        if (upstream_ring_ == NULL) {
            LOG4CXX_ERROR(logger_, "Failed to create ring " << ring_name << " : " << rte_strerror(rte_errno));
            return false;
        }

        LOG4CXX_INFO(logger_, "Created ring " << ring_name);
    }

    return true;
}

void PacketGeneratorCore::configure(OdinData::IpcMessage& config)
{
    // Update the config based from the passed IPCmessage

    LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Got update config.");

    if (config.has_param("dataset_name")) {
        std::string dataset_name = config.get_param<std::string>("dataset_name");

        auto& data_source_config = config_.data_source_config();
        auto& allocator = data_source_config.GetAllocator();

        if (data_source_config.HasMember("dataset_name")) {
            data_source_config["dataset_name"].SetString(dataset_name.c_str(), allocator);
        } else {
            data_source_config.AddMember("dataset_name", rapidjson::Value(dataset_name.c_str(), allocator), allocator);
        }

        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Setting dataset_name to: " << dataset_name);
    }
}

std::vector<std::pair<std::string, int>> PacketGeneratorCore::requestCommands()
{
    return { { "start_tx", DEFAULT_COMMAND_PRIORITY }, { "stop_tx", DEFAULT_COMMAND_PRIORITY } };
}

void PacketGeneratorCore::execute(const std::string& command, OdinData::IpcMessage& reply)
{
    LOG4CXX_INFO(logger_, "Picked up commands");
    if (command == "start_tx") {
        start_tx();
    } else if (command == "stop_tx") {
        stop_tx();
    } else {
        reply.set_nack("PacketTxCore: unknown command: " + command);
    }
}

void PacketGeneratorCore::start_tx(void)
{
    LOG4CXX_INFO(logger_, "Starting TX");
    packet_tx_ = true;
}

void PacketGeneratorCore::stop_tx(void)
{
    LOG4CXX_INFO(logger_, "Stopping TX");
    packet_tx_ = false;
}

bool PacketGeneratorCore::add_device(const std::string& pci_address)
{
    LOG4CXX_INFO(logger_, "Adding device");

    int ret = rte_eal_hotplug_add("pci", pci_address.c_str(), "");
    if (ret < 0) {
        LOG4CXX_ERROR(logger_, "Failed to hot plug device: " << pci_address);
        return false;
    }

    uint16_t port_id;

    ret = rte_eth_dev_get_port_by_name(pci_address.c_str(), &port_id);
    if (ret != 0) {
        LOG4CXX_ERROR(logger_, "Failed to get port ID for device: " << pci_address);
        return false;
    }

    LOG4CXX_INFO(logger_, "Starting device");
    DpdkDevice* device = new DpdkDevice(port_id, config_.dpdk_device());
    if (!device->start()) {
        LOG4CXX_ERROR(logger_, "Failed to start device: " << pci_address);
        delete device;
        return false;
    }

    struct rte_eth_link link;

    bool x = rte_eth_link_get_nowait(port_id, &link);

    LOG4CXX_INFO(
        logger_, "Port " << port_id << " link " << (link.link_status ? "UP" : "DOWN") << " speed " << link.link_speed
    );

    std::string ring_name = "tx_port_" + std::to_string(port_id);

    rte_ring* ring = rte_ring_lookup(ring_name.c_str());

    if (!ring) {
        ring = rte_ring_create(ring_name.c_str(), DEFAULT_RING_SIZE, socket_id_, RING_F_SP_ENQ | RING_F_SC_DEQ);
    }

    if (!ring) {
        LOG4CXX_ERROR(logger_, "Failed creating ring " << ring_name);
        delete device;
        return false;
    }

    TxDeviceContext ctx;

    ctx.port_id = port_id;
    ctx.device = device;
    ctx.ring = ring;

    tx_devices_.push_back(ctx);

    LOG4CXX_INFO(logger_, "Successfully added device: " << pci_address << " (Port ID: " << port_id << ")");

    return true;
}

void PacketGeneratorCore::requestConfiguration(OdinData::IpcMessage& reply)
{
    LOG4CXX_DEBUG(logger_, "Configuration requested for PacketGeneratorCore");
    std::string plugin = "XIDyn";

    const auto& data_source_config = config_.data_source_config();

    for (auto it = data_source_config.MemberBegin(); it != data_source_config.MemberEnd(); ++it) {
        if (it->value.IsString()) {
            reply.set_param(plugin + "/data_source_config/" + it->name.GetString(), std::string(it->value.GetString()));
        }
    }

    reply.set_param(plugin + "/packet_drop", static_cast<int>(config_.packet_drop));
}

DPDKREGISTER(DpdkWorkerCore, PacketGeneratorCore, "PacketGeneratorCore");

} // namespace FrameProcessor