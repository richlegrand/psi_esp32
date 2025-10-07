/**
 * libdatachannel streamer example
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef fileparser_hpp
#define fileparser_hpp

#include <string>
#include <vector>
#include "stream.hpp"
#include "esp_heap_caps.h"

class FileParser: public StreamSource {
    std::string directory;
    std::string extension;
    uint64_t sampleDuration_us;
    uint64_t sampleTime_us = 0;
    uint32_t counter = -1;
    bool loop;
    uint64_t loopTimestampOffset = 0;

    // Pre-loaded file buffers (raw PSRAM allocation)
    struct PSRAMBuffer {
        void* data;
        size_t size;
        PSRAMBuffer() : data(nullptr), size(0) {}
        PSRAMBuffer(void* d, size_t s) : data(d), size(s) {}
        ~PSRAMBuffer() { if (data) heap_caps_free(data); }
        // Move constructor/assignment for safe ownership transfer
        PSRAMBuffer(PSRAMBuffer&& other) : data(other.data), size(other.size) {
            other.data = nullptr; other.size = 0;
        }
        PSRAMBuffer& operator=(PSRAMBuffer&& other) {
            if (this != &other) {
                if (data) heap_caps_free(data);
                data = other.data; size = other.size;
                other.data = nullptr; other.size = 0;
            }
            return *this;
        }
        PSRAMBuffer(const PSRAMBuffer&) = delete;
        PSRAMBuffer& operator=(const PSRAMBuffer&) = delete;
    };
    std::vector<PSRAMBuffer> preloadedSamples;

protected:
    rtc::binary sample = {};

    // Helper to pre-load all files into PSRAM (called from constructor in main thread)
    void preloadAllFiles();

public:
    FileParser(std::string directory, std::string extension, uint32_t samplesPerSecond, bool loop);
    virtual ~FileParser();
    virtual void start() override;
    virtual void stop() override;
    virtual void loadNextSample() override;

    rtc::binary getSample() override;
    uint64_t getSampleTime_us() override;
    uint64_t getSampleDuration_us() override;
};

#endif /* fileparser_hpp */
