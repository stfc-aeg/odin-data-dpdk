#ifndef HDF5_IMAGE_LOADER_H
#define HDF5_IMAGE_LOADER_H

#include <vector>
#include <cstdint>
#include <string>
#include <memory>
#include <hdf5.h>
#include <log4cxx/logger.h>
using namespace log4cxx;
using namespace log4cxx::helpers;
#include <DebugLevelLogger.h>


enum class ImageFormat { MONO, COLOR };
enum class CompressionType { NONE, BLOSC };
enum class PixelFormat {
    UINT8,
    UINT16,
    UINT32
};

/**
 * @class HDF5ImageLoader
 * @brief Loads and manages images from HDF5 files for simulation purposes
 *
 * This class provides functionality to load image sequences from HDF5 files
 * and generate frames with optional text overlays for camera simulation.
 */
class HDF5ImageLoader {
public:
    /**
     * @brief Constructor for HDF5ImageLoader
     * 
     * @param width Expected image width
     * @param height Expected image height
     * @param bitDepth Bit depth of the images
     * @param format Image format (MONO or COLOR)
     * @param datasetName Name of the dataset in the HDF5 file
     */
    HDF5ImageLoader(int width, int height, int bitDepth, ImageFormat format,
                    const std::string& datasetName = "default");
                    
    /**
     * @brief Destructor - closes HDF5 resources
     */
    ~HDF5ImageLoader();
    
    /**
     * @brief Load images from an HDF5 file
     * 
     * @param filePath Path to the HDF5 file
     * @throws std::runtime_error if file cannot be opened or format is invalid
     */
    void loadImagesFromHDF5(const std::string& filePath);
    
    /**
     * @brief Generate an image frame with optional overlays
     * 
     * @param cameraNumber Camera number to display in overlay
     * @param frameNumber Frame number for sequence and overlay
     */
    void generateImage(int cameraNumber, uint64_t frameNumber);
    
    /**
     * @brief Get the current image data
     * 
     * @return Reference to the current frame data
     */
    const std::vector<uint16_t>& getImageData() const;
    
    /**
     * @brief Get the total number of frames in the loaded HDF5 file
     * 
     * @return Number of frames available
     */
    size_t getFrameCount() const;
    
    /**
     * @brief Enable/disable text overlay drawing
     * 
     * @param enabled Whether to draw camera number text
     */
    void setTextDrawingEnabled(bool enabled);
    
    /**
     * @brief Enable/disable frame number overlay drawing
     * 
     * @param enabled Whether to draw frame number text
     */
    void setFrameNumberDrawingEnabled(bool enabled);
    
    /**
     * @brief Set the output pixel format
     * 
     * @param format Target pixel format for output
     */
    void setOutputFormat(PixelFormat format);

private:

    LoggerPtr logger_;
    // Font data for text rendering
    static const uint8_t Font40_Table[];
    
    // Basic image properties
    int width_;
    int height_;
    int bitDepth_;
    ImageFormat format_;
    
    // Frame management
    std::vector<uint16_t> current_frame_;
    struct FrameCache {
        size_t frameIndex;
        std::vector<uint16_t> data;
        bool valid;
    };
    FrameCache frameCache_;
    
    // HDF5 file handling
    hid_t h5file_;
    hid_t dataset_;
    std::string datasetName_;
    hsize_t totalFrames_;
    
    // Drawing configuration
    bool drawTextEnabled_;
    bool drawFrameNumberEnabled_;
    
    // Format conversion
    PixelFormat inputFormat_;
    PixelFormat outputFormat_;
    
    // Private methods
    
    /**
     * @brief Load a specific frame from the HDF5 file
     * 
     * @param frameIndex Index of the frame to load
     */
    void loadFrameFromHDF5(size_t frameIndex);
    
    /**
     * @brief Convert pixel formats between different bit depths
     * 
     * @param input Input pixel data
     * @param output Output pixel data
     */
    template<typename InType, typename OutType>
    void convertPixelFormat(const std::vector<InType>& input, std::vector<OutType>& output);
    
    /**
     * @brief Draw a single pixel at specified coordinates
     * 
     * @param x X coordinate
     * @param y Y coordinate 
     * @param value Pixel value
     */
    void drawPixel(int x, int y, uint16_t value);
    
    /**
     * @brief Draw a single character
     * 
     * @param c Character to draw
     * @param x X position
     * @param y Y position
     * @param value Pixel value for the character
     * @param scale Scale factor for the character
     */
    void drawChar(char c, int x, int y, uint16_t value, int scale);
    
    /**
     * @brief Draw a text string
     * 
     * @param text Text string to draw
     * @param x X position
     * @param y Y position
     * @param value Pixel value for the text
     * @param scale Scale factor for the text
     */
    void drawText(const std::string& text, int x, int y, uint16_t value, int scale);
};

#endif // HDF5_IMAGE_LOADER_H