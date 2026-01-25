/**
 * JpegDecoder - Hardware JPEG decoder implementation for ESP32-P4
 */

#include "jpeg_decoder.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char* TAG = "JpegDecoder";

JpegDecoder::JpegDecoder() : decoder_(nullptr), initialized_(false) {
    // Configure JPEG decoder engine
    jpeg_decode_engine_cfg_t decode_eng_cfg = {
        .timeout_ms = 100,  // 100ms timeout for decode operation
    };

    esp_err_t ret = jpeg_new_decoder_engine(&decode_eng_cfg, &decoder_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create JPEG decoder engine: %s", esp_err_to_name(ret));
        return;
    }

    initialized_ = true;
    ESP_LOGI(TAG, "JPEG hardware decoder initialized");
}

JpegDecoder::~JpegDecoder() {
    if (decoder_) {
        jpeg_del_decoder_engine(decoder_);
        decoder_ = nullptr;
    }
    initialized_ = false;
}

bool JpegDecoder::decode(const uint8_t* jpeg_data, size_t jpeg_len,
                         uint8_t* yuv_output, size_t yuv_output_size,
                         uint32_t* out_width, uint32_t* out_height) {
    if (!initialized_ || !decoder_) {
        ESP_LOGE(TAG, "Decoder not initialized");
        return false;
    }

    if (!jpeg_data || jpeg_len == 0 || !yuv_output || !out_width || !out_height) {
        ESP_LOGE(TAG, "Invalid parameters");
        return false;
    }

    // Parse JPEG header to get dimensions
    jpeg_decode_picture_info_t picture_info;
    esp_err_t ret = jpeg_decoder_get_info(jpeg_data, jpeg_len, &picture_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse JPEG header: %s", esp_err_to_name(ret));
        return false;
    }

    *out_width = picture_info.width;
    *out_height = picture_info.height;

    // Calculate required output buffer size for YUV420
    // YUV420: Y plane (width * height) + U plane (width/2 * height/2) + V plane (width/2 * height/2)
    size_t required_size = picture_info.width * picture_info.height * 3 / 2;
    if (yuv_output_size < required_size) {
        ESP_LOGE(TAG, "Output buffer too small: have %zu, need %zu", yuv_output_size, required_size);
        return false;
    }

    // Configure decoder for YUV420 output
    jpeg_decode_cfg_t decode_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_YUV420,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,  // Not used for YUV output
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,       // Not used for YUV output
    };

    // Perform decode
    uint32_t out_size = 0;
    ret = jpeg_decoder_process(decoder_, &decode_cfg, jpeg_data, jpeg_len,
                               yuv_output, yuv_output_size, &out_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JPEG decode failed: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_LOGD(TAG, "Decoded JPEG: %ux%u, output size: %u bytes",
             picture_info.width, picture_info.height, out_size);

    return true;
}
