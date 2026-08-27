#include <gtest/gtest.h>
#include "castcore/logger.h"
#include "castcore/cast_engine.h"
#include "castcore/cast_session.h"
#include "castcore/state_machine.h"
#include "cast_channel.pb.h"
#include <nlohmann/json.hpp>

#include <thread>
#include <chrono>
#include <atomic>
#include <functional>

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

using namespace castcore;

class TestReceiverServer {
 public:
  TestReceiverServer() = default;

  ~TestReceiverServer() { Stop(); }

  void Start() {
    running_ = true;
    is_ready_ = false;
    tls_thread_ = std::thread(&TestReceiverServer::TlsLoop, this);
    udp_thread_ = std::thread(&TestReceiverServer::UdpLoop, this);
    while (!is_ready_.load() && running_.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  void Stop() {
    if (!running_.exchange(false)) return;
    if (server_fd_ >= 0) {
      shutdown(server_fd_, SHUT_RDWR);
      close(server_fd_);
      server_fd_ = -1;
    }
    int dummy = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(tls_port_);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    connect(dummy, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    close(dummy);

    int dummy_u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in uaddr{};
    uaddr.sin_family = AF_INET;
    uaddr.sin_port = htons(udp_port_);
    inet_pton(AF_INET, "127.0.0.1", &uaddr.sin_addr);
    sendto(dummy_u, "x", 1, 0, reinterpret_cast<struct sockaddr*>(&uaddr), sizeof(uaddr));
    close(dummy_u);

    if (udp_fd_ >= 0) { close(udp_fd_); udp_fd_ = -1; }
    if (tls_thread_.joinable()) tls_thread_.join();
    if (udp_thread_.joinable()) udp_thread_.join();
  }

  uint16_t GetTlsPort() const { return tls_port_; }
  uint16_t GetUdpPort() const { return udp_port_; }
  uint32_t GetUdpPacketsReceived() const { return packets_received_.load(); }
  uint32_t GetVideoPacketsReceived() const { return video_packets_received_.load(); }
  void DisconnectClient() {
    int fd = current_client_fd_.load();
    if (fd >= 0) {
      shutdown(fd, SHUT_RDWR);
    }
  }



 private:
  SSL_CTX* CreateCtx() {
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());

    EVP_PKEY* pkey = EVP_RSA_gen(2048);

    X509* x509 = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 31536000L);
    X509_set_pubkey(x509, pkey);

    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (unsigned char*)"TestCast", -1, -1, 0);
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
      if (r <= 0) {
        int err = SSL_get_error(ssl, r);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }
        char err_buf[256];
        ERR_error_string_n(ERR_get_error(), err_buf, sizeof(err_buf));
        LOG_INFO << "[TestReceiver] SSL_read closed: r=" << r << ", err=" << err << " (" << err_buf << ")";
        return false;
      }
      read_bytes += r;
    }
    return true;
  }

  void SendMsg(SSL* ssl, const std::string& ns, const std::string& payload, const std::string& src, const std::string& dest) {
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

  void TlsLoop() {
    SSL_CTX* ctx = CreateCtx();
    if (!ctx) return;

    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#if defined(SO_REUSEPORT)
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&opt), sizeof(opt));
#endif

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0; // Dynamic ephemeral port
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
      LOG_ERROR << "[TestReceiver] Failed to bind dynamic TLS port: " << strerror(errno);
      SSL_CTX_free(ctx);
      return;
    }
    socklen_t slen = sizeof(addr);
    getsockname(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), &slen);
    tls_port_ = ntohs(addr.sin_port);

    listen(server_fd_, 5);
    LOG_INFO << "[TestReceiver] Listening for Cast TLS on port " << tls_port_;
    is_ready_ = true;

    while (running_.load()) {
      struct sockaddr_in caddr{};
      socklen_t clen = sizeof(caddr);
      int cfd = accept(server_fd_, reinterpret_cast<struct sockaddr*>(&caddr), &clen);
      if (cfd < 0) break;
      current_client_fd_.store(cfd);

      SSL* ssl = SSL_new(ctx);
      SSL_set_fd(ssl, cfd);

      if (SSL_accept(ssl) > 0) {
        LOG_INFO << "[TestReceiver] Client SSL_accept succeeded!";
        uint8_t len_buf[4];
        while (running_.load()) {
          if (!ReadExact(ssl, len_buf, 4)) {
            break;
          }

          uint32_t msg_len = (static_cast<uint32_t>(len_buf[0]) << 24) |
                             (static_cast<uint32_t>(len_buf[1]) << 16) |
                             (static_cast<uint32_t>(len_buf[2]) << 8) |
                             static_cast<uint32_t>(len_buf[3]);

          std::vector<uint8_t> body(msg_len);
          if (!ReadExact(ssl, body.data(), msg_len)) {
            break;
          }

          proto::CastMessage in_msg;
          if (in_msg.ParseFromArray(body.data(), static_cast<int>(msg_len))) {
            std::string ns = in_msg.namespace_();
            std::string payload = in_msg.payload_utf8();
            LOG_INFO << "[TestReceiver] Received NS: " << ns << " Payload: " << payload;

            if (ns == kNamespaceHeartbeat) {
              SendMsg(ssl, kNamespaceHeartbeat, "{\"type\":\"PONG\"}", in_msg.destination_id(), in_msg.source_id());
            } else if (ns == kNamespaceReceiver) {
              try {
                auto j = nlohmann::json::parse(payload);
                std::string type = j.value("type", "");
                int req_id = j.value("requestId", 1);
                if (type == "LAUNCH") {
                  nlohmann::json status;
                  status["responseType"] = "RECEIVER_STATUS";
                  status["requestId"] = req_id;
                  nlohmann::json app;
                  app["appId"] = j.value("appId", "0F5096E8");
                  app["displayName"] = "Chrome Mirroring";
                  app["sessionId"] = "session-e2e-123";
                  app["transportId"] = "transport-e2e-456";
                  app["isIdleScreen"] = false;
                  status["status"]["applications"] = nlohmann::json::array({app});
                  status["status"]["volume"]["level"] = 1.0;
                  status["status"]["volume"]["muted"] = false;
                  SendMsg(ssl, kNamespaceReceiver, status.dump(), "receiver-0", in_msg.source_id());
                } else if (type == "STOP") {
                  nlohmann::json status;
                  status["responseType"] = "RECEIVER_STATUS";
                  status["requestId"] = req_id;
                  status["status"]["applications"] = nlohmann::json::array();
                  SendMsg(ssl, kNamespaceReceiver, status.dump(), "receiver-0", in_msg.source_id());
                }
              } catch (...) {}
            } else if (ns == kNamespaceWebrtc) {
              try {
                auto j = nlohmann::json::parse(payload);
                if (j.value("type", "") == "OFFER") {
                  nlohmann::json ans;
                  ans["type"] = "ANSWER";
                  ans["seqNum"] = j.value("seqNum", 1001);
                  ans["result"] = "ok";
                  ans["answer"]["castMode"] = "mirroring";
                  ans["answer"]["udpPort"] = udp_port_;
                  ans["answer"]["sendIndexes"] = nlohmann::json::array({0, 1});
                  ans["answer"]["ssrcs"] = nlohmann::json::array({10001, 10002});
                  SendMsg(ssl, kNamespaceWebrtc, ans.dump(), in_msg.destination_id(), in_msg.source_id());
                }
              } catch (...) {}
            }
          }
        }
        SSL_shutdown(ssl);
      }
      SSL_free(ssl);
      current_client_fd_.store(-1);
      close(cfd);
    }

    SSL_CTX_free(ctx);
  }

  void UdpLoop() {
    udp_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0; // Dynamic ephemeral UDP port
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    bind(udp_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    socklen_t ulen = sizeof(addr);
    getsockname(udp_fd_, reinterpret_cast<struct sockaddr*>(&addr), &ulen);
    udp_port_ = ntohs(addr.sin_port);

    uint8_t buf[4096];
    struct sockaddr_in saddr{};
    socklen_t slen = sizeof(saddr);

    while (running_.load()) {
      ssize_t r = recvfrom(udp_fd_, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                           reinterpret_cast<struct sockaddr*>(&saddr), &slen);
      if (r > 0) {
        packets_received_++;
        if (r >= 2 && (buf[1] & 0x7F) == 96) {
          video_packets_received_++;
        }
      }

    }
  }

  uint16_t tls_port_ = 0;
  uint16_t udp_port_ = 0;
  std::atomic<bool> running_{false};
  std::atomic<bool> is_ready_{false};
  std::atomic<uint32_t> packets_received_{0};
  std::atomic<uint32_t> video_packets_received_{0};
  std::atomic<int> current_client_fd_{-1};

  int server_fd_ = -1;
  int udp_fd_ = -1;
  std::thread tls_thread_;
  std::thread udp_thread_;
};

TEST(CastE2ETest, FullEndToEndSessionWithSimulatedReceiver) {
  TestReceiverServer server;
  server.Start();

  uint16_t test_tls_port = server.GetTlsPort();


  auto& engine = CastEngine::Instance();
  engine.Initialize();
  AppConfig saved_cfg = ConfigStore::Instance().Get();

  CastDevice dev;
  dev.id = "test-e2e-device";
  dev.name = "Test Chromecast TV";
  dev.model_name = "Chromecast Ultra";
  dev.ip_address = "127.0.0.1";
  dev.port = test_tls_port;
  dev.capabilities = kCapVideoOut | kCapAudioOut;

  engine.GetDiscovery().AddOrUpdateDevice(dev);

  bool ok = engine.StartCasting(dev.id, 0, QualityPreset::kBalanced, true);
  EXPECT_TRUE(ok);
  EXPECT_EQ(engine.GetState(), SessionState::kStreaming);

  // Allow streaming for 1.5 seconds
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));

  StreamStats stats = engine.GetStats();
  EXPECT_GT(stats.frames_sent, 0u);
  EXPECT_GT(stats.packets_sent, 0u);
  EXPECT_GT(server.GetUdpPacketsReceived(), 0u);
  EXPECT_GT(server.GetVideoPacketsReceived(), 0u)
      << "No video RTP was sent; capture callbacks may be queued without a video encoder worker";
  EXPECT_GT(stats.current_fps, 1.0);
  EXPECT_LT(stats.current_fps, 90.0)
      << "FPS telemetry must count video frames, not 100 Hz audio frames";
  // Stop session
  auto t0 = std::chrono::steady_clock::now();
  engine.StopCasting();
  auto t1 = std::chrono::steady_clock::now();
  double stop_duration_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

  EXPECT_EQ(engine.GetState(), SessionState::kIdle);
  // Verify stop finishes within 500ms budget
  EXPECT_LE(stop_duration_ms, 500.0);

  engine.Shutdown();
  ConfigStore::Instance().Mutable() = saved_cfg;
  ConfigStore::Instance().Save();
  server.Stop();
}

TEST(CastE2ETest, ReconnectRestartsVideoPipelineWithoutCrash) {
  TestReceiverServer server;
  server.Start();

  auto& engine = CastEngine::Instance();
  engine.Initialize();
  AppConfig saved_cfg = ConfigStore::Instance().Get();

  CastDevice dev;
  dev.id = "test-e2e-reconnect-device";
  dev.name = "Reconnect Test TV";
  dev.model_name = "Chromecast Ultra";
  dev.ip_address = "127.0.0.1";
  dev.port = server.GetTlsPort();
  dev.capabilities = kCapVideoOut | kCapAudioOut;
  engine.GetDiscovery().AddOrUpdateDevice(dev);

  ASSERT_TRUE(engine.StartCasting(dev.id, 0, QualityPreset::kBalanced, true));
  ASSERT_EQ(engine.GetState(), SessionState::kStreaming);

  auto wait_until = [](const std::function<bool()>& predicate, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      if (predicate()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
  };

  ASSERT_TRUE(wait_until([&] { return server.GetVideoPacketsReceived() > 0; }, 2000));
  const uint32_t video_before_disconnect = server.GetVideoPacketsReceived();

  server.DisconnectClient();
  ASSERT_TRUE(wait_until([&] { return engine.GetState() == SessionState::kReconnecting; }, 3000));
  ASSERT_TRUE(wait_until([&] { return engine.GetState() == SessionState::kStreaming; }, 8000));
  EXPECT_TRUE(wait_until(
      [&] { return server.GetVideoPacketsReceived() > video_before_disconnect; }, 2000))
      << "Video RTP did not resume after reconnect";

  engine.StopCasting();
  EXPECT_EQ(engine.GetState(), SessionState::kIdle);
  engine.Shutdown();
  ConfigStore::Instance().Mutable() = saved_cfg;
  ConfigStore::Instance().Save();
  server.Stop();
}
