/**
 * JpegFrameReader - Sequential JPEG file reader from LittleFS
 */

#include "jpeg_frame_reader.hpp"
#include "esp_log.h"
#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

static const char* TAG = "JpegFrameReader";

JpegFrameReader::JpegFrameReader(const char* frames_dir)
    : frames_dir_(frames_dir), current_index_(0) {
}

JpegFrameReader::~JpegFrameReader() {
}

// Helper function: check if filename ends with .jpg or .jpeg (case-insensitive)
static bool isJpegFile(const char* filename) {
    size_t len = strlen(filename);
    if (len < 5) return false;  // Minimum: "a.jpg"

    const char* ext = filename + len - 4;
    if (strcasecmp(ext, ".jpg") == 0) return true;

    if (len >= 6) {
        ext = filename + len - 5;
        if (strcasecmp(ext, ".jpeg") == 0) return true;
    }

    return false;
}

bool JpegFrameReader::init() {
    DIR* dir = opendir(frames_dir_.c_str());
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", frames_dir_.c_str());
        return false;
    }

    // Scan directory for JPEG files
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        // Skip "." and ".."
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // Check if it's a JPEG file
        if (isJpegFile(entry->d_name)) {
            // Build full path
            std::string full_path = frames_dir_ + "/" + entry->d_name;
            frame_files_.push_back(full_path);
            ESP_LOGD(TAG, "Found JPEG: %s", entry->d_name);
        }
    }

    closedir(dir);

    if (frame_files_.empty()) {
        ESP_LOGE(TAG, "No JPEG files found in %s", frames_dir_.c_str());
        return false;
    }

    // Sort alphabetically
    std::sort(frame_files_.begin(), frame_files_.end());

    ESP_LOGI(TAG, "Found %zu JPEG files in %s", frame_files_.size(), frames_dir_.c_str());
    for (size_t i = 0; i < frame_files_.size(); i++) {
        ESP_LOGD(TAG, "  [%zu] %s", i, frame_files_[i].c_str());
    }

    current_index_ = 0;
    return true;
}

bool JpegFrameReader::readNextFrame(std::vector<uint8_t>& jpeg_data) {
    if (frame_files_.empty()) {
        ESP_LOGE(TAG, "No frames available (did you call init()?)");
        return false;
    }

    // Get current frame path
    const std::string& path = frame_files_[current_index_];

    // Open file
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", path.c_str());
        return false;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        ESP_LOGE(TAG, "Invalid file size: %ld for %s", file_size, path.c_str());
        fclose(f);
        return false;
    }

    // Resize output buffer
    jpeg_data.resize(file_size);

    // Read file contents
    size_t bytes_read = fread(jpeg_data.data(), 1, file_size, f);
    fclose(f);

    if (bytes_read != (size_t)file_size) {
        ESP_LOGE(TAG, "Failed to read complete file: read %zu of %ld bytes", bytes_read, file_size);
        return false;
    }

    ESP_LOGD(TAG, "Read frame %zu/%zu: %s (%zu bytes)",
             current_index_ + 1, frame_files_.size(), path.c_str(), bytes_read);

    // Advance to next frame (loop at end)
    current_index_++;
    if (current_index_ >= frame_files_.size()) {
        current_index_ = 0;
        ESP_LOGD(TAG, "Looping back to first frame");
    }

    return true;
}
