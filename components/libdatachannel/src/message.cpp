/**
 * Copyright (c) 2019-2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "message.hpp"

#ifdef ESP32_PORT
#include <esp_heap_caps.h>
#endif

namespace rtc {

message_ptr make_message(size_t size, Message::Type type, unsigned int stream,
                         shared_ptr<Reliability> reliability) {
#ifdef ESP32_PORT
	auto message = std::allocate_shared<Message, PSRAMAllocator<Message>>(
	    PSRAMAllocator<Message>(), size, type);
#else
	auto message = std::make_shared<Message>(size, type);
#endif
	message->stream = stream;
	message->reliability = reliability;
	return message;
}

message_ptr make_message(binary &&data, Message::Type type, unsigned int stream, shared_ptr<Reliability> reliability) {
#ifdef ESP32_PORT
	auto message = std::allocate_shared<Message, PSRAMAllocator<Message>>(
	    PSRAMAllocator<Message>(), std::move(data), type);
#else
	auto message = std::make_shared<Message>(std::move(data), type);
#endif
	message->stream = stream;
	message->reliability = reliability;
	return message;
}
message_ptr make_message(binary &&data, shared_ptr<FrameInfo> frameInfo) {
#ifdef ESP32_PORT
	auto message = std::allocate_shared<Message, PSRAMAllocator<Message>>(
	    PSRAMAllocator<Message>(), std::move(data));
#else
	auto message = std::make_shared<Message>(std::move(data));
#endif
	message->frameInfo = frameInfo;
	return message;
}

message_ptr make_message(size_t size, message_ptr orig) {
	if (!orig)
		return nullptr;

#ifdef ESP32_PORT
	auto message = std::allocate_shared<Message, PSRAMAllocator<Message>>(
	    PSRAMAllocator<Message>(), size, orig->type);
#else
	auto message = std::make_shared<Message>(size, orig->type);
#endif
	std::copy(orig->begin(), orig->begin() + std::min(size, orig->size()), message->begin());
	message->stream = orig->stream;
	message->reliability = orig->reliability;
	message->frameInfo = orig->frameInfo;
	return message;
}

message_ptr make_message(message_variant data) {
	return std::visit( //
	    overloaded{
	        [&](binary data) { return make_message(std::move(data), Message::Binary); },
	        [&](string data) {
		        auto b = reinterpret_cast<const byte *>(data.data());
		        return make_message(b, b + data.size(), Message::String);
	        },
	    },
	    std::move(data));
}

#if RTC_ENABLE_MEDIA

message_ptr make_message_from_opaque_ptr(rtcMessage *&&message) {
	auto ptr = std::unique_ptr<Message>(reinterpret_cast<Message *>(message));
	return message_ptr(std::move(ptr));
}

#endif

message_variant to_variant(Message &&message) {
	switch (message.type) {
	case Message::String:
		return string(reinterpret_cast<const char *>(message.data()), message.size());
	default:
		return std::move(message);
	}
}

message_variant to_variant(const Message &message) {
	switch (message.type) {
	case Message::String:
		return string(reinterpret_cast<const char *>(message.data()), message.size());
	default:
		return message;
	}
}

} // namespace rtc
