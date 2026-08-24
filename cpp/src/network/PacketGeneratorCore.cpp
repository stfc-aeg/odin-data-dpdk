#include "PacketGeneratorCore.h"
#include "DpdkUtils.h"
#include <blosc.h>
#include "DpdkSharedBufferFrame.h"
#include </usr/lib/x86_64-linux-gnu/hdf5/serial/include/hdf5.h>
#include "DataSource.h"
#include "GeneratedDataSource.h"
#include "HDF5DataSource.h"
#include "DataSourceLoader.h"
#include <thread>
#include <chrono>

namespace FrameProcessor
{
    // Finds the bytes per pixel for specified data type
    size_t get_bytes_per_pixel(FrameProcessor::DataType data_type)
    {
        switch (data_type)
        {
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

    PacketGeneratorCore::PacketGeneratorCore(
        int fb_idx, int socket_id, DpdkWorkCoreReferences &dpdkWorkCoreReferences
    ) :
        DpdkWorkerCore(socket_id),
        logger_(Logger::getLogger("FP.PacketGeneratorCore")),
        proc_idx_(fb_idx),
        decoder_(dynamic_cast<PacketProtocolDecoder *>(dpdkWorkCoreReferences.decoder)),
        frame_callback_(dpdkWorkCoreReferences.frame_callback),
        shared_buf_(dpdkWorkCoreReferences.shared_buf)
    {

        // Get the configuration container for this worker
        config_.resolve(dpdkWorkCoreReferences.core_config);

        // Load the relevant DataSource class
        const FrameProcessor::DataType data_type = decoder_->get_frame_bit_depth();

        // Build the DataSource configuration
        rapidjson::Document data_source_config;
        data_source_config.SetObject();

        auto& allocator = data_source_config.GetAllocator();

        data_source_config.AddMember(
            "data_source",
            rapidjson::Value(config_.data_source.c_str(), allocator),
            allocator);

        if (!config_.pattern.empty())
        {
            data_source_config.AddMember(
                "pattern",
                rapidjson::Value(config_.pattern.c_str(), allocator),
                allocator);
        }

        if (!config_.file_path.empty())
        {
            data_source_config.AddMember(
                "file_path",
                rapidjson::Value(config_.file_path.c_str(), allocator),
                allocator);
        }

        if (!config_.dataset_name.empty())
        {
            data_source_config.AddMember(
                "dataset_name",
                rapidjson::Value(config_.dataset_name.c_str(), allocator),
                allocator);
        }

        // Load the requested DataSource
        data_source_ =
            FrameProcessor::DataSourceLoader<
                FrameProcessor::DataSource>::load_class(
                    config_.data_source,
                    decoder_,
                    data_source_config);

        if (!data_source_)
        {
            throw std::runtime_error(
                "Failed to load DataSource: " + config_.data_source);
        }

        // Class loading - reference it as base class
        // Base class must have all methods, doesn't care how it is specialised

        LOG4CXX_INFO(logger_, "FP.PacketGeneratorCore " << proc_idx_ << " Created with config:"
            << " | core_name: " << config_.core_name
            << " | num_cores: " << config_.num_cores
            << " | connect: " << config_.connect
            << " | upstream_core: " << config_.upstream_core
            << " | num_downsteam_cores: " << config_.num_downstream_cores
        );

        // Searches for the clear-frames ring and creates it if not existing
        std::string clear_frames_ring_name = ring_name_clear_frames(socket_id_);
        clear_frames_ring_ = rte_ring_lookup(clear_frames_ring_name.c_str());
        if (clear_frames_ring_ == NULL)
        {
            unsigned int clear_frames_ring_size = nearest_power_two(shared_buf_->get_num_buffers());
            LOG4CXX_DEBUG_LEVEL(2, logger_, "Creating frame processed ring name "
                << clear_frames_ring_name << " of size " << clear_frames_ring_size << " numa node: " << socket_id_
            );
            clear_frames_ring_ = rte_ring_create(
                clear_frames_ring_name.c_str(), clear_frames_ring_size, socket_id_, 0
            );
            if (clear_frames_ring_ == NULL)
            {
                LOG4CXX_ERROR(logger_, "Error creating frame processed ring " << clear_frames_ring_name
                    << " : " << rte_strerror(rte_errno)
                );
            }
            else
            {
                // Populate the ring with hugepages memory locations to the SMB
                for (int element = 0; element < shared_buf_->get_num_buffers(); element++)
                {

                    rte_ring_enqueue(clear_frames_ring_, shared_buf_->get_buffer_address(element));
                }
            }
        }
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

        // Initialise pointer to raw frame data
        void *raw_frame = nullptr;
        while (raw_frame == nullptr)
        {
            rte_ring_dequeue(clear_frames_ring_, &raw_frame);
        };

        // Calculate pixels and bytes in each packet
        uint32_t pixels_per_packet = frame_pixels / packets_per_frame;
        size_t bytes_per_packet  = static_cast<size_t>(pixels_per_packet) * bytes_per_pixel;

        // Calculate total packet length including headers
        int l2_len = sizeof(struct rte_ether_hdr);
        int l3_len = sizeof(struct rte_ipv4_hdr);
        int len_4 = sizeof(struct rte_udp_hdr);
        uint64_t data_len = bytes_per_packet;

        // Allocate memory for frame data based on above calculations
        uint16_t total_packet_length = l2_len + l3_len + len_4 + data_len + 64; // xiDyn_HDR_SIZE;
        uint32_t temp_ip_buf;
        uint64_t frame_number = 0;

        void *prepared_frame = new uint8_t[frame_pixels * bytes_per_pixel];

        // While loop to continuously dequeue frame objects
        while (likely(run_lcore_))
        {
            // Check if packet tx has been disabled
            if (!packet_tx_)
            {
                rte_pause();
                continue;
            }

            // Collect the data from the data source
            data_source_->getData(raw_frame);

            // Encode the data through the decoder method
            if (false) //(config_.reorder_frame)
            {
                decoder_->prepare_frame(raw_frame, prepared_frame);
                std::swap(raw_frame, prepared_frame);
            }

            for (uint32_t packet = 0; packet < packets_per_frame; packet++)
            {
                // Randomly drop packets based on configuration
                bool drop_packet = (rte_rand() % 1000) < config_.packet_drop;
                if (!drop_packet)
                    {
                    // Decide which ring to use depending on round robin every frame or packet
                    // uint32_t device_index = packet % tx_devices_.size(); // split frames over rings
                    uint32_t device_index = frame_number % tx_devices_.size(); // each frame on a different ring
                    
                    // Select relevant device and allocate memory for packet
                    auto& tx_dev = tx_devices_[device_index];
                    struct rte_mbuf* mbuf = rte_pktmbuf_alloc(tx_dev.device->mbuf_pool());

                    // Ensure buffer has been allocated
                    if (mbuf == nullptr)
                    {
                        LOG4CXX_ERROR(logger_, "Failed to allocate mbuf");
                        continue;
                    }

                    // Initialises packet buffer
                    rte_pktmbuf_append(mbuf, total_packet_length);
                    mbuf->pkt_len  = total_packet_length;
                    mbuf->data_len = total_packet_length;

                    // Set pointers to each protocol header
                    struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
                    struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)((char *)eth_hdr + l2_len);
                    struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)((char *)ip_hdr + l3_len);
                    struct PacketHeader *packet_header = (struct PacketHeader *)((char *)udp_hdr + len_4);
                    void *packet_data = reinterpret_cast<char *>(packet_header) + decoder_->get_packet_header_size();

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
                    ip_hdr->ihl = 5;
                    ip_hdr->next_proto_id = 17;
                    ip_hdr->packet_id = (uint16_t)(rand() % (65535 + 1)); // Generate random packet ID
                    ip_hdr->time_to_live = 128;
                    ip_hdr->total_length = rte_cpu_to_be_16(total_packet_length - l2_len);
                    ip_hdr->type_of_service = 0;
                    ip_hdr->version = 4;
                    ip_hdr->version_ihl = RTE_IPV4_VHL_DEF;

                    // Copy the packet data into the packet
                    rte_memcpy(packet_data, static_cast<uint8_t*>(raw_frame) + (static_cast<size_t>(packet) * bytes_per_packet), bytes_per_packet);

                    // Enqueues the completed packet onto the transmit ring
                    while (
                        rte_ring_enqueue(tx_dev.ring, mbuf) != 0
                    )
                    {
                        rte_pause();
                    }
                } else {
                    LOG4CXX_INFO(logger_, "Packet dropped!");
                }
            }
            frame_number++;
        }
  
        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " completed");
        return true;
    }

    void PacketGeneratorCore::stop(void)
    {
        if (run_lcore_)
        {
            LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " stopping");
            run_lcore_ = false;
        }
        else
        {
            LOG4CXX_DEBUG_LEVEL(2, logger_, "Core " << lcore_id_ << " already stopped");
        }
    }

    void PacketGeneratorCore::status(OdinData::IpcMessage& status, const std::string& path)
    {
        LOG4CXX_DEBUG(logger_, "Status requested for PacketGeneratorCore_" << proc_idx_
            << " from the DPDK plugin");

        std::string status_path = path + "/PacketGeneratorCore_" + std::to_string(proc_idx_) + "/";

        status.set_param(status_path + "data_source", config_.data_source);

        if (config_.data_source == "generated")
        {
            status.set_param(status_path + "pattern", config_.pattern);
        }
        else if (config_.data_source == "hdf5")
        {
            status.set_param(status_path + "file_path", config_.file_path);
        }
    }

    bool PacketGeneratorCore::connect(void)
    {
        for (const auto& addr : config_.device_addresses_)
        {
            if (!add_device(addr))
            {
                LOG4CXX_ERROR(
                    logger_,
                    "Failed adding device " << addr
                );

                return false;
            }
        }

        std::string ring_name = ring_name_str( config_.core_name, socket_id_, proc_idx_);

        upstream_ring_ = rte_ring_lookup(ring_name.c_str());

        //Should not create ring here

        if (upstream_ring_ == NULL)
        {
            uint32_t ring_size = 16384;

            upstream_ring_ = rte_ring_create(ring_name.c_str(), ring_size, socket_id_, RING_F_SP_ENQ | RING_F_SC_DEQ);

            if (upstream_ring_ == NULL)
            {
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
        LOG4CXX_INFO(logger_, config.get_param("dataset_name", false));
        // LOG4CXX_INFO(logger_, "Config: " << config.encode());
        // LOG4CXX_INFO(logger_, "Param 2: " << config.get_param("params/dataset_name", false));
        if (config.has_param("dataset_name"))
        {
            config_.dataset_name = config.get_param("dataset_name", false);
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Setting config_.dataset_name to: " <<  config_.dataset_name);
            // config_.data_source = config.get_param("dat", false);
            // LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Setting config_.data_source to: " <<  config_.data_source);
        }        

        // Look in PacketRxCore.cpp for example

    }

    std::vector<std::pair<std::string, int>> PacketGeneratorCore::requestCommands()
    {
        return {
            {"start_tx", DEFAULT_COMMAND_PRIORITY},
            {"stop_tx", DEFAULT_COMMAND_PRIORITY}
        };
    }

    void PacketGeneratorCore::execute(const std::string& command, OdinData::IpcMessage& reply)
    {
        LOG4CXX_INFO(logger_, "Picked up commands");
        if (command == "start_tx")
        {
            start_tx();
        }
        else if (command == "stop_tx")
        {
            stop_tx();
        }
        // else
        // {
        //     reply.set_nack("PacketTxCore: unknown command: " + command);
        // }
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

        LOG4CXX_INFO(logger_, "Port " << port_id << " link " << (link.link_status ? "UP" : "DOWN") << " speed " << link.link_speed);

        std::string ring_name = "tx_port_" + std::to_string(port_id);

        rte_ring* ring = rte_ring_lookup(ring_name.c_str());

        if (!ring)
        {
            ring = rte_ring_create(ring_name.c_str(), 16384, socket_id_, RING_F_SP_ENQ | RING_F_SC_DEQ);
        }


        if (!ring)
        {
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
        
        // if (decoder_) {
        reply.set_param(plugin + "/data_source", config_.data_source);
        reply.set_param(plugin + "/pattern", config_.pattern);
        reply.set_param(plugin + "/file_path", config_.file_path);
        reply.set_param(plugin + "/dataset_name", config_.dataset_name);
        reply.set_param(plugin + "/packet_drop", 
                        static_cast<int>(config_.packet_drop));
        // }
    }

    DPDKREGISTER(DpdkWorkerCore, PacketGeneratorCore, "PacketGeneratorCore");

}