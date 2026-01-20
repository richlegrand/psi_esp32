/**
 * Copyright (c) 2019 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_COMMON_H
#define RTC_COMMON_H

#ifdef RTC_STATIC
#define RTC_CPP_EXPORT
#else // dynamic library
#ifdef _WIN32
#ifdef RTC_EXPORTS
#define RTC_CPP_EXPORT __declspec(dllexport) // building the library
#else
#define RTC_CPP_EXPORT __declspec(dllimport) // using the library
#endif
#else // not WIN32
#define RTC_CPP_EXPORT
#endif
#endif

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602 // Windows 8
#endif
#ifdef _MSC_VER
#pragma warning(disable : 4251) // disable "X needs to have dll-interface..."
#endif
#endif

#ifndef RTC_ENABLE_WEBSOCKET
#define RTC_ENABLE_WEBSOCKET 1
#endif

#ifndef RTC_ENABLE_MEDIA
#define RTC_ENABLE_MEDIA 1
#endif

#include "rtc.h" // for C API defines

#include "utils.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// ESP32 PSRAM allocator - must be defined before rtc namespace
#ifdef ESP32_PORT
#include <esp_heap_caps.h>

template<typename T>
class PSRAMAllocator {
public:
	using value_type = T;

	PSRAMAllocator() = default;
	template<typename U> PSRAMAllocator(const PSRAMAllocator<U>&) {}

	T* allocate(std::size_t n) {
		size_t bytes = n * sizeof(T);
		if (auto p = static_cast<T*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM))) {
			return p;
		}
		throw std::bad_alloc();
	}

	void deallocate(T* p, std::size_t) {
		heap_caps_free(p);
	}
};

template<typename T, typename U>
bool operator==(const PSRAMAllocator<T>&, const PSRAMAllocator<U>&) { return true; }

template<typename T, typename U>
bool operator!=(const PSRAMAllocator<T>&, const PSRAMAllocator<U>&) { return false; }
#endif

namespace rtc {

using std::byte;
using std::nullopt;
using std::optional;
using std::shared_ptr;
using std::string;
using std::string_view;
using std::unique_ptr;
using std::variant;
using std::weak_ptr;

#ifdef ESP32_PORT
using binary = std::vector<byte, PSRAMAllocator<byte>>;

// Generic PSRAM vector for any type
template<typename T>
using psram_vector = std::vector<T, PSRAMAllocator<T>>;
#else
using binary = std::vector<byte>;

// On non-ESP32, psram_vector is just std::vector
template<typename T>
using psram_vector = std::vector<T>;
#endif

using message_variant = variant<binary, string>;

using std::int16_t;
using std::int32_t;
using std::int64_t;
using std::int8_t;
using std::ptrdiff_t;
using std::size_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;

} // namespace rtc

#endif
