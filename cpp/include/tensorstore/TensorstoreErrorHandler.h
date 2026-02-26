#ifndef INCLUDE_TENSORSTOREERRORHANDLER_H_
#define INCLUDE_TENSORSTOREERRORHANDLER_H_

#include <string>

namespace FrameProcessor {

// Error message formatting and handling for various tensorstore operations
class TensorstoreErrorHandler {
public:
    // Formats a dataset creation error message.
    static std::string FormatDatasetCreationError(
        const std::string& error_msg,
        const std::string& kvstore_driver,
        const std::string& s3_endpoint,
        const std::string& path
    );

    // Creates error message for S3 connection failures
    static std::string FormatS3ConnectionError(
        const std::string& s3_endpoint,
        const std::string& error_type
    );

    // Creates error message for schema mismatch
    static std::string FormatSchemaMismatchError(
        const std::string& path,
        const std::string& mismatch_type
    );

    // Creates a generic dataset error message
    static std::string FormatGenericDatasetError(
        const std::string& path,
        const std::string& error_msg
    );
};

} // namespace FrameProcessor

#endif // INCLUDE_TENSORSTOREERRORHANDLER_H_
