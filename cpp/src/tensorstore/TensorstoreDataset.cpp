#include "TensorstoreDataset.h"
#include <DebugLevelLogger.h>

using namespace log4cxx;
using namespace log4cxx::helpers;

namespace FrameProcessor {

tensorstore::Result<tensorstore::TensorStore<>> CreateDataset(
    const ::nlohmann::json& json_spec)
{
    static LoggerPtr logger(Logger::getLogger("FP.TensorstoreDataset"));
    
    auto context = tensorstore::Context::Default();
    
    try {
        // create|open allows reusing existing datasets or creating new ones.
        auto store_result = tensorstore::Open(
            json_spec,
            context,
            tensorstore::OpenMode::create | tensorstore::OpenMode::open,
            tensorstore::ReadWriteMode::read_write
        ).result();
        
        return store_result;
    } catch (const std::exception& e) {
        LOG4CXX_ERROR(logger, "An error occurred whilst creating the dataset: " << e.what());
        throw;
    }
}

} // namespace FrameProcessor
