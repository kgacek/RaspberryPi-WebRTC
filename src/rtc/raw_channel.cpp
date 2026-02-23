#include "raw_channel.h"

std::shared_ptr<RawChannel>
RawChannel::Create(rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) {
    return std::make_shared<RawChannel>(std::move(data_channel));
}

RawChannel::RawChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel)
    : data_channel_(data_channel),
      id_(Utils::GenerateUuid()),
      label_(data_channel->label()) {
    data_channel_->RegisterObserver(this);
}

RawChannel::~RawChannel() {
    DEBUG_PRINT("RawChannel (%s) is released!", label_.c_str());
    if (data_channel_) {
        data_channel_->UnregisterObserver();
    }
}

void RawChannel::OnStateChange() {
    webrtc::DataChannelInterface::DataState state = data_channel_->state();
    DEBUG_PRINT("[%s] OnStateChange => %s", data_channel_->label().c_str(),
                webrtc::DataChannelInterface::DataStateString(state));
}

void RawChannel::OnMessage(const webrtc::DataBuffer &buffer) {
    const uint8_t *data = buffer.data.data<uint8_t>();
    size_t length = buffer.data.size();
    std::string message(reinterpret_cast<const char *>(data), length);

    // Log occasionally to reduce spam (every 500th message)
    static int msg_count = 0;
    if (++msg_count % 500 == 0) {
        DEBUG_PRINT("[%s] Received %d messages (%zu bytes last)", label_.c_str(), msg_count, length);
    }

    if (message_handler_) {
        message_handler_(message);
    }
}

void RawChannel::SetMessageHandler(MessageHandler handler) {
    message_handler_ = std::move(handler);
}

void RawChannel::Send(const std::string &message) {
    if (!data_channel_ || data_channel_->state() != webrtc::DataChannelInterface::kOpen) {
        WARN_PRINT("[%s] Cannot send: DataChannel not open", label_.c_str());
        return;
    }

    webrtc::DataBuffer buffer(rtc::CopyOnWriteBuffer(message.data(), message.size()), true);
    if (!data_channel_->Send(buffer)) {
        ERROR_PRINT("[%s] Failed to send message", label_.c_str());
    }
}
