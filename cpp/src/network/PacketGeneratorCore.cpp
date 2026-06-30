#include "PacketGeneratorCore.h"
#include "DpdkUtils.h"
#include <blosc.h>
#include "DpdkSharedBufferFrame.h"
#include </usr/lib/x86_64-linux-gnu/hdf5/serial/include/hdf5.h>
#include <thread>
#include <chrono>

namespace FrameProcessor
{

    enum class NumericalPattern {
        Incrementing,
        PacketNum,
        Fixed,
        Unknown
    };

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

        LOG4CXX_INFO(logger_, "FP.PacketGeneratorCore " << proc_idx_ << " Created with config:"
            << " | core_name: " << config_.core_name
            << " | num_cores: " << config_.num_cores
            << " | connect: " << config_.connect
            << " | upstream_core: " << config_.upstream_core
            << " | num_downsteam_cores: " << config_.num_downstream_cores
        );

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
                // TODO - this is fatal and should raise an exception
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
        // Create rings here - copy structure above but for port ID rings

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
        uint16_t payload;
        uint32_t frame_pixels;

        dims[0] = decoder_->get_frame_x_resolution();
        dims[1] = decoder_->get_frame_y_resolution();

        frame_pixels = dims[0] * dims[1];
        packets_per_frame = decoder_->get_packets_per_frame();
        payload = decoder_->get_payload_size();

        // Need frames for loop for PREPARED_FRAMES
        
        // uint16_t *frame_buffer = new uint16_t[frame_pixels];
        // if (!frame_buffer) {
        //     LOG4CXX_ERROR(logger_, "Error allocating frame buffer");
        // } else {
        //     LOG4CXX_INFO(logger_, "Frame buffer allocated");
        // };

        uint16_t *data_array = nullptr;
        while (data_array == nullptr)
        {
            rte_ring_dequeue(clear_frames_ring_, (void**) &data_array);
        };

        uint64_t pixel_index = 0;
        uint64_t pixel_value = 0;

        uint32_t pixels_per_packet = frame_pixels / packets_per_frame;
        uint32_t bytes_per_packet  = pixels_per_packet * sizeof(uint16_t);

        NumericalPattern pattern = NumericalPattern::Unknown;
        LOG4CXX_INFO(logger_, "5");

        if (config_.test_pattern_mode == "numerical-incrementing")
            pattern = NumericalPattern::Incrementing;

        else if (config_.test_pattern_mode == "numerical-packetnum")
            pattern = NumericalPattern::PacketNum;

        else if (config_.test_pattern_mode == "numerical-fixed")
            pattern = NumericalPattern::Fixed;

        while (pixel_index < frame_pixels) {
            uint64_t value = 0;
            pixel_value = pixel_index;

            switch (pattern) {
                case NumericalPattern::Incrementing: {
                    uint64_t pos = pixel_index % 131072;

                    value =
                        static_cast<uint16_t>(
                            (pos <= 65535) ? pos : (131071 - pos)
                    );
                    break;
                }

                case NumericalPattern::PacketNum: {
                    value = std::trunc(pixel_value / pixels_per_packet);
                    break;
                }

                case NumericalPattern::Fixed: {
                    value = 84;
                    break;
                }

                default: {
                    value = 0;
                    break;
                }
            }

            data_array[pixel_index++] = value;
        }

        int l2_len = sizeof(struct rte_ether_hdr);
        int l3_len = sizeof(struct rte_ipv4_hdr);
        int len_4 = sizeof (struct rte_udp_hdr);

        uint64_t data_len = (frame_pixels / packets_per_frame)  * sizeof(uint16_t); // pixels_per_packet

        rte_be64_t frame_counter = 0;
        uint16_t total_packet_length = l2_len + l3_len + len_4 + data_len + 64; // xiDyn_HDR_SIZE;
        uint32_t temp_ip_buf;
        uint64_t frame_number = 0;

        // if (!upstream_ring_) {
        //     LOG4CXX_ERROR(logger_, "upstream_ring_ is NULL");
        //     return false;
        // }

        // While loop to continuously dequeue frame objects
        while (likely(run_lcore_))
        {
            if (!packet_tx_)
            {
                rte_pause();
                continue;
            }

            for (uint32_t packet = 0; packet < packets_per_frame; packet++)
            {
                // struct rte_mbuf* mbuf = rte_pktmbuf_alloc(device_->mbuf_pool());

                uint32_t device_index = packet % tx_devices_.size();
                auto& tx_dev = tx_devices_[device_index];

                struct rte_mbuf* mbuf = rte_pktmbuf_alloc(tx_dev.device->mbuf_pool());

                if (mbuf == nullptr)
                {
                    LOG4CXX_ERROR(
                        logger_,
                        "Failed to allocate mbuf"
                    );
                    continue;
                }

                rte_pktmbuf_append(
                    mbuf,
                    total_packet_length
                );

                mbuf->pkt_len  = total_packet_length;
                mbuf->data_len = total_packet_length;

                struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
                struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)((char *)eth_hdr + l2_len);
                struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)((char *)ip_hdr + l3_len);
                struct PacketHeader *packet_header = (struct PacketHeader *)((char *)udp_hdr + len_4);
                uint16_t *packet_data = (uint16_t *)((char *) packet_header + decoder_->get_packet_header_size());

                decoder_->set_packet_number(packet_header, packet);
                decoder_->set_packet_frame_number(packet_header, frame_number);

                // uint16_t *packet_data = (uint16_t *)((char *))

                // rte_memcpy((uint16_t))

                rte_ether_unformat_addr(config_.source_mac_address[device_index].c_str(), &eth_hdr->src_addr);
                rte_ether_unformat_addr(config_.destination_mac_address[device_index].c_str(), &eth_hdr->dst_addr);

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

                rte_memcpy(packet_data, data_array + (packet * pixels_per_packet), bytes_per_packet);

                // LOG4CXX_INFO(
                //     logger_,
                //     "Enqueue packet "
                //     << packet
                //     << " to port "
                //     << tx_dev.port_id
                //     << " ring count before "
                //     << rte_ring_count(tx_dev.ring)
                // );

                while (
                    rte_ring_enqueue(
                        tx_dev.ring,
                        mbuf
                    ) != 0
                )
                {
                    rte_pause();
                }
                // if ((packet % 1000) == 0)
                // {
                //     LOG4CXX_INFO(
                //         logger_,
                //         "Ring "
                //         << tx_dev.port_id
                //         << " count="
                //         << rte_ring_count(tx_dev.ring)
                //     );
                // }
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

    }

    std::vector<std::string> PacketGeneratorCore::requestCommands()
    {
        return {"start_tx", "stop_tx"};
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

        LOG4CXX_INFO(logger_, "Hotplugging device");
        int ret = rte_eal_hotplug_add("pci", pci_address.c_str(), "");
        if (ret < 0) {
            LOG4CXX_ERROR(logger_, "Failed to hot plug device: " << pci_address);
            return false;
        }

        uint16_t port_id;

        LOG4CXX_INFO(logger_, "Fetching port ID");
        ret = rte_eth_dev_get_port_by_name(pci_address.c_str(), &port_id);
        if (ret != 0) {
            LOG4CXX_ERROR(logger_, "Failed to get port ID for device: " << pci_address);
            return false;
        }

        LOG4CXX_WARN(logger_, "PORT ID" << port_id);

        LOG4CXX_INFO(logger_, "Starting device");
        DpdkDevice* device = new DpdkDevice(port_id, config_.dpdk_device());
        if (!device->start()) {
            LOG4CXX_ERROR(logger_, "Failed to start device: " << pci_address);
            delete device;
            return false;
        }

        struct rte_eth_link link;

        rte_eth_link_get_nowait(port_id, &link);

        LOG4CXX_INFO(
            logger_,
            "Port "
            << port_id
            << " link "
            << (link.link_status ? "UP" : "DOWN")
            << " speed "
            << link.link_speed
        );

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

    DPDKREGISTER(DpdkWorkerCore, PacketGeneratorCore, "PacketGeneratorCore");

}
