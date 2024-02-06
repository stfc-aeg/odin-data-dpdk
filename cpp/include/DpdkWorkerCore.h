#ifndef INCLUDE_DPDKWORKERCORE_H_
#define INCLUDE_DPDKWORKERCORE_H_

#include <boost/function.hpp>
#include <boost/bind.hpp>

#include <IpcMessage.h>

#include <rte_memory.h>
#include <rte_launch.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_debug.h>
#include <rte_errno.h>
#include "DataBlockFrame.h"
#include "ProtocolDecoder.h"
#include "DpdkSharedBuffer.h"
#include "DpdkCoreConfiguration.h"
#include "DpdkCoreLoader.h"

namespace FrameProcessor
{
    class DpdkWorkerCore
    {
    public:

        DpdkWorkerCore(int socket_id=SOCKET_ID_ANY) :
            lcore_id_(-1),
            socket_id_(socket_id),
            run_lcore_(false)
        {

        };
        virtual ~DpdkWorkerCore() {};

        virtual bool run(unsigned int lcore_id) = 0;
        virtual void stop(void) = 0;
        virtual void status(OdinData::IpcMessage& status, const std::string& path) = 0;
        virtual bool connect(void) = 0;

        inline unsigned int lcore_id(void) const { return lcore_id_; }
        inline unsigned int socket_id(void) const { return socket_id_; }

    protected:
        unsigned int lcore_id_;
        unsigned int socket_id_;
        bool run_lcore_;
    };
}

#endif /* INCLUDE_DPDKWORKERCORE_H_ */