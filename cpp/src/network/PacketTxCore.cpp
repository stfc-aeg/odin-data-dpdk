#include "PacketTxCore.h"
#include "DpdkUtils.h"
#include <blosc.h>
#include "DpdkSharedBufferFrame.h"
#include </usr/lib/x86_64-linux-gnu/hdf5/serial/include/hdf5.h>
#include <thread>
#include <chrono>

namespace FrameProcessor
{

    PacketTxCore::PacketTxCore(
        int fb_idx, int socket_id, DpdkWorkCoreReferences &dpdkWorkCoreReferences
    ) :
        DpdkWorkerCore(socket_id, dpdkWorkCoreReferences.stream_id, dpdkWorkCoreReferences.config_key),
        logger_(Logger::getLogger("FP.PacketTxCore")),
        proc_idx_(fb_idx),
        decoder_(dynamic_cast<PacketProtocolDecoder *>(dpdkWorkCoreReferences.decoder)),
        frame_callback_(dpdkWorkCoreReferences.frame_callback),
        shared_buf_(dpdkWorkCoreReferences.shared_buf),
        total_packets_(0),
        sent_packets_(0),
        dropped_packets_(0),
        mean_burst_us_(0),
        min_burst_us_(0),
        max_burst_us_(0),
        max_burst_us_all_time_(0),
        mean_pkts_per_burst_(0),
        max_pkts_per_burst_(0),
        packets_per_second_(0),
        busy_percentage_(0.0),
        estimated_pps_(0),
        max_estimated_pps_(0),
        core_status_("constructing")        
    {

        // Get the configuration container for this worker
        config_.resolve(dpdkWorkCoreReferences.core_config, dpdkWorkCoreReferences.config_key);

        LOG4CXX_INFO(logger_, "FP.PacketTxCore " << proc_idx_ << " Created with config:"
            << " | core_name: " << config_.core_name
            << " | num_cores: " << config_.num_cores
            << " | connect: " << config_.connect
            << " | upstream_core: " << config_.upstream_core
            << " | num_downsteam_cores: " << config_.num_downstream_cores
        );

        // Create or lookup rings used to recycle frame buffers between cores
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
                // Log failure to create the ring (should be fatal)
                LOG4CXX_ERROR(logger_, "Error creating frame processed ring " << clear_frames_ring_name
                    << " : " << rte_strerror(rte_errno)
                );
            }
            else
            {
                // Populate the newly created ring with hugepages memory locations from the SMB
                for (int element = 0; element < shared_buf_->get_num_buffers(); element++)
                {
                    rte_ring_enqueue(clear_frames_ring_, shared_buf_->get_buffer_address(element));
                }
            }
        }
        else
        {
            LOG4CXX_DEBUG_LEVEL(2, logger_, "Frame processed ring name "
                << clear_frames_ring_name << " has already been created"
            );
        }

        core_status_ = "ready";

    }

    PacketTxCore::~PacketTxCore(void)
    {
        LOG4CXX_DEBUG_LEVEL(2, logger_, "PacketTxCore destructor");
        stop();
    }

     bool PacketTxCore::run(unsigned int lcore_id)
    {
        lcore_id_ = lcore_id;
        run_lcore_ = true;
        core_status_ = "starting";

        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " starting up");

        if (tx_devices_.empty())
        {
            LOG4CXX_ERROR(logger_, "No TX rings connected. Stopping PacketTxCore.");
            core_status_ = "error";
            run_lcore_ = false;
            return false;
        }

        // Per-burst processing timing, aggregated every second (same scheme as PacketRxCore::run())
        const uint64_t cycles_per_sec = rte_get_tsc_hz();
        uint64_t timing_last = rte_get_tsc_cycles();
        uint64_t bursts_this_second = 0;
        uint64_t pkts_this_second = 0;
        uint64_t cycles_this_second = 0;
        uint64_t max_burst_cycles_this_second = 0;
        uint64_t min_burst_cycles_this_second = UINT64_MAX;
        uint64_t max_pkts_this_second = 0;

        core_status_ = "running";

        // While loop to continuously dequeue frame objects
        while (likely(run_lcore_))
        {
            uint64_t burst_start_cycles = 0;
            uint64_t pkts_this_burst = 0;

            for (auto& tx_dev : tx_devices_)
            {
                struct rte_mbuf* mbuf = nullptr;

                int ret = rte_ring_dequeue(tx_dev.ring, (void**)&mbuf);

                if (ret != 0)
                {
                    continue;
                }

                // Start the burst timer on the first packet found, as Rx does once a burst is non-empty
                if (pkts_this_burst == 0)
                    burst_start_cycles = rte_get_tsc_cycles();

                pkts_this_burst++;
                total_packets_++;

                // Send the packets
                uint16_t sent = rte_eth_tx_burst(tx_dev.port_id, 0, &mbuf, 1);

                if (likely(sent != 0))
                {
                    sent_packets_++;
                    tx_dev.packets_sent++;
                }
                else
                {
                    if ((tx_dev.packets_dropped % 10000) == 0)
                    {
                        LOG4CXX_ERROR(logger_, "TX FAILED port " << tx_dev.port_id
                            << ", dropped " << (tx_dev.packets_dropped + 1) << " packet(s) so far");
                    }

                    dropped_packets_++;
                    tx_dev.packets_dropped++;
                    rte_pktmbuf_free(mbuf);
                }
            }

            if (pkts_this_burst > 0)
            {
                uint64_t burst_cycles = rte_get_tsc_cycles() - burst_start_cycles;
                bursts_this_second++;
                pkts_this_second += pkts_this_burst;
                cycles_this_second += burst_cycles;
                if (burst_cycles > max_burst_cycles_this_second)
                    max_burst_cycles_this_second = burst_cycles;
                if (burst_cycles < min_burst_cycles_this_second)
                    min_burst_cycles_this_second = burst_cycles;
                if (pkts_this_burst > max_pkts_this_second)
                    max_pkts_this_second = pkts_this_burst;
            }

            uint64_t timing_now = rte_get_tsc_cycles();
            if (unlikely((timing_now - timing_last) >= cycles_per_sec))
            {
                uint64_t elapsed_cycles = timing_now - timing_last;

                packets_per_second_ = (pkts_this_second * cycles_per_sec) / elapsed_cycles;
                busy_percentage_ = (static_cast<double>(cycles_this_second) * 100.0) /
                                   static_cast<double>(elapsed_cycles);

                if (bursts_this_second > 0)
                {
                    mean_burst_us_ = (static_cast<double>(cycles_this_second) * 1000000.0) /
                                     (static_cast<double>(bursts_this_second) * cycles_per_sec);
                    min_burst_us_  = (static_cast<double>(min_burst_cycles_this_second) * 1000000.0) /
                                     static_cast<double>(cycles_per_sec);
                    max_burst_us_  = (static_cast<double>(max_burst_cycles_this_second) * 1000000.0) /
                                     static_cast<double>(cycles_per_sec);
                    mean_pkts_per_burst_ = static_cast<double>(pkts_this_second) /
                                           static_cast<double>(bursts_this_second);
                    max_pkts_per_burst_ = max_pkts_this_second;
                    estimated_pps_ = cycles_this_second > 0
                        ? (pkts_this_second * cycles_per_sec) / cycles_this_second
                        : 0;

                    if (max_burst_us_ > max_burst_us_all_time_)
                        max_burst_us_all_time_ = max_burst_us_;
                    if (estimated_pps_ > max_estimated_pps_)
                        max_estimated_pps_ = estimated_pps_;
                }
                else
                {
                    // Idle this second: report zero rather than freezing the last busy values
                    mean_burst_us_ = min_burst_us_ = max_burst_us_ = 0.0;
                    mean_pkts_per_burst_ = 0.0;
                    max_pkts_per_burst_ = 0;
                    estimated_pps_ = 0;
                }

                bursts_this_second = 0;
                pkts_this_second = 0;
                cycles_this_second = 0;
                max_burst_cycles_this_second = 0;
                min_burst_cycles_this_second = UINT64_MAX;
                max_pkts_this_second = 0;
                timing_last = timing_now;
            }

            rte_pause();
        }

        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " completed");
        core_status_ = "stopped";

        return true;
    }

    void PacketTxCore::stop(void)
    {
        if (run_lcore_)
        {
            LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " stopping");
            core_status_ = "stopping";
            run_lcore_ = false;
        }
        else
        {
            if (core_status_ != "error")
                core_status_ = "stopped";

            LOG4CXX_DEBUG_LEVEL(2, logger_, "Core " << lcore_id_ << " already stopped");
        }
    }

    void PacketTxCore::status(OdinData::IpcMessage& status, const std::string& path)
    {
        std::string status_path = path + "/PacketTxCore_" + std::to_string(proc_idx_) + "/";

        // Generic worker core status (same keys as PacketRxCore)
        status.set_param(status_path + "status", core_status_);
        status.set_param(status_path + "packets_received", total_packets_);
        status.set_param(status_path + "packets_processed", sent_packets_);
        status.set_param(status_path + "packets_dropped", dropped_packets_);
        status.set_param(status_path + "packets_per_second", packets_per_second_);
        status.set_param(status_path + "processing_time/average_us", mean_burst_us_);
        status.set_param(status_path + "processing_time/min_us", min_burst_us_);
        status.set_param(status_path + "processing_time/max_us", max_burst_us_);
        status.set_param(status_path + "busy_percentage", busy_percentage_);

        // PacketTxCore-specific status
        status.set_param(status_path + "max_burst_us_all_time", max_burst_us_all_time_);
        status.set_param(status_path + "mean_pkts_per_burst", mean_pkts_per_burst_);
        status.set_param(status_path + "max_pkts_per_burst", max_pkts_per_burst_);
        status.set_param(status_path + "estimated_pps", estimated_pps_);
        status.set_param(status_path + "max_estimated_pps", max_estimated_pps_);
        status.set_param(status_path + "num_tx_devices", (uint64_t)tx_devices_.size());

        // mbuf pool occupancy, looked up by its well-known name as in PacketRxCore
        struct rte_mempool* mbuf_pool = rte_mempool_lookup(mbuf_pool_name_str(socket_id_).c_str());
        if (mbuf_pool)
        {
            uint32_t mbuf_avail = rte_mempool_avail_count(mbuf_pool);
            uint32_t mbuf_in_use = rte_mempool_in_use_count(mbuf_pool);
            uint32_t mbuf_total = mbuf_avail + mbuf_in_use;

            status.set_param(status_path + "mbuf_pool_available", mbuf_avail);
            status.set_param(status_path + "mbuf_pool_in_use", mbuf_in_use);
            status.set_param(status_path + "mbuf_pool_total", mbuf_total);
            status.set_param(status_path + "mbuf_pool_utilization_pct",
                            mbuf_total > 0 ? (mbuf_in_use * 100) / mbuf_total : 0);
        }

        // Per-device counters, TX ring occupancy and port xstats
        for (const auto& dev : tx_devices_)
        {
            const std::string dpath = status_path + "tx_device_" + std::to_string(dev.port_id) + "/";

            status.set_param(dpath + "packets_processed", dev.packets_sent);
            status.set_param(dpath + "packets_dropped", dev.packets_dropped);

            if (dev.ring)
            {
                uint64_t count = (uint64_t)rte_ring_count(dev.ring);
                uint64_t size  = (uint64_t)rte_ring_get_size(dev.ring);
                status.set_param(dpath + "tx_ring_count", count);
                status.set_param(dpath + "tx_ring_free", (uint64_t)rte_ring_free_count(dev.ring));
                status.set_param(dpath + "tx_ring_size", size);
                status.set_param(dpath + "tx_ring_utilization_pct", size > 0 ? (count * 100) / size : (uint64_t)0);
            }

            int len = rte_eth_xstats_get(dev.port_id, NULL, 0);
            if (len > 0)
            {
                struct rte_eth_xstat *xstats = (struct rte_eth_xstat *)calloc(len, sizeof(*xstats));
                struct rte_eth_xstat_name *xstats_names = (struct rte_eth_xstat_name *)calloc(len, sizeof(*xstats_names));

                if (xstats && xstats_names)
                {
                    int ret = rte_eth_xstats_get(dev.port_id, xstats, len);
                    if (ret >= 0 && ret <= len)
                    {
                        ret = rte_eth_xstats_get_names(dev.port_id, xstats_names, len);
                        if (ret >= 0 && ret <= len)
                        {
                            std::string xstats_path = dpath + "port_xstats/";
                            for (int i = 0; i < len; i++)
                            {
                                if (xstats[i].value > 0)
                                    status.set_param(xstats_path + std::string(xstats_names[i].name), xstats[i].value);
                            }
                        }
                    }
                }

                if (xstats) free(xstats);
                if (xstats_names) free(xstats_names);
            }
        }
    }

    bool PacketTxCore::connect(void)
    {

        // connect to the ring for incoming packets
        std::string upstream_ring_name = ring_name_str(config_.upstream_core, socket_id_, proc_idx_);
        struct rte_ring* upstream_ring = rte_ring_lookup(upstream_ring_name.c_str());
        if (upstream_ring == NULL)
        {
            // this needs to error out as there should always be upstream resources at this point
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Failed to Connect to upstream resources!");
            return false;
        }
        else
        {
            upstream_ring_ = upstream_ring;
            LOG4CXX_DEBUG_LEVEL(2, logger_, "Frame ready ring with name "
                << upstream_ring_name << " has already been created"
            );  
        }

        // connect to the ring for new memory locations packets
        std::string clear_frames_ring_name = ring_name_clear_frames(socket_id_);
        clear_frames_ring_ = rte_ring_lookup(clear_frames_ring_name.c_str());
        if (clear_frames_ring_ == NULL)
        {
            // this needs to error out as there should always be upstream resources at this point
            LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Failed to Connect to upstream resources!");
            return false;
        }
        else
        {
            LOG4CXX_DEBUG_LEVEL(2, logger_, "Frame ready ring with name "
                << upstream_ring_name << " has already been created"
            );  
        }

        while (tx_devices_.size() < config_.num_devices)
        {
            for (uint16_t port_id = 0; port_id < RTE_MAX_ETHPORTS; port_id++)
            {

                if (!rte_eth_dev_is_valid_port(port_id))
                    continue;

                std::string ring_name = "tx_port_" + std::to_string(port_id);

                rte_ring* ring = rte_ring_lookup( ring_name.c_str());

                if (!ring)
                    continue;

                bool already_added = false;

                for (auto& dev : tx_devices_)
                {
                    if (dev.port_id == port_id)
                    {
                        already_added = true;
                    }
                }

                if (already_added)
                    continue;

                TxDeviceContext ctx;
                ctx.port_id = port_id;
                ctx.ring = ring;
                tx_devices_.push_back(ctx);

                LOG4CXX_INFO(logger_, "Connected " << ring_name << " ("
                    << tx_devices_.size() << "/" << config_.num_devices << ")");
            }


            if (tx_devices_.size() < config_.num_devices)
            {
                LOG4CXX_INFO(
                    logger_,
                    "Waiting for TX rings..."
                );

                rte_delay_ms(100);
            }
        }

        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Connected to upstream resources successfully!");

        return true;
    }

    void PacketTxCore::configure(OdinData::IpcMessage& config)
    {
        // Update the config based from the passed IPCmessage

        LOG4CXX_INFO(logger_, config_.core_name << " : " << proc_idx_ << " Got update config.");

    }

    void PacketTxCore::requestConfiguration(OdinData::IpcMessage& reply)
    {
        return;
        // LOG4CXX_DEBUG(logger_, "Configuration requested for worker core");
    }

    void PacketTxCore::execute(const std::string& command, OdinData::IpcMessage& reply)
    {
        {
            reply.set_nack("PacketTxCore: unknown command: " + command);
        }
    }

    std::vector<std::pair<std::string, int>> PacketTxCore::requestCommands()
    {
        return {};
    }

    DPDKREGISTER(DpdkWorkerCore, PacketTxCore, "PacketTxCore");

}
