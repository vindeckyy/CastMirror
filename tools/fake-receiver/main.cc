#include "castcore/logger.h"
#include "castcore/types.h"
#include "cast_channel.pb.h"
#include <nlohmann/json.hpp>

#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <random>
#include <cstring>
#include <chrono>
#include <csignal>
#include <string>
#include <algorithm>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #define close closesocket
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
#endif

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/pem.h>

using namespace castcore;

static std::atomic<bool> g_terminate{false};
static void SignalHandler(int) { g_terminate = true; }

static void PrintHelp(const char* prog) {
  std::cout << "Usage: " << prog << " [tls_port] [udp_port] [options]\n"
            << "Fake Cast Receiver for bench and e2e testing\n"
            << "\nPositional:\n"
            << "  tls_port           TLS Cast channel port (default 8009, 0 for dynamic ephemeral)\n"
            << "  udp_port           UDP media port (default 33533, 0 for dynamic ephemeral)\n"
            << "\nOptions:\n"
            << "  --tls-port PORT    same as positional tls_port\n"
            << "  --udp-port PORT    same as positional udp_port\n"
            << "  --loss FRACTION    simulate packet loss rate (e.g. 0.05 for 5%)\n"
            << "  --jitter-min MS    simulate minimum jitter delay in ms\n"
            << "  --jitter-max MS    simulate maximum jitter delay in ms\n"
            << "  --help, -h         show this help\n"
            << "\nExamples:\n"
            << "  " << prog << " 8009 33533\n"
            << "  " << prog << " 28009 53533   # bench baseline ports\n"
            << "  " << prog << " --tls-port 28009 --udp-port 53533 --loss 0.05\n";
}

class FakeCastReceiver {
 public:
  FakeCastReceiver(uint16_t tls_port = 8009, uint16_t udp_port = 33533)
      : tls_port_(tls_port), udp_port_(udp_port) {}

  ~FakeCastReceiver() {
    Stop();
  }

  bool Start() {
    running_ = true;
    is_ready_ = false;
    tls_thread_ = std::thread(&FakeCastReceiver::TlsServerLoop, this);
    udp_thread_ = std::thread(&FakeCastReceiver::UdpMediaLoop, this);
    while (!is_ready_.load() && running_.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    LOG_INFO << "Fake Cast Receiver running on 127.0.0.1:" << tls_port_ << " and UDP :" << udp_port_;
    return true;
  }

  void Stop() {
    if (!running_.exchange(false)) return;
    if (server_fd_ >= 0) {
#if defined(_WIN32)
      shutdown(server_fd_, SD_BOTH);
#else
      shutdown(server_fd_, SHUT_RDWR);
#endif
      close(server_fd_);
      server_fd_ = -1;
    }
    if (udp_fd_ >= 0) {
#if defined(_WIN32)
      shutdown(udp_fd_, SD_BOTH);
#else
      shutdown(udp_fd_, SHUT_RDWR);
#endif
      close(udp_fd_);
      udp_fd_ = -1;
    }
    // Unblock accept/recvfrom with dummy connects strictly to 127.0.0.1 loopback
    {
      int dummy = socket(AF_INET, SOCK_STREAM, 0);
      if (dummy >= 0) {
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(tls_port_);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        connect(dummy, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        close(dummy);
      }
    }
    {
      int dummy_u = socket(AF_INET, SOCK_DGRAM, 0);
      if (dummy_u >= 0) {
        struct sockaddr_in uaddr{};
        uaddr.sin_family = AF_INET;
        uaddr.sin_port = htons(udp_port_);
        inet_pton(AF_INET, "127.0.0.1", &uaddr.sin_addr);
        sendto(dummy_u, "x", 1, 0, reinterpret_cast<struct sockaddr*>(&uaddr), sizeof(uaddr));
        close(dummy_u);
      }
    }
    if (tls_thread_.joinable()) tls_thread_.join();
    if (udp_thread_.joinable()) udp_thread_.join();
    LOG_INFO << "Fake Cast Receiver stopped";
  }

  bool IsReady() const { return is_ready_.load(); }
  uint16_t GetTlsPort() const { return tls_port_; }
  uint16_t GetUdpPort() const { return udp_port_; }
  std::string GetBoundIp() const { return bound_ip_; }
  uint32_t GetUdpPacketsReceived() const { return packets_received_.load(); }
  uint32_t GetVideoPacketsReceived() const { return video_packets_received_.load(); }
  uint32_t GetPacketsDropped() const { return packets_dropped_.load(); }
  uint32_t GetNonLoopbackPackets() const { return non_loopback_packets_.load(); }
  bool GetAllPacketsLoopback() const { return non_loopback_packets_.load() == 0; }

  // Synthetic Fault Injector Interface
  void SetSimulatedLossRate(double loss_fraction) {
    simulated_loss_rate_.store(std::clamp(loss_fraction, 0.0, 1.0));
  }

  void SetSimulatedJitter(int min_ms, int max_ms) {
    jitter_min_ms_.store(std::max(0, min_ms));
    jitter_max_ms_.store(std::max(min_ms, max_ms));
  }

  void TriggerNackBurst(uint32_t frame_id, const std::vector<uint16_t>& packets) {
    if (!has_sender_.load() || udp_fd_ < 0 || packets.empty()) return;
    ApplySimulatedJitter();

    size_t count = std::min(packets.size(), size_t{32});
    size_t pkt_len = 20 + count * 4;
    std::vector<uint8_t> rtcp(pkt_len, 0);
    rtcp[0] = 0x8F; // V=2, FMT=15 (CAST)
    rtcp[1] = 206;  // PT=206
    uint16_t words = static_cast<uint16_t>((pkt_len / 4) - 1);
    rtcp[2] = static_cast<uint8_t>((words >> 8) & 0xFF);
    rtcp[3] = static_cast<uint8_t>(words & 0xFF);

    // Receiver SSRC (10002), Sender SSRC (2)
    rtcp[4] = 0x00; rtcp[5] = 0x00; rtcp[6] = 0x27; rtcp[7] = 0x12;
    rtcp[8] = 0x00; rtcp[9] = 0x00; rtcp[10] = 0x00; rtcp[11] = 0x02;

    // 'CAST'
    rtcp[12] = 'C'; rtcp[13] = 'A'; rtcp[14] = 'S'; rtcp[15] = 'T';
    rtcp[16] = static_cast<uint8_t>(frame_id > 0 ? (frame_id - 1) & 0xFF : 0);
    rtcp[17] = static_cast<uint8_t>(count);
    rtcp[18] = 0x01; rtcp[19] = 0x90; // 400ms delay

    for (size_t i = 0; i < count; ++i) {
      size_t off = 20 + i * 4;
      rtcp[off] = static_cast<uint8_t>(frame_id & 0xFF);
      rtcp[off + 1] = static_cast<uint8_t>((packets[i] >> 8) & 0xFF);
      rtcp[off + 2] = static_cast<uint8_t>(packets[i] & 0xFF);
      rtcp[off + 3] = 0x00;
    }

    std::lock_guard<std::mutex> lock(sender_mutex_);
    sendto(udp_fd_, reinterpret_cast<const char*>(rtcp.data()), rtcp.size(), 0,
           reinterpret_cast<const struct sockaddr*>(&sender_addr_), sizeof(sender_addr_));
  }

  void TriggerPictureLossIndicator() {
    if (!has_sender_.load() || udp_fd_ < 0) return;
    ApplySimulatedJitter();

    uint8_t pli[12] = {
      0x81, 206, 0x00, 0x02,
      0x00, 0x00, 0x27, 0x12, // Receiver SSRC 10002
      0x00, 0x00, 0x00, 0x02  // Sender SSRC 2
    };
    std::lock_guard<std::mutex> lock(sender_mutex_);
    sendto(udp_fd_, reinterpret_cast<const char*>(pli), sizeof(pli), 0,
           reinterpret_cast<const struct sockaddr*>(&sender_addr_), sizeof(sender_addr_));
  }

 private:
  void ApplySimulatedJitter() {
    int min_j = jitter_min_ms_.load();
    int max_j = jitter_max_ms_.load();
    if (max_j > 0) {
      int delay = min_j;
      if (max_j > min_j) {
        delay = min_j + (std::rand() % (max_j - min_j + 1));
      }
      if (delay > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
      }
    }
  }

  void SendReceiverReport() {
    if (!has_sender_.load() || udp_fd_ < 0) return;
    ApplySimulatedJitter();

    uint8_t rtcp[32]{};
    rtcp[0] = 0x81; // V=2, RC=1
    rtcp[1] = 201;  // RR
    rtcp[2] = 0x00; rtcp[3] = 0x07; // Length = 7 words (32 bytes)
    rtcp[4] = 0x00; rtcp[5] = 0x00; rtcp[6] = 0x27; rtcp[7] = 0x12; // Receiver SSRC = 10002

    // Report block
    rtcp[8] = 0x00; rtcp[9] = 0x00; rtcp[10] = 0x00; rtcp[11] = 0x02; // Sender SSRC = 2
    double loss_rate = simulated_loss_rate_.load();
    rtcp[12] = static_cast<uint8_t>(std::clamp(static_cast<int>(loss_rate * 256.0), 0, 255));
    uint32_t dropped = packets_dropped_.load();
    rtcp[13] = static_cast<uint8_t>((dropped >> 16) & 0xFF);
    rtcp[14] = static_cast<uint8_t>((dropped >> 8) & 0xFF);
    rtcp[15] = static_cast<uint8_t>(dropped & 0xFF);

    int max_j = jitter_max_ms_.load();
    int min_j = jitter_min_ms_.load();
    uint32_t jitter_val = static_cast<uint32_t>((min_j + max_j) / 2);
    rtcp[20] = static_cast<uint8_t>((jitter_val >> 24) & 0xFF);
    rtcp[21] = static_cast<uint8_t>((jitter_val >> 16) & 0xFF);
    rtcp[22] = static_cast<uint8_t>((jitter_val >> 8) & 0xFF);
    rtcp[23] = static_cast<uint8_t>(jitter_val & 0xFF);

    std::lock_guard<std::mutex> lock(sender_mutex_);
    sendto(udp_fd_, reinterpret_cast<const char*>(rtcp), sizeof(rtcp), 0,
           reinterpret_cast<const struct sockaddr*>(&sender_addr_), sizeof(sender_addr_));
  }

  SSL_CTX* CreateServerContext() {
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();

    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) return nullptr;

    EVP_PKEY* pkey = EVP_RSA_gen(2048);

    X509* x509 = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 31536000L);
    X509_set_pubkey(x509, pkey);

    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC, (unsigned char*)"US", -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (unsigned char*)"Google Inc", -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (unsigned char*)"Fake Cast Receiver", -1, -1, 0);
    X509_set_issuer_name(x509, name);

    X509_sign(x509, pkey, EVP_sha256());

    SSL_CTX_use_certificate(ctx, x509);
    SSL_CTX_use_PrivateKey(ctx, pkey);

    X509_free(x509);
    EVP_PKEY_free(pkey);

    return ctx;
  }

  bool WriteExact(SSL* ssl, const uint8_t* buf, size_t len) {
    size_t written = 0;
    while (written < len) {
      int r = SSL_write(ssl, buf + written, static_cast<int>(len - written));
      if (r <= 0) return false;
      written += r;
    }
    return true;
  }

  bool ReadExact(SSL* ssl, uint8_t* buf, size_t len) {
    size_t read_bytes = 0;
    while (read_bytes < len) {
      int r = SSL_read(ssl, buf + read_bytes, static_cast<int>(len - read_bytes));
      if (r <= 0) return false;
      read_bytes += r;
    }
    return true;
  }

  void SendCastMsg(SSL* ssl, const std::string& ns, const std::string& payload,
                  const std::string& src, const std::string& dest) {
    proto::CastMessage msg;
    msg.set_protocol_version(proto::CastMessage::CASTV2_1_0);
    msg.set_source_id(src);
    msg.set_destination_id(dest);
    msg.set_namespace_(ns);
    msg.set_payload_type(proto::CastMessage::STRING);
    msg.set_payload_utf8(payload);

    std::string s;
    msg.SerializeToString(&s);
    uint32_t len = static_cast<uint32_t>(s.size());
    uint8_t hdr[4];
    hdr[0] = static_cast<uint8_t>((len >> 24) & 0xFF);
    hdr[1] = static_cast<uint8_t>((len >> 16) & 0xFF);
    hdr[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
    hdr[3] = static_cast<uint8_t>(len & 0xFF);
    std::vector<uint8_t> pkt(4 + len);
    std::memcpy(pkt.data(), hdr, 4);
    std::memcpy(pkt.data() + 4, s.data(), len);
    WriteExact(ssl, pkt.data(), pkt.size());
  }

  void TlsServerLoop() {
    SSL_CTX* ctx = CreateServerContext();
    if (!ctx) return;

    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#if defined(SO_REUSEPORT)
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&opt), sizeof(opt));
#endif

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(tls_port_);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
      LOG_ERROR << "Fake receiver failed to bind TLS port " << tls_port_ << " (" << strerror(errno) << ")";
      SSL_CTX_free(ctx);
      return;
    }

    socklen_t slen = sizeof(addr);
    getsockname(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), &slen);
    tls_port_ = ntohs(addr.sin_port);

    listen(server_fd_, 5);
    is_ready_ = true;

    while (running_.load()) {
      struct sockaddr_in client_addr{};
      socklen_t clen = sizeof(client_addr);
      int client_fd = accept(server_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &clen);
      if (client_fd < 0) {
        if (!running_.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }

      char client_ip[INET_ADDRSTRLEN]{};
      inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
      if (std::string(client_ip) != "127.0.0.1") {
        non_loopback_packets_++;
      }

      SSL* ssl = SSL_new(ctx);
      SSL_set_fd(ssl, client_fd);

      if (SSL_accept(ssl) <= 0) {
        SSL_free(ssl);
        close(client_fd);
        continue;
      }

      LOG_INFO << "[FakeReceiver] Client connected via TLS from " << client_ip << "!";

      uint8_t len_buf[4];
      while (running_.load()) {
        if (!ReadExact(ssl, len_buf, 4)) break;

        uint32_t msg_len = (static_cast<uint32_t>(len_buf[0]) << 24) |
                           (static_cast<uint32_t>(len_buf[1]) << 16) |
                           (static_cast<uint32_t>(len_buf[2]) << 8) |
                           static_cast<uint32_t>(len_buf[3]);

        std::vector<uint8_t> body(msg_len);
        if (!ReadExact(ssl, body.data(), msg_len)) break;

        proto::CastMessage in_msg;
        if (in_msg.ParseFromArray(body.data(), static_cast<int>(msg_len))) {
          std::string ns = in_msg.namespace_();
          std::string payload = in_msg.payload_utf8();

          if (ns == kNamespaceHeartbeat) {
            SendCastMsg(ssl, kNamespaceHeartbeat, "{\"type\":\"PONG\"}", in_msg.destination_id(), in_msg.source_id());
          } else if (ns == kNamespaceReceiver) {
            try {
              auto j = nlohmann::json::parse(payload);
              std::string type = j.value("type", "");
              int req_id = j.value("requestId", 1);

              if (type == "GET_STATUS") {
                nlohmann::json status;
                status["responseType"] = "RECEIVER_STATUS";
                status["requestId"] = req_id;
                status["status"]["applications"] = nlohmann::json::array();
                status["status"]["volume"]["level"] = 1.0;
                status["status"]["volume"]["muted"] = false;
                SendCastMsg(ssl, kNamespaceReceiver, status.dump(), "receiver-0", in_msg.source_id());
              } else if (type == "LAUNCH") {
                std::string app_id = j.value("appId", "0F5096E8");
                nlohmann::json status;
                status["responseType"] = "RECEIVER_STATUS";
                status["requestId"] = req_id;
                nlohmann::json app;
                app["appId"] = app_id;
                app["displayName"] = "Chrome Mirroring";
                app["sessionId"] = "fake-session-987654";
                app["transportId"] = "fake-transport-123456";
                app["isIdleScreen"] = false;
                status["status"]["applications"] = nlohmann::json::array({app});
                status["status"]["volume"]["level"] = 1.0;
                status["status"]["volume"]["muted"] = false;

                LOG_INFO << "[FakeReceiver] App " << app_id << " LAUNCHED -> returning RECEIVER_STATUS";
                SendCastMsg(ssl, kNamespaceReceiver, status.dump(), "receiver-0", in_msg.source_id());
              } else if (type == "STOP") {
                nlohmann::json status;
                status["responseType"] = "RECEIVER_STATUS";
                status["requestId"] = req_id;
                status["status"]["applications"] = nlohmann::json::array();
                status["status"]["volume"]["level"] = 1.0;
                status["status"]["volume"]["muted"] = false;

                LOG_INFO << "[FakeReceiver] STOP received -> returning to idle";
                SendCastMsg(ssl, kNamespaceReceiver, status.dump(), "receiver-0", in_msg.source_id());
              }
            } catch (...) {}
          } else if (ns == kNamespaceWebrtc) {
            try {
              auto j = nlohmann::json::parse(payload);
              std::string type = j.value("type", "");
              int seq = j.value("seqNum", 1001);

              if (type == "OFFER") {
                LOG_INFO << "[FakeReceiver] OFFER received! Replying with ANSWER on UDP port " << udp_port_;
                nlohmann::json ans;
                ans["type"] = "ANSWER";
                ans["seqNum"] = seq;
                ans["result"] = "ok";
                ans["answer"]["castMode"] = "mirroring";
                ans["answer"]["udpPort"] = udp_port_;
                ans["answer"]["sendIndexes"] = nlohmann::json::array({0, 1});
                ans["answer"]["ssrcs"] = nlohmann::json::array({10001, 10002});
                ans["answer"]["constraints"]["video"]["maxDimensions"] = {
                  {"width", 1920}, {"height", 1080}, {"frameRate", "60/1"}
                };

                SendCastMsg(ssl, kNamespaceWebrtc, ans.dump(), in_msg.destination_id(), in_msg.source_id());
              }
            } catch (...) {}
          }
        }
      }

      SSL_shutdown(ssl);
      SSL_free(ssl);
      close(client_fd);
    }

    SSL_CTX_free(ctx);
  }

  void UdpMediaLoop() {
    udp_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd_ < 0) return;
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(udp_port_);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (bind(udp_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
      LOG_ERROR << "Fake receiver failed to bind UDP port " << udp_port_ << " (" << strerror(errno) << ")";
      return;
    }

    socklen_t ulen = sizeof(addr);
    getsockname(udp_fd_, reinterpret_cast<struct sockaddr*>(&addr), &ulen);
    udp_port_ = ntohs(addr.sin_port);

    uint8_t buf[4096];
    uint32_t loop_count = 0;
    std::mt19937 rng(1337);
    std::uniform_real_distribution<double> dist(0.0, 1.0);

    while (running_.load()) {
      struct sockaddr_in saddr{};
      socklen_t slen = sizeof(saddr);
      ssize_t r = recvfrom(udp_fd_, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                           reinterpret_cast<struct sockaddr*>(&saddr), &slen);
      if (r > 0) {
        {
          std::lock_guard<std::mutex> lock(sender_mutex_);
          sender_addr_ = saddr;
          has_sender_ = true;
        }

        char src_ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &saddr.sin_addr, src_ip, sizeof(src_ip));
        if (std::string(src_ip) != "127.0.0.1") {
          non_loopback_packets_++;
        }

        double loss_rate = simulated_loss_rate_.load();
        bool is_rtp = (r >= 12 && (buf[1] & 0x7F) == 96);
        if (loss_rate > 0.0 && dist(rng) < loss_rate) {
          packets_dropped_++;
          if (is_rtp && r >= 19) {
            uint8_t fid = buf[13];
            uint16_t pid = (static_cast<uint16_t>(buf[14]) << 8) | buf[15];
            TriggerNackBurst(fid, {pid});
          }
          continue;
        }

        packets_received_++;
        if (is_rtp) {
          video_packets_received_++;
        }

        loop_count++;
        if (loop_count % 30 == 0) {
          SendReceiverReport();
        }
      } else {
        if (!running_.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
  }

  uint16_t tls_port_ = 8009;
  uint16_t udp_port_ = 33533;
  std::string bound_ip_ = "127.0.0.1";
  std::atomic<bool> running_{false};
  std::atomic<bool> is_ready_{false};
  std::atomic<uint32_t> packets_received_{0};
  std::atomic<uint32_t> video_packets_received_{0};
  std::atomic<uint32_t> packets_dropped_{0};
  std::atomic<uint32_t> non_loopback_packets_{0};

  std::atomic<double> simulated_loss_rate_{0.0};
  std::atomic<int> jitter_min_ms_{0};
  std::atomic<int> jitter_max_ms_{0};

  std::mutex sender_mutex_;
  struct sockaddr_in sender_addr_{};
  std::atomic<bool> has_sender_{false};

  int server_fd_ = -1;
  int udp_fd_ = -1;
  std::thread tls_thread_;
  std::thread udp_thread_;
};

int main(int argc, char** argv) {
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);

  LOG_INFO << "===========================================";
  LOG_INFO << "   CastMirror: Standalone Fake Receiver    ";
  LOG_INFO << "===========================================";

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h") { PrintHelp(argv[0]); return 0; }
  }

  uint16_t tls_port = 8009;
  uint16_t udp_port = 33533;
  double loss_rate = 0.0;
  int jitter_min = 0;
  int jitter_max = 0;
  std::vector<std::string> positionals;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--tls-port" && i + 1 < argc) {
      try { tls_port = static_cast<uint16_t>(std::stoi(argv[++i])); } catch (...) {}
    } else if (arg == "--udp-port" && i + 1 < argc) {
      try { udp_port = static_cast<uint16_t>(std::stoi(argv[++i])); } catch (...) {}
    } else if (arg == "--loss" && i + 1 < argc) {
      try { loss_rate = std::stod(argv[++i]); } catch (...) {}
    } else if (arg == "--jitter-min" && i + 1 < argc) {
      try { jitter_min = std::stoi(argv[++i]); } catch (...) {}
    } else if (arg == "--jitter-max" && i + 1 < argc) {
      try { jitter_max = std::stoi(argv[++i]); } catch (...) {}
    } else if (arg.rfind("--", 0) == 0) {
      LOG_WARN << "Unknown option: " << arg;
    } else {
      positionals.push_back(arg);
    }
  }

  if (positionals.size() >= 1) {
    try { tls_port = static_cast<uint16_t>(std::stoi(positionals[0])); } catch (...) {
      LOG_WARN << "Invalid TLS port: " << positionals[0];
    }
  }
  if (positionals.size() >= 2) {
    try { udp_port = static_cast<uint16_t>(std::stoi(positionals[1])); } catch (...) {
      LOG_WARN << "Invalid UDP port: " << positionals[1];
    }
  }

  FakeCastReceiver receiver(tls_port, udp_port);
  if (loss_rate > 0.0) {
    receiver.SetSimulatedLossRate(loss_rate);
  }
  if (jitter_max > 0 || jitter_min > 0) {
    receiver.SetSimulatedJitter(jitter_min, jitter_max);
  }
  receiver.Start();

  LOG_INFO << "Fake Receiver ready on 127.0.0.1 (TLS:" << receiver.GetTlsPort()
           << " UDP:" << receiver.GetUdpPort() << "). Press Ctrl+C to terminate.";

  while (!g_terminate.load()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  LOG_INFO << "Shutting down Fake Receiver on signal...";
  receiver.Stop();

  return 0;
}
