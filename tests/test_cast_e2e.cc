#include <gtest/gtest.h>
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif
#include <cstdio>
#include <cstdlib>
#include "castcore/logger.h"
#include "castcore/cast_engine.h"
#include "castcore/cast_session.h"
#include "castcore/state_machine.h"
#include "castcore/adaptive_controller.h"
#include "cast_channel.pb.h"
#include <nlohmann/json.hpp>

#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <random>
#include <functional>
#include <algorithm>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#define close closesocket
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
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
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
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
    // Wake the accept() and recvfrom() calls with a throwaway connection and datagram, join the
    // threads, and only then close the sockets. Closing a descriptor another thread is still
    // blocked on is a data race.
    int dummy = socket(AF_INET, SOCK_STREAM, 0);
    if (dummy >= 0) {
      struct sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(tls_port_.load());
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      connect(dummy, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
      close(dummy);
    }

    int dummy_u = socket(AF_INET, SOCK_DGRAM, 0);
    if (dummy_u >= 0) {
      struct sockaddr_in uaddr{};
      uaddr.sin_family = AF_INET;
      uaddr.sin_port = htons(udp_port_.load());
      inet_pton(AF_INET, "127.0.0.1", &uaddr.sin_addr);
      sendto(dummy_u, "x", 1, 0, reinterpret_cast<struct sockaddr*>(&uaddr), sizeof(uaddr));
      close(dummy_u);
    }

    if (tls_thread_.joinable()) tls_thread_.join();
    if (udp_thread_.joinable()) udp_thread_.join();

    const int sfd = server_fd_.exchange(-1);
    if (sfd >= 0) {
      shutdown(sfd, SHUT_RDWR);
      close(sfd);
    }
    const int ufd = udp_fd_.exchange(-1);
    if (ufd >= 0) close(ufd);
  }

  uint16_t GetTlsPort() const { return tls_port_; }
  uint16_t GetUdpPort() const { return udp_port_; }
  std::string GetBoundIp() const { return bound_ip_; }
  uint32_t GetUdpPacketsReceived() const { return packets_received_.load(); }
  uint32_t GetVideoPacketsReceived() const { return video_packets_received_.load(); }
  // RTP timestamps (bytes 4-7) of the first and last audio frame seen, and the
  // largest backwards step between consecutive ones. A sender that stamps a
  // frame off the wall clock instead of the capture clock walks these out of
  // order, which is what desyncs audio from video on the receiver.
  uint32_t GetAudioRtpFirst() const { return audio_rtp_first_.load(); }
  uint32_t GetAudioRtpLast() const { return audio_rtp_last_.load(); }
  uint32_t GetAudioRtpBackwardsSteps() const { return audio_rtp_backwards_.load(); }
  uint32_t GetAudioFrames() const { return audio_frames_.load(); }
  uint32_t GetAudioPacketsReceived() const { return audio_packets_received_.load(); }
  uint32_t GetPacketsDropped() const { return packets_dropped_.load(); }
  uint32_t GetNonLoopbackPackets() const { return non_loopback_packets_.load(); }
  bool GetAllPacketsLoopback() const { return non_loopback_packets_.load() == 0; }

  void DisconnectClient() {
    int fd = current_client_fd_.load();
    if (fd >= 0) {
      shutdown(fd, SHUT_RDWR);
    }
  }

  // Synthetic Fault Injector Interface
  void SetSimulatedLossRate(double loss_fraction) {
    simulated_loss_rate_.store(std::clamp(loss_fraction, 0.0, 1.0));
  }

  // Answer the next OFFERs with {"result":"error"} to exercise fast-fail.
  void SetRejectOffer(bool reject) { reject_offer_.store(reject); }

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
    rtcp[0] = 0x8F;  // V=2, FMT=15 (CAST)
    rtcp[1] = 206;  // PT=206
    uint16_t words = static_cast<uint16_t>((pkt_len / 4) - 1);
    rtcp[2] = static_cast<uint8_t>((words >> 8) & 0xFF);
    rtcp[3] = static_cast<uint8_t>(words & 0xFF);

    // Receiver SSRC (10002), Sender SSRC (2)
    rtcp[4] = 0x00;
    rtcp[5] = 0x00;
    rtcp[6] = 0x27;
    rtcp[7] = 0x12;
    rtcp[8] = 0x00;
    rtcp[9] = 0x00;
    rtcp[10] = 0x00;
    rtcp[11] = 0x02;

    // 'CAST'
    rtcp[12] = 'C';
    rtcp[13] = 'A';
    rtcp[14] = 'S';
    rtcp[15] = 'T';
    rtcp[16] = static_cast<uint8_t>(frame_id > 0 ? (frame_id - 1) & 0xFF : 0);
    rtcp[17] = static_cast<uint8_t>(count);
    rtcp[18] = 0x01;
    rtcp[19] = 0x90;  // 400ms delay

    for (size_t i = 0; i < count; ++i) {
      size_t off = 20 + i * 4;
      rtcp[off] = static_cast<uint8_t>(frame_id & 0xFF);
      rtcp[off + 1] = static_cast<uint8_t>((packets[i] >> 8) & 0xFF);
      rtcp[off + 2] = static_cast<uint8_t>(packets[i] & 0xFF);
      rtcp[off + 3] = 0x00;
    }

    std::lock_guard<std::mutex> lock(sender_mutex_);
    sendto(udp_fd_,
           reinterpret_cast<const char*>(rtcp.data()),
           rtcp.size(),
           0,
           reinterpret_cast<const struct sockaddr*>(&sender_addr_),
           sizeof(sender_addr_));
  }

  void TriggerPictureLossIndicator() {
    if (!has_sender_.load() || udp_fd_ < 0) return;
    ApplySimulatedJitter();

    uint8_t pli[12] = {
        0x81,
        206,
        0x00,
        0x02,
        0x00,
        0x00,
        0x27,
        0x12,  // Receiver SSRC 10002
        0x00,
        0x00,
        0x00,
        0x02  // Sender SSRC 2
    };
    std::lock_guard<std::mutex> lock(sender_mutex_);
    sendto(udp_fd_,
           reinterpret_cast<const char*>(pli),
           sizeof(pli),
           0,
           reinterpret_cast<const struct sockaddr*>(&sender_addr_),
           sizeof(sender_addr_));
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
    rtcp[0] = 0x81;  // V=2, RC=1
    rtcp[1] = 201;  // RR
    rtcp[2] = 0x00;
    rtcp[3] = 0x07;  // Length = 7 words (32 bytes)
    rtcp[4] = 0x00;
    rtcp[5] = 0x00;
    rtcp[6] = 0x27;
    rtcp[7] = 0x12;  // Receiver SSRC = 10002

    // Report block
    rtcp[8] = 0x00;
    rtcp[9] = 0x00;
    rtcp[10] = 0x00;
    rtcp[11] = 0x02;  // Sender SSRC = 2
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
    sendto(udp_fd_,
           reinterpret_cast<const char*>(rtcp),
           sizeof(rtcp),
           0,
           reinterpret_cast<const struct sockaddr*>(&sender_addr_),
           sizeof(sender_addr_));
  }

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
        LOG_INFO << "[TestReceiver] SSL_read closed: r=" << r << ", err=" << err << " (" << err_buf
                 << ")";
        return false;
      }
      read_bytes += r;
    }
    return true;
  }

  void SendMsg(SSL* ssl, const std::string& ns, const std::string& payload, const std::string& src,
               const std::string& dest) {
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
    setsockopt(
        server_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#if defined(SO_REUSEPORT)
    setsockopt(
        server_fd_, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&opt), sizeof(opt));
#endif

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;  // Dynamic ephemeral port
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (::bind(server_fd_.load(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
      LOG_ERROR << "[TestReceiver] Failed to bind dynamic TLS port: " << strerror(errno);
      SSL_CTX_free(ctx);
      running_ = false;
      return;
    }
    socklen_t slen = sizeof(addr);
    getsockname(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), &slen);
    tls_port_ = ntohs(addr.sin_port);

    listen(server_fd_, 5);
    LOG_INFO << "[TestReceiver] Listening for Cast TLS on 127.0.0.1:" << tls_port_;
    is_ready_ = true;

    while (running_.load()) {
      struct sockaddr_in caddr{};
      socklen_t clen = sizeof(caddr);
      int cfd = accept(server_fd_, reinterpret_cast<struct sockaddr*>(&caddr), &clen);
      if (cfd < 0) break;
      current_client_fd_.store(cfd);

      char client_ip[INET_ADDRSTRLEN]{};
      inet_ntop(AF_INET, &caddr.sin_addr, client_ip, sizeof(client_ip));
      if (std::string(client_ip) != "127.0.0.1") {
        non_loopback_packets_++;
      }

      SSL* ssl = SSL_new(ctx);
      SSL_set_fd(ssl, cfd);

      if (SSL_accept(ssl) > 0) {
        LOG_INFO << "[TestReceiver] Client SSL_accept succeeded from " << client_ip << "!";
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
              SendMsg(ssl,
                      kNamespaceHeartbeat,
                      "{\"type\":\"PONG\"}",
                      in_msg.destination_id(),
                      in_msg.source_id());
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
                  if (reject_offer_.load()) {
                    ans["result"] = "error";
                  } else {
                    ans["result"] = "ok";
                    ans["answer"]["castMode"] = "mirroring";
                    ans["answer"]["udpPort"] = udp_port_.load();
                    ans["answer"]["sendIndexes"] = nlohmann::json::array({0, 1});
                    ans["answer"]["ssrcs"] = nlohmann::json::array({10001, 10002});
                  }
                  SendMsg(ssl,
                          kNamespaceWebrtc,
                          ans.dump(),
                          in_msg.destination_id(),
                          in_msg.source_id());
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
    addr.sin_port = 0;  // Dynamic ephemeral UDP port
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (::bind(udp_fd_.load(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
      LOG_ERROR << "[TestReceiver] Failed to bind dynamic UDP port: " << strerror(errno);
      running_ = false;
      return;
    }
    socklen_t ulen = sizeof(addr);
    getsockname(udp_fd_, reinterpret_cast<struct sockaddr*>(&addr), &ulen);
    udp_port_ = ntohs(addr.sin_port);

    uint8_t buf[4096];
    struct sockaddr_in saddr{};
    socklen_t slen = sizeof(saddr);
    uint32_t loop_count = 0;
    std::mt19937 rng(1337);
    std::uniform_real_distribution<double> dist(0.0, 1.0);

    while (running_.load()) {
      ssize_t r = recvfrom(udp_fd_,
                           reinterpret_cast<char*>(buf),
                           sizeof(buf),
                           0,
                           reinterpret_cast<struct sockaddr*>(&saddr),
                           &slen);
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
        // Video is PT 96, audio is PT 127. The marker bit marks the last packet
        // of a frame, so for audio it identifies the start of the next frame.
        bool is_rtp = (r >= 12 && (buf[1] & 0x7F) == 96);
        bool is_audio = (r >= 12 && (buf[1] & 0x7F) == 127);
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
        } else if (is_audio && r >= 12) {
          audio_packets_received_++;
          // Only judge the timestamp once per frame, on the marker packet that
          // starts the next one, so a multi-packet frame is not counted twice.
          if ((buf[1] & 0x80) != 0) {
            audio_frames_++;
            const uint32_t rtp = (static_cast<uint32_t>(buf[4]) << 24) |
                                 (static_cast<uint32_t>(buf[5]) << 16) |
                                 (static_cast<uint32_t>(buf[6]) << 8) | buf[7];
            if (!audio_rtp_seen_.load()) {
              audio_rtp_first_.store(rtp);
              audio_rtp_last_.store(rtp);
              audio_rtp_seen_.store(true);
            } else {
              const uint32_t prev = audio_rtp_last_.load();
              if (rtp < prev) audio_rtp_backwards_++;
              audio_rtp_last_.store(rtp);
            }
          }
        }

        loop_count++;
        if (loop_count % 30 == 0) {
          SendReceiverReport();
        }
      }
    }
  }

  std::atomic<uint16_t> tls_port_{0};
  std::atomic<uint16_t> udp_port_{0};
  std::string bound_ip_ = "127.0.0.1";
  std::atomic<bool> reject_offer_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> is_ready_{false};
  std::atomic<uint32_t> packets_received_{0};
  std::atomic<uint32_t> video_packets_received_{0};
  std::atomic<uint32_t> audio_packets_received_{0};
  std::atomic<uint32_t> audio_frames_{0};
  std::atomic<uint32_t> audio_rtp_first_{0};
  std::atomic<uint32_t> audio_rtp_last_{0};
  std::atomic<uint32_t> audio_rtp_backwards_{0};
  std::atomic<bool> audio_rtp_seen_{false};

  std::atomic<uint32_t> packets_dropped_{0};
  std::atomic<uint32_t> non_loopback_packets_{0};
  std::atomic<int> current_client_fd_{-1};

  std::atomic<double> simulated_loss_rate_{0.0};
  std::atomic<int> jitter_min_ms_{0};
  std::atomic<int> jitter_max_ms_{0};

  std::mutex sender_mutex_;
  struct sockaddr_in sender_addr_{};
  std::atomic<bool> has_sender_{false};

  std::atomic<int> server_fd_{-1};
  std::atomic<int> udp_fd_{-1};
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
  ConfigStore::Instance().Mutable().verify_device_cert = false;

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
  ConfigStore::Instance().Mutable().verify_device_cert = false;

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
  EXPECT_TRUE(
      wait_until([&] { return server.GetVideoPacketsReceived() > video_before_disconnect; }, 2000))
      << "Video RTP did not resume after reconnect";

  engine.StopCasting();
  EXPECT_EQ(engine.GetState(), SessionState::kIdle);
  engine.Shutdown();
  ConfigStore::Instance().Mutable() = saved_cfg;
  ConfigStore::Instance().Save();
  server.Stop();
}

TEST(CastE2ETest, PacedStreamingUnderSyntheticLoss) {
  TestReceiverServer server;
  server.SetSimulatedLossRate(0.05);  // 5% synthetic packet loss
  server.Start();

  auto& engine = CastEngine::Instance();
  engine.Initialize();
  AppConfig saved_cfg = ConfigStore::Instance().Get();
  ConfigStore::Instance().Mutable().verify_device_cert = false;

  CastDevice dev;
  dev.id = "test-e2e-loss-device";
  dev.name = "Loss Test TV";
  dev.model_name = "Chromecast Ultra";
  dev.ip_address = "127.0.0.1";
  dev.port = server.GetTlsPort();
  dev.capabilities = kCapVideoOut | kCapAudioOut;

  engine.GetDiscovery().AddOrUpdateDevice(dev);

  bool ok = engine.StartCasting(dev.id, 0, QualityPreset::kBalanced, true);
  ASSERT_TRUE(ok);
  ASSERT_EQ(engine.GetState(), SessionState::kStreaming);

  // Stream for 2 seconds under 5% loss
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));

  StreamStats stats = engine.GetStats();
  EXPECT_GT(stats.frames_sent, 0u);
  EXPECT_GT(stats.packets_sent, 0u);
  EXPECT_GT(server.GetVideoPacketsReceived(), 0u);
  EXPECT_GT(server.GetPacketsDropped(), 0u) << "Simulated loss should drop some packets";

  // Verify pipeline continues producing frames without stalls
  EXPECT_GT(stats.current_fps, 1.0);
  EXPECT_EQ(engine.GetState(), SessionState::kStreaming);

  auto t0 = std::chrono::steady_clock::now();
  engine.StopCasting();
  auto t1 = std::chrono::steady_clock::now();
  double stop_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  EXPECT_EQ(engine.GetState(), SessionState::kIdle);
  EXPECT_LE(stop_ms, 500.0);

  engine.Shutdown();
  ConfigStore::Instance().Mutable() = saved_cfg;
  ConfigStore::Instance().Save();
  server.Stop();
}

TEST(CastE2ETest, DynamicDelayAdaptationEndToEnd) {
  TestReceiverServer server;
  server.Start();

  auto& engine = CastEngine::Instance();
  engine.Initialize();
  AppConfig saved_cfg = ConfigStore::Instance().Get();
  ConfigStore::Instance().Mutable().verify_device_cert = false;

  CastDevice dev;
  dev.id = "test-e2e-delay-device";
  dev.name = "Delay Test TV";
  dev.model_name = "Chromecast with Google TV";
  dev.ip_address = "127.0.0.1";
  dev.port = server.GetTlsPort();
  dev.capabilities = kCapVideoOut | kCapAudioOut;

  engine.GetDiscovery().AddOrUpdateDevice(dev);

  bool ok = engine.StartCasting(dev.id, 0, QualityPreset::kAuto, true);
  ASSERT_TRUE(ok);
  ASSERT_EQ(engine.GetState(), SessionState::kStreaming);

  // Allow initial streaming
  std::this_thread::sleep_for(std::chrono::milliseconds(600));

  StreamStats initial_stats = engine.GetStats();
  int initial_delay = initial_stats.target_delay_ms;
  EXPECT_GE(initial_delay, AdaptiveController::kMinPlayoutDelayMs);
  EXPECT_LE(initial_delay, AdaptiveController::kMaxPlayoutDelayMs);

  // Inject network jitter (>30ms) and packet loss (>3%) + NACK bursts to trigger dynamic delay scaling
  server.SetSimulatedJitter(35, 55);
  server.SetSimulatedLossRate(0.06);
  server.TriggerNackBurst(5, {1, 2, 3, 4, 5, 6, 7});

  // Wait for adaptation intervals to process feedback and scale playout delay
  std::this_thread::sleep_for(std::chrono::milliseconds(2200));

  StreamStats degraded_stats = engine.GetStats();
  EXPECT_GE(degraded_stats.target_delay_ms, initial_delay);
  EXPECT_LE(degraded_stats.target_delay_ms, AdaptiveController::kMaxPlayoutDelayMs);
  EXPECT_GE(degraded_stats.target_delay_ms, AdaptiveController::kMinPlayoutDelayMs);

  // Clear simulated faults
  server.SetSimulatedJitter(0, 0);
  server.SetSimulatedLossRate(0.0);

  engine.StopCasting();
  EXPECT_EQ(engine.GetState(), SessionState::kIdle);

  engine.Shutdown();
  ConfigStore::Instance().Mutable() = saved_cfg;
  ConfigStore::Instance().Save();
  server.Stop();
}

TEST(CastE2ETest, ZeroExternalNetworkAudit) {
  TestReceiverServer server;
  server.Start();

  // Audit receiver bound addresses
  EXPECT_EQ(server.GetBoundIp(), "127.0.0.1");
  EXPECT_GT(server.GetTlsPort(), 0);
  EXPECT_GT(server.GetUdpPort(), 0);

  auto& engine = CastEngine::Instance();
  engine.Initialize();
  AppConfig saved_cfg = ConfigStore::Instance().Get();
  ConfigStore::Instance().Mutable().verify_device_cert = false;

  CastDevice dev;
  dev.id = "test-e2e-loopback-device";
  dev.name = "Test Receiver";
  dev.model_name = "Chromecast Ultra";
  dev.ip_address = "127.0.0.1";
  dev.port = server.GetTlsPort();
  dev.capabilities = kCapVideoOut | kCapAudioOut;

  engine.GetDiscovery().AddOrUpdateDevice(dev);

  bool ok = engine.StartCasting(dev.id, 0, QualityPreset::kBalanced, true);
  ASSERT_TRUE(ok);
  ASSERT_EQ(engine.GetState(), SessionState::kStreaming);

  std::this_thread::sleep_for(std::chrono::milliseconds(1200));

  StreamStats stats = engine.GetStats();
  EXPECT_EQ(stats.device_ip, "127.0.0.1");
  EXPECT_GT(server.GetUdpPacketsReceived(), 0u);
  EXPECT_GT(server.GetVideoPacketsReceived(), 0u);

  // Strict audit: verify 100% of received packets and connections originated strictly from 127.0.0.1
  EXPECT_EQ(server.GetNonLoopbackPackets(), 0u)
      << "Detected traffic from external or non-loopback network interfaces during test!";
  EXPECT_TRUE(server.GetAllPacketsLoopback());

  engine.StopCasting();
  EXPECT_EQ(engine.GetState(), SessionState::kIdle);

  engine.Shutdown();
  ConfigStore::Instance().Mutable() = saved_cfg;
  ConfigStore::Instance().Save();
  server.Stop();
}

TEST(CastE2ETest, UnverifiedDeviceRejectedWhenVerificationEnforced) {
  TestReceiverServer server;
  server.Start();

  auto& engine = CastEngine::Instance();
  engine.Initialize();
  AppConfig saved_cfg = ConfigStore::Instance().Get();
  // Ensure certificate verification against Cast Root CA is strictly enforced.
  // Keep the auth timeout short: the fake receiver never answers the
  // device-auth challenge, so this test fails by timeout.
  ConfigStore::Instance().Mutable().verify_device_cert = true;
  ConfigStore::Instance().Mutable().answer_timeout_s = 1;

  CastDevice dev;
  dev.id = "test-e2e-untrusted-device";
  dev.name = "Untrusted Receiver";
  dev.model_name = "Fake Receiver";
  dev.ip_address = "127.0.0.1";
  dev.port = server.GetTlsPort();
  dev.capabilities = kCapVideoOut | kCapAudioOut;

  engine.GetDiscovery().AddOrUpdateDevice(dev);

  // The connection MUST be rejected due to unverified self-signed TLS cert
  bool ok = engine.StartCasting(dev.id, 0, QualityPreset::kBalanced, true);
  EXPECT_FALSE(ok);
  EXPECT_NE(engine.GetState(), SessionState::kStreaming);

  engine.Shutdown();
  ConfigStore::Instance().Mutable() = saved_cfg;
  ConfigStore::Instance().Save();
  server.Stop();
}

TEST(CastE2ETest, FrameQueueBackpressureOverrunsTrackedWithoutCrash) {
  TestReceiverServer server;
  server.Start();

  auto& engine = CastEngine::Instance();
  engine.Initialize();
  AppConfig saved_cfg = ConfigStore::Instance().Get();
  ConfigStore::Instance().Mutable().verify_device_cert = false;

  CastDevice dev;
  dev.id = "test-e2e-overrun-device";
  dev.name = "Overrun Test TV";
  dev.model_name = "Chromecast Ultra";
  dev.ip_address = "127.0.0.1";
  dev.port = server.GetTlsPort();
  dev.capabilities = kCapVideoOut | kCapAudioOut;

  engine.GetDiscovery().AddOrUpdateDevice(dev);

  bool ok = engine.StartCasting(dev.id, 0, QualityPreset::kBalanced, true);
  ASSERT_TRUE(ok);
  ASSERT_EQ(engine.GetState(), SessionState::kStreaming);

  // Allow session to stream and capture initial stats
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));

  StreamStats stats = engine.GetStats();
  EXPECT_EQ(engine.GetState(), SessionState::kStreaming);
  // Drops counter is accessible and tracked in stats
  EXPECT_GE(stats.video_queue_overruns, 0u);
  EXPECT_GE(stats.video_frames_dropped_capture, 0u);

  engine.StopCasting();
  EXPECT_EQ(engine.GetState(), SessionState::kIdle);

  engine.Shutdown();
  ConfigStore::Instance().Mutable() = saved_cfg;
  ConfigStore::Instance().Save();
  server.Stop();
}

TEST(CastE2ETest, MutedSessionKeepsSendingSilenceWithoutStackOverflow) {
  TestReceiverServer server;
  server.Start();

  auto& engine = CastEngine::Instance();
  engine.Initialize();
  AppConfig saved_cfg = ConfigStore::Instance().Get();
  ConfigStore::Instance().Mutable().verify_device_cert = false;

  CastDevice dev;
  dev.id = "test-e2e-mute-device";
  dev.name = "Mute Test TV";
  dev.model_name = "Chromecast Ultra";
  dev.ip_address = "127.0.0.1";
  dev.port = server.GetTlsPort();
  dev.capabilities = kCapVideoOut | kCapAudioOut;
  engine.GetDiscovery().AddOrUpdateDevice(dev);

  ASSERT_TRUE(engine.StartCasting(dev.id, 0, QualityPreset::kBalanced, true));
  ASSERT_EQ(engine.GetState(), SessionState::kStreaming);

  // Let the pipeline settle so the audio keepalive baseline is established.
  std::this_thread::sleep_for(std::chrono::milliseconds(700));

  engine.SetLiveAudioMuted(true);
  ASSERT_TRUE(engine.IsLiveAudioMuted());
  const StreamStats before = engine.GetStats();

  // Audio capture delivers ~100 frames/s, and the adaptation loop injects a
  // silence keepalive every 200ms once audio is enabled. 2.5s therefore pumps
  // far more than 20 frames through ProcessAudioFrame while muted. Before the
  // fix this recursed ProcessAudioFrame <-> InjectSilenceAudioFrame until the
  // stack overflowed (guaranteed crash).
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));

  EXPECT_EQ(engine.GetState(), SessionState::kStreaming)
      << "Muting must not crash or tear the session down";

  const StreamStats after = engine.GetStats();
  EXPECT_GT(after.packets_sent, before.packets_sent)
      << "Silence frames must still be encoded and sent while muted";
#if defined(_WIN32)
  // frames_sent counts video frames. The Windows capture repeats the last frame at a steady
  // rate; X11 under a headless Xvfb sends nothing while the screen is static.
  EXPECT_GT(after.frames_sent, before.frames_sent);
#endif

  engine.SetLiveAudioMuted(false);
  engine.StopCasting();
  EXPECT_EQ(engine.GetState(), SessionState::kIdle);

  engine.Shutdown();
  ConfigStore::Instance().Mutable() = saved_cfg;
  ConfigStore::Instance().Save();
  server.Stop();
}

TEST(CastE2ETest, SilenceKeepalivesKeepTheAudioTimelineMovingForward) {
  TestReceiverServer server;
  server.Start();

  auto& engine = CastEngine::Instance();
  engine.Initialize();
  AppConfig saved_cfg = ConfigStore::Instance().Get();
  ConfigStore::Instance().Mutable().verify_device_cert = false;

  CastDevice dev;
  dev.id = "test-e2e-audio-timeline-device";
  dev.name = "Audio Timeline TV";
  dev.model_name = "Chromecast Ultra";
  dev.ip_address = "127.0.0.1";
  dev.port = server.GetTlsPort();
  dev.capabilities = kCapVideoOut | kCapAudioOut;
  engine.GetDiscovery().AddOrUpdateDevice(dev);

  ASSERT_TRUE(engine.StartCasting(dev.id, 0, QualityPreset::kBalanced, true));
  ASSERT_EQ(engine.GetState(), SessionState::kStreaming);

  // Mute makes every audio frame a synthetic one, the strongest exercise of the
  // path: InjectSilenceAudioFrame has to continue the capture timeline rather
  // than read the wall clock. Real capture runs ~10-20 ms ahead of "now"
  // (WASAPI renders ahead of the capture instant), so a keepalive stamped with
  // "now" steps the audio clock backwards against the last real frame, and the
  // receiver's audio falls behind video for good.
  engine.SetLiveAudioMuted(true);
  ASSERT_TRUE(engine.IsLiveAudioMuted());
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));

  engine.SetLiveAudioMuted(false);
  std::this_thread::sleep_for(std::chrono::milliseconds(700));

  ASSERT_GT(server.GetAudioFrames(), 10u) << "not enough audio frames to judge the timeline";
  EXPECT_EQ(server.GetAudioRtpBackwardsSteps(), 0u)
      << "audio RTP timestamps must advance monotonically: a backwards step is what "
         "desyncs audio from video on the receiver";
  engine.StopCasting();
  engine.Shutdown();
  ConfigStore::Instance().Mutable() = saved_cfg;
  ConfigStore::Instance().Save();
  server.Stop();
}

TEST(CastE2ETest, FrozenStreamSurvivesStallDetectorWindow) {
  TestReceiverServer server;
  server.Start();

  auto& engine = CastEngine::Instance();
  engine.Initialize();
  AppConfig saved_cfg = ConfigStore::Instance().Get();
  ConfigStore::Instance().Mutable().verify_device_cert = false;

  CastDevice dev;
  dev.id = "test-e2e-freeze-device";
  dev.name = "Freeze Test TV";
  dev.model_name = "Chromecast Ultra";
  dev.ip_address = "127.0.0.1";
  dev.port = server.GetTlsPort();
  dev.capabilities = kCapVideoOut | kCapAudioOut;
  engine.GetDiscovery().AddOrUpdateDevice(dev);

  ASSERT_TRUE(engine.StartCasting(dev.id, 0, QualityPreset::kBalanced, true));
  ASSERT_EQ(engine.GetState(), SessionState::kStreaming);

  // Let video actually flow so the stall detector has a live baseline.
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  ASSERT_GT(server.GetVideoPacketsReceived(), 0u);

  engine.SetFreezeStream(true);
  ASSERT_TRUE(engine.IsStreamFrozen());

  // The adaptation loop's stall detector fires after 5s of no video sends.
  // Freezing for 6.5s of real wall time proves it no longer forces a reconnect
  // and that the user-visible frozen state is not clobbered.
  std::this_thread::sleep_for(std::chrono::milliseconds(6500));

  EXPECT_EQ(engine.GetState(), SessionState::kStreaming)
      << "Freezing the picture must not force a session reconnect";
  EXPECT_TRUE(engine.IsStreamFrozen());

  engine.SetFreezeStream(false);
  engine.StopCasting();
  EXPECT_EQ(engine.GetState(), SessionState::kIdle);

  engine.Shutdown();
  ConfigStore::Instance().Mutable() = saved_cfg;
  ConfigStore::Instance().Save();
  server.Stop();
}

// Regression: state callbacks fire from inside Start()/Stop(). Reading stats or
// state from them used to self-deadlock on the session/engine mutexes, and a
// StopCasting() racing a still-connecting Start() could free the session under it.
TEST(CastE2ETest, StateCallbackMayReadStatsAndStopMayCancelStart) {
  TestReceiverServer server;
  server.Start();

  auto& engine = CastEngine::Instance();
  engine.Initialize();
  AppConfig saved_cfg = ConfigStore::Instance().Get();
  ConfigStore::Instance().Mutable().verify_device_cert = false;

  CastDevice dev;
  dev.id = "test-e2e-reentrant-device";
  dev.name = "Reentrancy Test TV";
  dev.model_name = "Chromecast Ultra";
  dev.ip_address = "127.0.0.1";
  dev.port = server.GetTlsPort();
  dev.capabilities = kCapVideoOut | kCapAudioOut;
  engine.GetDiscovery().AddOrUpdateDevice(dev);

  std::atomic<int> callbacks{0};
  engine.SetOnStateChanged([&](SessionState, SessionState, const std::string&) {
    (void)engine.GetStats();
    (void)engine.GetState();
    ++callbacks;
  });

  ASSERT_TRUE(engine.StartCasting(dev.id, 0, QualityPreset::kBalanced, true));
  EXPECT_GT(callbacks.load(), 0);
  engine.StopCasting();
  EXPECT_EQ(engine.GetState(), SessionState::kIdle);

  // Cancel a Start() that is still connecting.
  std::atomic<bool> start_returned{false};
  std::thread starter([&] {
    (void)engine.StartCasting(dev.id, 0, QualityPreset::kBalanced, true);
    start_returned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  engine.StopCasting();
  starter.join();
  EXPECT_TRUE(start_returned.load());
  engine.StopCasting();

  engine.SetOnStateChanged(nullptr);
  engine.Shutdown();
  ConfigStore::Instance().Mutable() = saved_cfg;
  ConfigStore::Instance().Save();
  server.Stop();
}
// A receiver that answers {"result":"error"} must fail the start immediately,
// not after the full answer timeout.
TEST(CastE2ETest, RejectedOfferFailsFast) {
  TestReceiverServer server;
  server.Start();
  server.SetRejectOffer(true);

  auto& engine = CastEngine::Instance();
  engine.Initialize();
  AppConfig saved_cfg = ConfigStore::Instance().Get();
  ConfigStore::Instance().Mutable().verify_device_cert = false;
  ConfigStore::Instance().Mutable().answer_timeout_s = 10;

  CastDevice dev;
  dev.id = "test-e2e-reject-device";
  dev.name = "Reject Test TV";
  dev.model_name = "Chromecast Ultra";
  dev.ip_address = "127.0.0.1";
  dev.port = server.GetTlsPort();
  dev.capabilities = kCapVideoOut | kCapAudioOut;
  engine.GetDiscovery().AddOrUpdateDevice(dev);

  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_FALSE(engine.StartCasting(dev.id, 0, QualityPreset::kBalanced, true));
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 5)
      << "an error ANSWER must not wait out answer_timeout_s";

  engine.StopCasting();
  engine.Shutdown();
  ConfigStore::Instance().Mutable() = saved_cfg;
  ConfigStore::Instance().Save();
  server.Stop();
}
#if defined(_WIN32)
// Long-running stream against the simulated receiver, watching for leaks. Off unless
// CASTMIRROR_SOAK_MINUTES is set, because it takes that long. scripts/soak_windows.ps1
// runs it and prints the samples. Memory and handle counts must level off: after a
// warm-up, growth beyond a small margin means something is not being released.
TEST(CastE2ETest, SoakStreamDoesNotLeakMemoryOrHandles) {
  const char* minutes_env = std::getenv("CASTMIRROR_SOAK_MINUTES");
  if (!minutes_env) GTEST_SKIP() << "set CASTMIRROR_SOAK_MINUTES to run the soak test";
  const int minutes = std::max(1, std::atoi(minutes_env));

  TestReceiverServer server;
  server.Start();
  auto& engine = CastEngine::Instance();
  engine.Initialize();
  AppConfig saved_cfg = ConfigStore::Instance().Get();
  ConfigStore::Instance().Mutable().verify_device_cert = false;

  CastDevice dev;
  dev.id = "test-e2e-soak-device";
  dev.name = "Soak Test TV";
  dev.model_name = "Chromecast Ultra";
  dev.ip_address = "127.0.0.1";
  dev.port = server.GetTlsPort();
  dev.capabilities = kCapVideoOut | kCapAudioOut;
  engine.GetDiscovery().AddOrUpdateDevice(dev);

  ASSERT_TRUE(engine.StartCasting(dev.id, 0, QualityPreset::kBalanced, true));

  auto sample = [] {
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    return std::pair<size_t, DWORD>{pmc.WorkingSetSize, handles};
  };

  const auto start = std::chrono::steady_clock::now();
  const auto end = start + std::chrono::minutes(minutes);
  const auto warmup_end = start + std::chrono::minutes(std::min(2, minutes));
  size_t baseline_ws = 0;
  DWORD baseline_handles = 0;
  size_t peak_ws = 0;
  DWORD peak_handles = 0;
  int samples = 0;
  while (std::chrono::steady_clock::now() < end) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    ASSERT_EQ(engine.GetState(), SessionState::kStreaming)
        << "the stream must stay up for the whole soak";
    auto [ws, handles] = sample();
    if (std::chrono::steady_clock::now() < warmup_end) {
      baseline_ws = ws;
      baseline_handles = handles;
    }
    peak_ws = std::max(peak_ws, ws);
    peak_handles = std::max(peak_handles, handles);
    if (++samples % 6 == 0) {
      std::fprintf(stderr,
                   "[soak] %3d s  working set %6.1f MB  handles %u  frames %llu\n",
                   static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                                        std::chrono::steady_clock::now() - start)
                                        .count()),
                   ws / 1048576.0,
                   static_cast<unsigned>(handles),
                   static_cast<unsigned long long>(engine.GetStats().frames_sent));
    }
  }

  const StreamStats stats = engine.GetStats();
  engine.StopCasting();
  EXPECT_GT(stats.frames_sent, static_cast<uint64_t>(minutes) * 60 * 10)
      << "frames must keep flowing";
  // After the warm-up the working set may wander, but it must not climb: allow 64 MB
  // (allocator slack, stats windows) and 40 extra handles (sockets in flight).
  EXPECT_LT(peak_ws, baseline_ws + 64u * 1048576u) << "working set kept growing";
  EXPECT_LT(peak_handles, baseline_handles + 40u) << "handle count kept growing";
  std::fprintf(stderr,
               "[soak] done: baseline %.1f MB / %u handles, peak %.1f MB / %u handles\n",
               baseline_ws / 1048576.0,
               static_cast<unsigned>(baseline_handles),
               peak_ws / 1048576.0,
               static_cast<unsigned>(peak_handles));

  engine.Shutdown();
  ConfigStore::Instance().Mutable() = saved_cfg;
  ConfigStore::Instance().Save();
  server.Stop();
}
#endif
