/**
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "h264rtppacketizer.hpp"

#include "impl/internals.hpp"

#include <algorithm>
#include <cassert>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#ifdef ESP32_PORT
#include <esp_heap_caps.h>
#include <esp_timer.h>
extern bool g_log_frame_timing;  // Defined in httpd_server.cpp
#define HEAP_CHECK(label) do { \
    size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL); \
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM); \
    PLOG_WARNING << "HEAP[" << label << "]: DMA=" << dma_free << ", PSRAM=" << psram_free; \
} while(0)
#else
#define HEAP_CHECK(label)
#endif

namespace rtc {

H264RtpPacketizer::H264RtpPacketizer(shared_ptr<RtpPacketizationConfig> rtpConfig,
                                     size_t maxFragmentSize)
    : RtpPacketizer(std::move(rtpConfig)), mSeparator(Separator::Length), mMaxFragmentSize(maxFragmentSize) {}

H264RtpPacketizer::H264RtpPacketizer(Separator separator,
                                     shared_ptr<RtpPacketizationConfig> rtpConfig,
                                     size_t maxFragmentSize)
    : RtpPacketizer(rtpConfig), mSeparator(separator), mMaxFragmentSize(maxFragmentSize) {}

#ifdef ESP32_PORT
psram_vector<binary> H264RtpPacketizer::fragment(binary data) {
	uint64_t start = esp_timer_get_time();
	auto nalus = splitFrame(data);
	uint64_t split_end = esp_timer_get_time();
	auto fragments = NalUnit::GenerateFragments(nalus, mMaxFragmentSize);
	uint64_t frag_end = esp_timer_get_time();

	// Log if flag is set (synchronized with other pipeline layers)
	if (g_log_frame_timing) {
		uint32_t split_ms = (split_end - start) / 1000;
		uint32_t gen_ms = (frag_end - split_end) / 1000;
		PLOG_INFO << "  H264: " << data.size() << "B, split=" << split_ms
		          << "ms, gen=" << gen_ms << "ms (" << nalus.size()
		          << " NALs -> " << fragments.size() << " frags)";
	}

	return fragments;
}

psram_vector<NalUnit> H264RtpPacketizer::splitFrame(const binary &frame) {
	psram_vector<NalUnit> nalus;

	// ESP32 optimization: All NAL boundaries are in first ~30 bytes
	// Scan only the header region instead of the entire frame
	const size_t HEADER_SCAN_SIZE = 100;
#else
std::vector<binary> H264RtpPacketizer::fragment(binary data) {
	return NalUnit::GenerateFragments(splitFrame(data), mMaxFragmentSize);
}

std::vector<NalUnit> H264RtpPacketizer::splitFrame(const binary &frame) {
	std::vector<NalUnit> nalus;
#endif
	if (mSeparator == Separator::Length) {
		size_t index = 0;
		while (index < frame.size()) {
			assert(index + 4 < frame.size());
			if (index + 4 >= frame.size()) {
				LOG_WARNING << "Invalid NAL Unit data (incomplete length), ignoring!";
				break;
			}
			uint32_t length;
			std::memcpy(&length, frame.data() + index, sizeof(uint32_t));
			length = ntohl(length);
			auto naluStartIndex = index + 4;
			auto naluEndIndex = naluStartIndex + length;

			assert(naluEndIndex <= frame.size());
			if (naluEndIndex > frame.size()) {
				LOG_WARNING << "Invalid NAL Unit data (incomplete unit), ignoring!";
				break;
			}
			auto begin = frame.begin() + naluStartIndex;
			auto end = frame.begin() + naluEndIndex;
			nalus.emplace_back(begin, end);
			index = naluEndIndex;
		}
	} else {
#ifdef ESP32_PORT
		// ESP32 optimization: Scan only first HEADER_SCAN_SIZE bytes for NAL boundaries
		// All start codes are in the first ~30 bytes (SPS, PPS, IDR headers)
		// After that, it's all payload data for the final NAL unit
		NalUnitStartSequenceMatch match = NUSM_noMatch;
		size_t index = 0;

		// Skip first start code
		while (index < frame.size()) {
			match = NalUnit::StartSequenceMatchSucc(match, frame[index++], mSeparator);
			if (match == NUSM_longMatch || match == NUSM_shortMatch) {
				match = NUSM_noMatch;
				break;
			}
		}

		size_t naluStartIndex = index;
		size_t scanLimit = std::min(HEADER_SCAN_SIZE, frame.size());

		// Scan header region for additional NAL boundaries
		while (index < scanLimit) {
			match = NalUnit::StartSequenceMatchSucc(match, frame[index], mSeparator);
			if (match == NUSM_longMatch || match == NUSM_shortMatch) {
				auto sequenceLength = match == NUSM_longMatch ? 4 : 3;
				size_t naluEndIndex = index - sequenceLength;
				match = NUSM_noMatch;
				auto begin = frame.begin() + naluStartIndex;
				auto end = frame.begin() + naluEndIndex + 1;
				nalus.emplace_back(begin, end);
				naluStartIndex = index + 1;
			}
			index++;
		}

		// Final NAL unit extends from last boundary to end of frame
		auto begin = frame.begin() + naluStartIndex;
		auto end = frame.end();
		nalus.emplace_back(begin, end);
#else
		// Non-ESP32: Full frame scan (original logic)
		NalUnitStartSequenceMatch match = NUSM_noMatch;
		size_t index = 0;
		while (index < frame.size()) {
			match = NalUnit::StartSequenceMatchSucc(match, frame[index++], mSeparator);
			if (match == NUSM_longMatch || match == NUSM_shortMatch) {
				match = NUSM_noMatch;
				break;
			}
		}

		size_t naluStartIndex = index;

		while (index < frame.size()) {
			match = NalUnit::StartSequenceMatchSucc(match, frame[index], mSeparator);
			if (match == NUSM_longMatch || match == NUSM_shortMatch) {
				auto sequenceLength = match == NUSM_longMatch ? 4 : 3;
				size_t naluEndIndex = index - sequenceLength;
				match = NUSM_noMatch;
				auto begin = frame.begin() + naluStartIndex;
				auto end = frame.begin() + naluEndIndex + 1;
				nalus.emplace_back(begin, end);
				naluStartIndex = index + 1;
			}
			index++;
		}
		auto begin = frame.begin() + naluStartIndex;
		auto end = frame.end();
		nalus.emplace_back(begin, end);
#endif
	}

	return nalus;
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
