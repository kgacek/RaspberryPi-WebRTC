#ifndef CLOUDFLARE_SERVICE_H_
#define CLOUDFLARE_SERVICE_H_

#include "signaling_service.h"

#include <boost/asio.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

class CloudflareService : public SignalingService,
                          public std::enable_shared_from_this<CloudflareService> {
  public:
    static std::shared_ptr<CloudflareService>
    Create(Args args, std::shared_ptr<Conductor> conductor, boost::asio::io_context &ioc);

    CloudflareService(Args args, std::shared_ptr<Conductor> conductor,
                      boost::asio::io_context &ioc);

    ~CloudflareService();

  protected:
    void Connect() override;
    void Disconnect() override;

  private:
    // Cloudflare Calls API
    std::string CreateCloudflareSession();
    bool PublishTrack(const std::string &sdp, std::string &answer_sdp);

    // ArcadeRally Backend API
    bool RegisterWithBackend();
    void SendHeartbeat();
    void CheckActiveSession();

    // Active session management
    void OnActiveSessionDetected(const std::string &session_id,
                                 const std::string &control_session_id);
    void OnSessionEnded();
    void SubscribeToControlDataChannel(const std::string &control_session_id);

    // SDP handling
    void OnLocalSdp(const std::string &peer_id, const std::string &sdp, const std::string &type);

    // HTTP helpers
    nlohmann::json HttpPost(const std::string &url, const nlohmann::json &payload,
                            const std::map<std::string, std::string> &headers);
    nlohmann::json HttpGet(const std::string &url,
                           const std::map<std::string, std::string> &headers);
    nlohmann::json HttpPut(const std::string &url, const nlohmann::json &payload,
                           const std::map<std::string, std::string> &headers);
    std::string ExtractVideoMid(const std::string &sdp);

    // State
    boost::asio::io_context &ioc_;
    boost::asio::steady_timer heartbeat_timer_;
    boost::asio::steady_timer active_session_timer_;

    std::string cloudflare_session_id_;
    std::string active_session_id_;
    std::string control_session_id_;

    rtc::scoped_refptr<RtcPeer> video_peer_;
    rtc::scoped_refptr<RtcPeer> control_peer_;

    // Config from Args
    std::string cf_app_id_;
    std::string cf_token_;
    std::string arcaderally_api_;
    std::string car_id_;
    std::string car_api_key_;

    // CURL handle for thread safety
    CURL *curl_;
};

#endif // CLOUDFLARE_SERVICE_H_
