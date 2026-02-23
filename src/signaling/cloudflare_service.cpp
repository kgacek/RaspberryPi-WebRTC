#include "cloudflare_service.h"

#include "common/logging.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

// CURL write callback
static size_t WriteCallback(void *contents, size_t size, size_t nmemb, std::string *userp) {
    userp->append((char *)contents, size * nmemb);
    return size * nmemb;
}

std::shared_ptr<CloudflareService> CloudflareService::Create(Args args,
                                                             std::shared_ptr<Conductor> conductor,
                                                             boost::asio::io_context &ioc) {
    auto service = std::make_shared<CloudflareService>(args, conductor, ioc);
    return service;
}

CloudflareService::CloudflareService(Args args, std::shared_ptr<Conductor> conductor,
                                     boost::asio::io_context &ioc)
    : SignalingService(conductor, false),
      ioc_(ioc),
      heartbeat_timer_(ioc),
      active_session_timer_(ioc),
      cf_app_id_(args.cf_app_id),
      cf_token_(args.cf_token),
      arcaderally_api_(args.arcaderally_api),
      car_id_(args.car_id),
      car_api_key_(args.car_api_key) {

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl_ = curl_easy_init();
}

CloudflareService::~CloudflareService() {
    Disconnect();
    if (curl_) {
        curl_easy_cleanup(curl_);
    }
    curl_global_cleanup();
}

void CloudflareService::Connect() {
    INFO_PRINT("Connecting CloudflareService...");

    // 1. Create Cloudflare session (empty POST request)
    cloudflare_session_id_ = CreateCloudflareSession();
    if (cloudflare_session_id_.empty()) {
        ERROR_PRINT("Failed to create Cloudflare session");
        return;
    }

    INFO_PRINT("Cloudflare session created: %s", cloudflare_session_id_.c_str());

    // 2. Register with ArcadeRally backend
    if (!RegisterWithBackend()) {
        WARN_PRINT("Failed to register with backend");
    }

    // 3. Create video peer (will publish tracks in OnLocalSdp callback)
    PeerConfig config;
    config.has_candidates_in_sdp = false;

    video_peer_ = CreatePeer(config);
    if (!video_peer_) {
        ERROR_PRINT("Failed to create peer");
        return;
    }

    video_peer_->OnLocalSdp(
        [this](const std::string &peer_id, const std::string &sdp, const std::string &type) {
            OnLocalSdp(peer_id, sdp, type);
        });

    INFO_PRINT("Video peer created: %s", video_peer_->id().c_str());

    // Trigger SDP offer generation with small delay (gives tracks time to initialize)
    // Use weak_ptr to avoid keeping service alive if it's being destroyed
    auto self = weak_from_this();
    auto offer_timer = std::make_shared<boost::asio::steady_timer>(
        ioc_, std::chrono::milliseconds(100));
    
    offer_timer->async_wait([self, peer = video_peer_, offer_timer](const boost::system::error_code &ec) {
        DEBUG_PRINT("[CLOUDFLARE] Timer callback started, ec=%d", ec.value());
        if (!ec) {
            DEBUG_PRINT("[CLOUDFLARE] Timer no error, checking service lock");
            if (auto service = self.lock()) {
                DEBUG_PRINT("[CLOUDFLARE] Service locked successfully");
                if (peer) {
                    DEBUG_PRINT("[CLOUDFLARE] Peer is valid, calling CreateOffer()");
                    peer->CreateOffer();
                    INFO_PRINT("[CLOUDFLARE] CreateOffer() returned successfully");
                } else {
                    ERROR_PRINT("[CLOUDFLARE] Peer is null!");
                }
            } else {
                ERROR_PRINT("[CLOUDFLARE] Service already destroyed!");
            }
        } else {
            ERROR_PRINT("[CLOUDFLARE] Timer error: %s", ec.message().c_str());
        }
    });

    // 4. Start periodic tasks
    SendHeartbeat();
    CheckActiveSession();

    INFO_PRINT("CloudflareService connected successfully");
}

void CloudflareService::Disconnect() {
    INFO_PRINT("Disconnecting CloudflareService...");
    heartbeat_timer_.cancel();
    active_session_timer_.cancel();

    if (video_peer_) {
        video_peer_ = nullptr;
    }

    if (control_peer_) {
        control_peer_ = nullptr;
    }

    // Clear session state
    cloudflare_session_id_.clear();
    active_session_id_.clear();
    control_session_id_.clear();
}

std::string CloudflareService::CreateCloudflareSession() {
    std::string url = "https://rtc.live.cloudflare.com/v1/apps/" + cf_app_id_ + "/sessions/new";

    if (!curl_) {
        ERROR_PRINT("CURL not initialized");
        return "";
    }

    curl_easy_reset(curl_);

    std::string response_string;

    std::map<std::string, std::string> headers = {{"Authorization", "Bearer " + cf_token_}};

    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_POST, 1L);
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, 0L); // Empty body
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response_string);

    // Set headers
    struct curl_slist *header_list = nullptr;
    for (const auto &[key, value] : headers) {
        std::string header = key + ": " + value;
        header_list = curl_slist_append(header_list, header.c_str());
    }
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, header_list);

    CURLcode res = curl_easy_perform(curl_);
    curl_slist_free_all(header_list);

    if (res != CURLE_OK) {
        ERROR_PRINT("CURL POST failed: %s", curl_easy_strerror(res));
        return "";
    }

    long http_code = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);

    if (http_code != 200 && http_code != 201) {
        WARN_PRINT("HTTP POST returned code %ld for URL: %s", http_code, url.c_str());
        WARN_PRINT("Response: %s", response_string.c_str());
        return "";
    }

    try {
        auto response = nlohmann::json::parse(response_string);
        if (response.contains("sessionId")) {
            return response["sessionId"].get<std::string>();
        }
    } catch (const std::exception &e) {
        ERROR_PRINT("Failed to parse JSON response: %s", e.what());
    }

    ERROR_PRINT("Failed to create Cloudflare session");
    return "";
}

void CloudflareService::OnLocalSdp(const std::string &peer_id, const std::string &sdp,
                                   const std::string &type) {
    DEBUG_PRINT("[CLOUDFLARE] OnLocalSdp called, peer_id=%s, type=%s, sdp_length=%zu", 
                peer_id.c_str(), type.c_str(), sdp.length());
    
    if (type != "offer") {
        DEBUG_PRINT("[CLOUDFLARE] Not an offer, ignoring");
        return;
    }

    INFO_PRINT("Received local SDP offer, publishing track to Cloudflare");

    // Extract video mid from SDP
    DEBUG_PRINT("[CLOUDFLARE] Extracting video mid from SDP");
    std::string video_mid = ExtractVideoMid(sdp);
    DEBUG_PRINT("[CLOUDFLARE] Extracted video_mid: %s", video_mid.c_str());

    // Publish track to existing Cloudflare session
    std::string url = "https://rtc.live.cloudflare.com/v1/apps/" + cf_app_id_ + "/sessions/" +
                      cloudflare_session_id_ + "/tracks/new";

    nlohmann::json payload = {
        {"sessionDescription", {{"type", "offer"}, {"sdp", sdp}}},
        {"tracks", {{{"location", "local"}, {"trackName", "camera"}, {"mid", video_mid}}}}};

    std::map<std::string, std::string> headers = {{"Authorization", "Bearer " + cf_token_},
                                                  {"Content-Type", "application/json"}};

    auto response = HttpPost(url, payload, headers);

    if (response.contains("sessionDescription")) {
        std::string answer_sdp = response["sessionDescription"]["sdp"];

        auto peer = GetPeer(peer_id);
        if (peer) {
            peer->SetRemoteSdp(answer_sdp, "answer");
            INFO_PRINT("Video track published successfully");

            // Print session info
            INFO_PRINT("");
            INFO_PRINT("============================================================");
            INFO_PRINT("CAR STREAMING ACTIVE");
            INFO_PRINT("Car ID: %s", car_id_.c_str());
            INFO_PRINT("Cloudflare Session: %s", cloudflare_session_id_.c_str());
            INFO_PRINT("ArcadeRally API: %s", arcaderally_api_.c_str());
            INFO_PRINT("Players can now book slots and start sessions!");
            INFO_PRINT("============================================================");
            INFO_PRINT("");
        }
    } else {
        ERROR_PRINT("No sessionDescription in response");
    }
}

bool CloudflareService::RegisterWithBackend() {
    std::string url = arcaderally_api_ + "/cars/" + car_id_ + "/heartbeat";

    // Get current timestamp in ISO8601 format
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%SZ");

    nlohmann::json payload = {
        {"status", "ACTIVE"},
        {"metadata", {{"cloudflareSessionId", cloudflare_session_id_}, {"lastSeen", ss.str()}}}};

    std::map<std::string, std::string> headers = {{"X-Car-Api-Key", car_api_key_},
                                                  {"Content-Type", "application/json"}};

    auto response = HttpPost(url, payload, headers);

    if (!response.is_null()) {
        INFO_PRINT("Registered car %s with backend", car_id_.c_str());
        return true;
    }

    return false;
}

void CloudflareService::SendHeartbeat() {
    std::string url = arcaderally_api_ + "/cars/" + car_id_ + "/heartbeat";

    // Get current timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%SZ");

    nlohmann::json payload = {
        {"metadata", {{"cloudflareSessionId", cloudflare_session_id_}, {"lastSeen", ss.str()}}}};

    std::map<std::string, std::string> headers = {{"X-Car-Api-Key", car_api_key_},
                                                  {"Content-Type", "application/json"}};

    HttpPost(url, payload, headers);

    DEBUG_PRINT("Heartbeat sent");

    // Schedule next heartbeat
    heartbeat_timer_.expires_after(std::chrono::seconds(30));
    heartbeat_timer_.async_wait([this](const boost::system::error_code &ec) {
        if (!ec) {
            SendHeartbeat();
        }
    });
}

void CloudflareService::CheckActiveSession() {
    std::string url = arcaderally_api_ + "/cars/" + car_id_ + "/active-session";

    std::map<std::string, std::string> headers = {{"X-Car-Api-Key", car_api_key_}};

    auto response = HttpGet(url, headers);

    if (response.contains("hasActiveSession") && response["hasActiveSession"].get<bool>()) {
        auto session = response["session"];
        std::string session_id = session["id"];
        std::string control_session_id = session.value("controlSessionId", "");

        if (session_id != active_session_id_) {
            active_session_id_ = session_id;
            INFO_PRINT("New active session: %s", session_id.c_str());
        }

        if (!control_session_id.empty() && control_session_id != control_session_id_) {
            control_session_id_ = control_session_id;
            INFO_PRINT("Control session ID available: %s", control_session_id.c_str());
            SubscribeToControlDataChannel(control_session_id);
        }
    } else {
        if (!active_session_id_.empty()) {
            INFO_PRINT("Session ended");
            OnSessionEnded();
        }
    }

    // Schedule next check
    active_session_timer_.expires_after(std::chrono::seconds(5));
    active_session_timer_.async_wait([this](const boost::system::error_code &ec) {
        if (!ec) {
            CheckActiveSession();
        }
    });
}

void CloudflareService::OnSessionEnded() {
    active_session_id_.clear();
    control_session_id_.clear();

    // Send stop command to car (handled in Conductor)
    if (conductor) {
        // conductor->StopCar(); // This would need to be implemented
    }

    // Close control peer
    if (control_peer_) {
        control_peer_ = nullptr;
    }
}

void CloudflareService::SubscribeToControlDataChannel(const std::string &control_session_id) {
    INFO_PRINT("Subscribing to control DataChannel: %s", control_session_id.c_str());

    // TODO: Implement control DataChannel subscription
    // This requires:
    // 1. Create new Cloudflare session for subscribing
    // 2. Establish DataChannel transport
    // 3. Subscribe to remote control DataChannel
    // 4. Create local peer with negotiated DataChannel
    // 5. Setup message handler

    WARN_PRINT("Control DataChannel subscription not yet implemented");
}

std::string CloudflareService::ExtractVideoMid(const std::string &sdp) {
    std::string video_mid = "0"; // Default

    std::istringstream stream(sdp);
    std::string line;
    bool in_video_section = false;

    while (std::getline(stream, line)) {
        // Remove \r if present
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.find("m=video") == 0) {
            in_video_section = true;
        } else if (line.find("m=") == 0) {
            in_video_section = false;
        } else if (in_video_section && line.find("a=mid:") == 0) {
            video_mid = line.substr(6); // Skip "a=mid:"
            break;
        }
    }

    DEBUG_PRINT("Extracted video mid: %s", video_mid.c_str());
    return video_mid;
}

// HTTP Helper methods
nlohmann::json CloudflareService::HttpPost(const std::string &url, const nlohmann::json &payload,
                                           const std::map<std::string, std::string> &headers) {
    if (!curl_) {
        ERROR_PRINT("CURL not initialized");
        return nlohmann::json();
    }

    curl_easy_reset(curl_);

    std::string response_string;
    std::string payload_str = payload.dump();

    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_POST, 1L);
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, payload_str.c_str());
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response_string);

    // Set headers
    struct curl_slist *header_list = nullptr;
    for (const auto &[key, value] : headers) {
        std::string header = key + ": " + value;
        header_list = curl_slist_append(header_list, header.c_str());
    }
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, header_list);

    CURLcode res = curl_easy_perform(curl_);
    curl_slist_free_all(header_list);

    if (res != CURLE_OK) {
        ERROR_PRINT("CURL POST failed: %s", curl_easy_strerror(res));
        return nlohmann::json();
    }

    long http_code = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);

    if (http_code != 200 && http_code != 201) {
        WARN_PRINT("HTTP POST returned code %ld for URL: %s", http_code, url.c_str());
        DEBUG_PRINT("Response: %s", response_string.c_str());
    }

    try {
        return nlohmann::json::parse(response_string);
    } catch (const std::exception &e) {
        ERROR_PRINT("Failed to parse JSON response: %s", e.what());
        return nlohmann::json();
    }
}

nlohmann::json CloudflareService::HttpGet(const std::string &url,
                                          const std::map<std::string, std::string> &headers) {
    if (!curl_) {
        ERROR_PRINT("CURL not initialized");
        return nlohmann::json();
    }

    curl_easy_reset(curl_);

    std::string response_string;

    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response_string);

    // Set headers
    struct curl_slist *header_list = nullptr;
    for (const auto &[key, value] : headers) {
        std::string header = key + ": " + value;
        header_list = curl_slist_append(header_list, header.c_str());
    }
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, header_list);

    CURLcode res = curl_easy_perform(curl_);
    curl_slist_free_all(header_list);

    if (res != CURLE_OK) {
        ERROR_PRINT("CURL GET failed: %s", curl_easy_strerror(res));
        return nlohmann::json();
    }

    long http_code = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);

    if (http_code != 200) {
        WARN_PRINT("HTTP GET returned code %ld for URL: %s", http_code, url.c_str());
    }

    try {
        return nlohmann::json::parse(response_string);
    } catch (const std::exception &e) {
        ERROR_PRINT("Failed to parse JSON response: %s", e.what());
        return nlohmann::json();
    }
}

nlohmann::json CloudflareService::HttpPut(const std::string &url, const nlohmann::json &payload,
                                          const std::map<std::string, std::string> &headers) {
    if (!curl_) {
        ERROR_PRINT("CURL not initialized");
        return nlohmann::json();
    }

    curl_easy_reset(curl_);

    std::string response_string;
    std::string payload_str = payload.dump();

    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, payload_str.c_str());
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response_string);

    // Set headers
    struct curl_slist *header_list = nullptr;
    for (const auto &[key, value] : headers) {
        std::string header = key + ": " + value;
        header_list = curl_slist_append(header_list, header.c_str());
    }
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, header_list);

    CURLcode res = curl_easy_perform(curl_);
    curl_slist_free_all(header_list);

    if (res != CURLE_OK) {
        ERROR_PRINT("CURL PUT failed: %s", curl_easy_strerror(res));
        return nlohmann::json();
    }

    long http_code = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);

    if (http_code != 200 && http_code != 201) {
        WARN_PRINT("HTTP PUT returned code %ld for URL: %s", http_code, url.c_str());
    }

    try {
        return nlohmann::json::parse(response_string);
    } catch (const std::exception &e) {
        ERROR_PRINT("Failed to parse JSON response: %s", e.what());
        return nlohmann::json();
    }
}
