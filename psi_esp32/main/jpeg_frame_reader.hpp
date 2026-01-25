/**
 * JpegFrameReader - Sequential JPEG file reader from LittleFS
 *
 * Reads JPEG files from a directory in alphabetical order and loops
 * back to the beginning when reaching the end.
 */

#ifndef JPEG_FRAME_READER_HPP
#define JPEG_FRAME_READER_HPP

#include <vector>
#include <string>
#include <cstdint>
#include <cstddef>

class JpegFrameReader {
public:
    /**
     * Create a JPEG frame reader for the specified directory
     *
     * @param frames_dir Path to directory containing JPEG files (e.g., "/littlefs/frames")
     */
    JpegFrameReader(const char* frames_dir);
    ~JpegFrameReader();

    /**
     * Initialize: scan directory for JPEG files and sort alphabetically
     *
     * @return true on success, false on failure
     */
    bool init();

    /**
     * Read next JPEG file (loops back to start at end)
     *
     * @param jpeg_data Output vector to receive JPEG file contents
     * @return true on success, false on failure
     */
    bool readNextFrame(std::vector<uint8_t>& jpeg_data);

    /**
     * Get total number of frames found
     */
    size_t getFrameCount() const { return frame_files_.size(); }

    /**
     * Get current frame index
     */
    size_t getCurrentIndex() const { return current_index_; }

private:
    std::string frames_dir_;
    std::vector<std::string> frame_files_;  // Full paths, sorted alphabetically
    size_t current_index_;
};

#endif // JPEG_FRAME_READER_HPP
