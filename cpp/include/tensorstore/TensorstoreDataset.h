#ifndef INCLUDE_TENSORSTOREDATASET_H_
#define INCLUDE_TENSORSTOREDATASET_H_

#include <tensorstore/tensorstore.h>
#include <tensorstore/context.h>
#include <tensorstore/open.h>
#include <tensorstore/open_mode.h>
#include <nlohmann/json.hpp>
#include <log4cxx/logger.h>

namespace FrameProcessor {

// Creates or opens a tensorstore dataset
tensorstore::Result<tensorstore::TensorStore<>> CreateDataset(
    const ::nlohmann::json& json_spec
);

} // namespace FrameProcessor

#endif // INCLUDE_TENSORSTOREDATASET_H_
