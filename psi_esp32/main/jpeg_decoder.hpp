/**
 * JpegDecoder - Hardware JPEG decoder wrapper for ESP32-P4
 *
 * Provides simple interface to ESP32-P4's hardware JPEG decoder for
 * converting JPEG bitstreams to YUV420 format for H.264 encoding.
 */

#ifndef JPEG_DECODER_HPP
#define JPEG_DECODER_HPP

#include <cstdint>
#include <cstddef>

extern "C" {
#include "driver/jpeg_decode.h"
}

class JpegDecoder {
public:
    JpegDecoder();
    ~JpegDecoder();

    /**
     * Decode JPEG to YUV420 output buffer
     *
     * @param jpeg_data Input JPEG bitstream
     * @param jpeg_len Length of JPEG data
     * @param yuv_output Output buffer for YUV420 data (must be pre-allocated)
     * @param yuv_output_size Size of output buffer
     * @param out_width Returns decoded image width
     * @param out_height Returns decoded image height
     * @return true on success, false on failure
     */
    bool decode(const uint8_t* jpeg_data, size_t jpeg_len,
                uint8_t* yuv_output, size_t yuv_output_size,
                uint32_t* out_width, uint32_t* out_height);

private:
    jpeg_decoder_handle_t decoder_;
    bool initialized_;
};

#endif // JPEG_DECODER_HPP
