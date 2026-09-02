#ifndef CASTCORE_CAST_CHANNEL_H_
#define CASTCORE_CAST_CHANNEL_H_

#include "castcore/types.h"
#include "cast_channel.pb.h"
#include <string>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <openssl/ssl.h>
#include <openssl/err.h>

namespace castcore {

class CastChannel {
 public:
  using MessageCallback = std::function<void(const std::string& ns,
                                             const std::string& payload,
                                             const std::string& src_id,
                                             const std::string& dest_id)>;
  using StatusCallback = std::function<void(bool is_connected, const std::string& error_msg)>;

  CastChannel();
  ~CastChannel();

  bool Connect(const std::string& ip_address, uint16_t port = 8009, int timeout_ms = 5000);
  void Disconnect();
  bool IsConnected() const;

  bool SendCastMessage(const std::string& namespace_,
                       const std::string& payload_utf8,
                       const std::string& destination_id = kPlatformReceiverId,
                       const std::string& source_id = kPlatformSenderId);

  bool ConnectVirtual(const std::string& destination_id, const std::string& source_id = kPlatformSenderId);
  bool DisconnectVirtual(const std::string& destination_id, const std::string& source_id = kPlatformSenderId);

  int LaunchApp(const std::string& app_id);
  int StopApp(const std::string& session_id);
  int RequestReceiverStatus();

  void SetMessageCallback(MessageCallback callback);
  void SetStatusCallback(StatusCallback callback);
  void SetAppTransportId(const std::string& transport_id);

  std::string GetConnectedIp() const { return ip_address_; }
  bool HeartbeatTimedOut() const;

  void SetVerifyDeviceCert(bool verify) { verify_device_cert_ = verify; }
  bool GetVerifyDeviceCert() const { return verify_device_cert_; }

 private:
  void NoteIncomingPong();
  void NotifyDisconnected(const std::string& reason);
  void ReceiveLoop();
  void HeartbeatLoop();
  bool SendRawPacket(const uint8_t* data, size_t length);

  std::string ip_address_;
  std::string app_transport_id_;
  uint16_t port_ = 8009;
  std::atomic<bool> is_connected_{false};
  std::atomic<bool> should_stop_{false};
  bool verify_device_cert_ = true;

  int socket_fd_ = -1;
  SSL_CTX* ssl_ctx_ = nullptr;
  SSL* ssl_ = nullptr;

  std::atomic<int> next_request_id_{1};
  std::mutex send_mutex_;
  std::mutex callback_mutex_;

  MessageCallback message_callback_;
  StatusCallback status_callback_;

  std::thread receive_thread_;
  std::thread heartbeat_thread_;
  std::atomic<int64_t> last_pong_ms_{0};
  std::atomic<bool> disconnect_notified_{false};
};

} // namespace castcore

#endif // CASTCORE_CAST_CHANNEL_H_
