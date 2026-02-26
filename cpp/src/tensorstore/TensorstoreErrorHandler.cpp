#include "TensorstoreErrorHandler.h"

namespace FrameProcessor {

std::string TensorstoreErrorHandler::FormatDatasetCreationError(
    const std::string& error_msg,
    const std::string& kvstore_driver,
    const std::string& s3_endpoint,
    const std::string& path)
{
    // Check for S3-specific errors
    if (kvstore_driver == "s3") {
        if (error_msg.find("invalid scheme") != std::string::npos) {
            return FormatS3ConnectionError(s3_endpoint, "invalid_scheme");
        }
        if (error_msg.find("permission_denied") != std::string::npos) {
            return FormatS3ConnectionError(s3_endpoint, "permission_denied");
        }
    }

    // Check for schema mismatch errors
    if (error_msg.find("chunk_shape") != std::string::npos) {
        return FormatSchemaMismatchError(path, "chunk_shape");
    }
    if (error_msg.find("data_type") != std::string::npos) {
        return FormatSchemaMismatchError(path, "data_type");
    }
    if (error_msg.find("shape") != std::string::npos) {
        return FormatSchemaMismatchError(path, "shape");
    }

    return FormatGenericDatasetError(path, error_msg);
}

std::string TensorstoreErrorHandler::FormatS3ConnectionError(
    const std::string& s3_endpoint,
    const std::string& error_type)
{
    if (error_type == "invalid_scheme") {
        return "S3 endpoint '" + s3_endpoint + 
               "' is missing http:// or https:// prefix";
    } else if (error_type == "permission_denied") {
        return "Cannot connect to S3 endpoint '" + s3_endpoint + 
               "'. Please check your credentials and permissions.";
    }
    return "S3 connection error for endpoint '" + s3_endpoint + "': " + error_type;
}

std::string TensorstoreErrorHandler::FormatSchemaMismatchError(
    const std::string& path,
    const std::string& mismatch_type)
{
    return "Dataset already exists at '" + path + "' with different " + mismatch_type + 
           ". Please use a different path.";
}

std::string TensorstoreErrorHandler::FormatGenericDatasetError(
    const std::string& path,
    const std::string& error_msg)
{
    return "Failed to create/open dataset at '" + path + "': " + error_msg;
}

} // namespace FrameProcessor
