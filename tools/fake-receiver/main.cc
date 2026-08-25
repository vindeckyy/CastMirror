#include "castcore/logger.h"
#include "castcore/types.h"
#include "cast_channel.pb.h"
#include <nlohmann/json.hpp>

#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <cstring>
#include <chrono>

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
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    LOG_INFO << "Fake Cast Receiver running on TLS :" << tls_port_ << " and UDP :" << udp_port_;
    return true;
  }

  void Stop() {
    if (!running_.exchange(false)) return;
    if (server_fd_ >= 0) {
      close(server_fd_);
      server_fd_ = -1;
    }
    if (udp_fd_ >= 0) {
      close(udp_fd_);
      udp_fd_ = -1;
    }
    if (tls_thread_.joinable()) tls_thread_.join();
    if (udp_thread_.joinable()) udp_thread_.join();
  }

 private:
  SSL_CTX* CreateServerContext() {
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();

    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) return nullptr;

    // Generate ephemeral self-signed certificate for test server
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
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
      LOG_ERROR << "Fake receiver failed to bind TLS port " << tls_port_;
      SSL_CTX_free(ctx);
      return;
    }

    listen(server_fd_, 5);
    is_ready_ = true;

    while (running_.load()) {
      struct sockaddr_in client_addr{};
      socklen_t clen = sizeof(client_addr);
      int client_fd = accept(server_fd_, reinterpret_cast<struct sockaddr*>(&client_addr), &clen);
      if (client_fd < 0) break;

      SSL* ssl = SSL_new(ctx);
      SSL_set_fd(ssl, client_fd);

      if (SSL_accept(ssl) <= 0) {
        SSL_free(ssl);
        close(client_fd);
        continue;
      }

      LOG_INFO << "[FakeReceiver] Client connected via TLS!";

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
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(udp_port_);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    bind(udp_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));

    uint8_t buf[4096];
    uint32_t packets_received = 0;
    struct sockaddr_in sender_addr{};
    socklen_t slen = sizeof(sender_addr);

    while (running_.load()) {
      ssize_t r = recvfrom(udp_fd_, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                           reinterpret_cast<struct sockaddr*>(&sender_addr), &slen);
      if (r > 0) {
        packets_received++;
        if (packets_received % 60 == 0) {
          // Send RTCP CAST Feedback packet back to sender
          uint8_t rtcp[32];
          // RTCP Receiver Report Header
          rtcp[0] = 0x81; // V=2, RC=1
          rtcp[1] = 201;  // RR
          rtcp[2] = 0x00; rtcp[3] = 0x07; // Length = 7 words (32 bytes)
          rtcp[4] = 0x00; rtcp[5] = 0x00; rtcp[6] = 0x27; rtcp[7] = 0x12; // SSRC = 10002

          // Report block
          rtcp[8] = 0x00; rtcp[9] = 0x00; rtcp[10] = 0x00; rtcp[11] = 0x02; // Sender SSRC = 2
          rtcp[12] = 0x00; // Fraction lost = 0
          rtcp[13] = 0x00; rtcp[14] = 0x00; rtcp[15] = 0x00; // Cumulative lost = 0
          rtcp[16] = 0x00; rtcp[17] = 0x00; rtcp[18] = 0x00; rtcp[19] = 0x00;
          rtcp[20] = 0x00; rtcp[21] = 0x00; rtcp[22] = 0x00; rtcp[23] = 0x00; // Jitter = 0
          rtcp[24] = 0x00; rtcp[25] = 0x00; rtcp[26] = 0x00; rtcp[27] = 0x00;
          rtcp[28] = 0x00; rtcp[29] = 0x00; rtcp[30] = 0x00; rtcp[31] = 0x00;

          sendto(udp_fd_, reinterpret_cast<const char*>(rtcp), sizeof(rtcp), 0,
                 reinterpret_cast<struct sockaddr*>(&sender_addr), slen);
        }
      }
    }
  }

  uint16_t tls_port_ = 8009;
  uint16_t udp_port_ = 33533;
  std::atomic<bool> running_{false};
  std::atomic<bool> is_ready_{false};
  int server_fd_ = -1;
  int udp_fd_ = -1;
  std::thread tls_thread_;
  std::thread udp_thread_;
};

int main(int argc, char** argv) {
  LOG_INFO << "===========================================";
  LOG_INFO << "   CastMirror: Standalone Fake Receiver    ";
  LOG_INFO << "===========================================";

  uint16_t tls_port = (argc > 1) ? static_cast<uint16_t>(std::stoi(argv[1])) : 8009;
  uint16_t udp_port = (argc > 2) ? static_cast<uint16_t>(std::stoi(argv[2])) : 33533;

  FakeCastReceiver receiver(tls_port, udp_port);
  receiver.Start();

  LOG_INFO << "Fake Receiver ready. Press Ctrl+C to terminate.";
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  return 0;
}
