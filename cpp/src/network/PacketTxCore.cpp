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
        DpdkWorkerCore(socket_id),
        logger_(Logger::getLogger("FP.PacketTxCore")),
        proc_idx_(fb_idx),
        decoder_(dynamic_cast<PacketProtocolDecoder *>(dpdkWorkCoreReferences.decoder)),
        frame_callback_(dpdkWorkCoreReferences.frame_callback),
        shared_buf_(dpdkWorkCoreReferences.shared_buf)
    {

        // Get the configuration container for this worker
        config_.resolve(dpdkWorkCoreReferences.core_config);

        LOG4CXX_INFO(logger_, "FP.PacketTxCore " << proc_idx_ << " Created with config:"
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
            {
                LOG4CXX_ERROR(logger_, "Error creating frame processed ring " << clear_frames_ring_name
                    << " : " << rte_strerror(rte_errno)
                );
                // TODO - this is fatal and should raise an exception
            }
        }
        else
        {
            {
                // Populate the ring with hugepages memory locations to the SMB

                for (int element = 0; element < shared_buf_->get_num_buffers(); element++)
                {

                    rte_ring_enqueue(clear_frames_ring_, shared_buf_->get_buffer_address(element));
                }
            }
        }

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

        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " starting up");
       
        // While loop to continuously dequeue frame objects
        while (likely(run_lcore_))
        {
            for (auto& tx_dev : tx_devices_)
            {
                struct rte_mbuf* mbuf = nullptr;

                int ret = rte_ring_dequeue(tx_dev.ring, (void**)&mbuf);

                if (ret != 0)
                {
                    continue;
                }

                // LOG4CXX_INFO(
                //     logger_,
                //     "Dequeued packet from port "
                //     << tx_dev.port_id
                //     << " ring count "
                //     << rte_ring_count(tx_dev.ring)
                // );

                uint16_t sent = rte_eth_tx_burst(tx_dev.port_id, 0,&mbuf, 1);

                if (sent == 0)
                {
                    LOG4CXX_ERROR(
                        logger_,
                        "TX FAILED port "
                        << tx_dev.port_id
                    );

                    rte_pktmbuf_free(mbuf);
                }
            }

            rte_pause();
        }
 
        LOG4CXX_INFO(logger_, "Core " << lcore_id_ << " completed");

        return true;
    }

    void PacketTxCore::stop(void)
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

    void PacketTxCore::status(OdinData::IpcMessage& status, const std::string& path)
    {
        LOG4CXX_DEBUG(logger_, "Status requested for PacketTxCore_" << proc_idx_
            << " from the DPDK plugin");

        std::string status_path = path + "/PacketTxCore_" + std::to_string(proc_idx_) + "/";

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

    DPDKREGISTER(DpdkWorkerCore, PacketTxCore, "PacketTxCore");

}
