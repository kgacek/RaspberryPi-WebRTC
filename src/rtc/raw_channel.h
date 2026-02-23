#ifndef RAW_CHANNEL_H_
#define RAW_CHANNEL_H_

#include <functional>
#include <memory>
#include <string>

#include <api/data_channel_interface.h>

#include "common/logging.h"
#include "common/utils.h"

// Simplified DataChannel wrapper for raw string messages (e.g., JSON)
// Does not use Protobuf parsing like RtcChannel
class RawChannel : public webrtc::DataChannelObserver,
                   public std::enable_shared_from_this<RawChannel> {
  public:
    using MessageHandler = std::function<void(const std::string &)>;

    static std::shared_ptr<RawChannel>
    Create(rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel);

    RawChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel);
    ~RawChannel();

    std::string id() const { return id_; }
    std::string label() const { return label_; }

    // webrtc::DataChannelObserver
    void OnStateChange() override;
    void OnMessage(const webrtc::DataBuffer &buffer) override;

    void SetMessageHandler(MessageHandler handler);
    void Send(const std::string &message);

  private:
    rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel_;
    std::string id_;
    std::string label_;
    MessageHandler message_handler_;
};

#endif // RAW_CHANNEL_H_
