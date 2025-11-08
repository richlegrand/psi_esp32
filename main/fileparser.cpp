/**
 * libdatachannel streamer example
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "fileparser.hpp"
#include <fstream>
#include "esp_log.h"
#include "esp_heap_caps.h"

using namespace std;

static const char* TAG = "FileParser";

FileParser::FileParser(string directory, string extension, uint32_t samplesPerSecond, bool loop) {
    this->directory = directory;
    this->extension = extension;
    this->loop = loop;
    this->sampleDuration_us = 1000 * 1000 / samplesPerSecond;

    // Pre-load all files into PSRAM (safe since constructor runs in main thread)
    preloadAllFiles();
}

FileParser::~FileParser() {
	stop();
}

void FileParser::start() {
    sampleTime_us = std::numeric_limits<uint64_t>::max() - sampleDuration_us + 1;
    loadNextSample();
}

void FileParser::stop() {
    sample = {};
    sampleTime_us = 0;
    counter = -1;
}

void FileParser::preloadAllFiles() {
    ESP_LOGI(TAG, "Pre-loading files from %s", directory.c_str());
    preloadedSamples.clear();

    int fileIndex = 0;
    while (true) {
        string url = directory + "/" + to_string(fileIndex) + extension;
        ifstream source(url, ios_base::binary);
        if (!source) {
            // No more files
            break;
        }

        vector<char> contents((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());

        // Allocate in PSRAM
        void* psram_data = heap_caps_malloc(contents.size(), MALLOC_CAP_SPIRAM);
        if (!psram_data) {
            ESP_LOGE(TAG, "Failed to allocate %zu bytes in PSRAM for %s", contents.size(), url.c_str());
            continue;
        }

        // Copy to PSRAM
        memcpy(psram_data, contents.data(), contents.size());

        preloadedSamples.emplace_back(psram_data, contents.size());
        ESP_LOGI(TAG, "Loaded %s: %zu bytes", url.c_str(), contents.size());

        fileIndex++;
    }

    ESP_LOGI(TAG, "Pre-loaded %d files into PSRAM", fileIndex);
}

void FileParser::loadNextSample() {
    ++counter;

    if (preloadedSamples.empty()) {
        sample = {};
        return;
    }

    if (counter >= (int)preloadedSamples.size()) {
        if (loop && counter > 0) {
            loopTimestampOffset = sampleTime_us;
            counter = 0; // Reset to first file
        } else {
            sample = {};
            return;
        }
    }

    // Get pre-loaded sample from PSRAM (no flash access!)
    const auto& buffer = preloadedSamples[counter];
    auto* b = reinterpret_cast<const std::byte*>(buffer.data);
    sample.assign(b, b + buffer.size);
    sampleTime_us += sampleDuration_us;
}

rtc::binary FileParser::getSample() {
	return std::move(sample);
}

uint64_t FileParser::getSampleTime_us() {
	return sampleTime_us;
}

uint64_t FileParser::getSampleDuration_us() {
	return sampleDuration_us;
}

