#include "castcore/device_discovery.h"
#include "castcore/logger.h"

#include <cstring>
#include <chrono>
#include <algorithm>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef int socklen_t;
  #define close closesocket
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <poll.h>
#endif

namespace castcore {

namespace {

constexpr const char* kCastServiceType = "_googlecast._tcp.local";
constexpr const char* kMdnsMulticastGroup = "224.0.0.251";
constexpr uint16_t kMdnsPort = 5353;

} // namespace

DeviceDiscovery::DeviceDiscovery() = default;

DeviceDiscovery::~DeviceDiscovery() {
  Stop();
}

bool DeviceDiscovery::Start() {
  if (running_.exchange(true)) return true;

  LOG_INFO << "Starting Cast device discovery...";
  discovery_thread_ = std::thread(&DeviceDiscovery::DiscoveryLoop, this);
  return true;
}

void DeviceDiscovery::Stop() {
  if (!running_.exchange(false)) return;

  LOG_INFO << "Stopping Cast device discovery...";
  if (discovery_thread_.joinable()) {
    discovery_thread_.join();
  }
}

bool DeviceDiscovery::IsRunning() const {
  return running_.load();
}

void DeviceDiscovery::TriggerScan() {
  // Can be called to force an immediate probe
}

std::vector<CastDevice> DeviceDiscovery::GetDevices() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return devices_;
}

std::optional<CastDevice> DeviceDiscovery::FindDeviceById(const std::string& id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& dev : devices_) {
    if (dev.id == id) return dev;
  }
  return std::nullopt;
}

std::optional<CastDevice> DeviceDiscovery::FindDeviceByIp(const std::string& ip) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& dev : devices_) {
    if (dev.ip_address == ip) return dev;
  }
  return std::nullopt;
}

void DeviceDiscovery::SetCallback(DevicesCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  callback_ = std::move(callback);
}

void DeviceDiscovery::AddOrUpdateDevice(const CastDevice& device) {
  DevicesCallback cb;
  std::vector<CastDevice> current;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(devices_.begin(), devices_.end(),
                           [&](const CastDevice& d) {
                             return (!device.id.empty() && d.id == device.id) ||
                                    (device.id.empty() && d.ip_address == device.ip_address);
                           });

    if (it != devices_.end()) {
      *it = device;
      it->last_seen = std::chrono::steady_clock::now();
    } else {
      devices_.push_back(device);
      devices_.back().last_seen = std::chrono::steady_clock::now();
      LOG_INFO << "Discovered Cast Device: " << device.name
               << " (" << device.model_name << ") at " << device.ip_address << ":" << device.port;
    }

    current = devices_;
    cb = callback_;
  }

  if (cb) {
    cb(current);
  }
}

void DeviceDiscovery::RemoveDevice(const std::string& device_id) {
  DevicesCallback cb;
  std::vector<CastDevice> current;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::remove_if(devices_.begin(), devices_.end(),
                             [&](const CastDevice& d) { return d.id == device_id; });
    if (it != devices_.end()) {
      devices_.erase(it, devices_.end());
      current = devices_;
      cb = callback_;
    }
  }

  if (cb) {
    cb(current);
  }
}

std::map<std::string, std::string> DeviceDiscovery::ParseTxtRecord(const std::vector<std::string>& txt_entries) {
  std::map<std::string, std::string> result;
  for (const auto& entry : txt_entries) {
    auto eq = entry.find('=');
    if (eq != std::string::npos) {
      std::string key = entry.substr(0, eq);
      std::string val = entry.substr(eq + 1);
      result[key] = val;
    } else {
      result[entry] = "";
    }
  }
  return result;
}

CastDevice DeviceDiscovery::ParseFromMdnsData(const std::string& name,
                                              const std::string& ip,
                                              uint16_t port,
                                              const std::vector<std::string>& txt_entries) {
  auto txt_map = ParseTxtRecord(txt_entries);
  CastDevice device;
  device.ip_address = ip;
  device.port = port > 0 ? port : 8009;

  device.id = txt_map.count("id") ? txt_map["id"] : ip;
  device.name = txt_map.count("fn") ? txt_map["fn"] : (name.empty() ? ("Chromecast-" + ip) : name);
  device.model_name = txt_map.count("md") ? txt_map["md"] : "Chromecast";

  if (txt_map.count("ca")) {
    try {
      device.capabilities = static_cast<uint32_t>(std::stoul(txt_map["ca"]));
    } catch (...) {
      device.capabilities = kCapVideoOut | kCapAudioOut;
    }
  } else {
    device.capabilities = kCapVideoOut | kCapAudioOut;
  }

  if (txt_map.count("st")) {
    device.status = (txt_map["st"] == "1") ? DeviceStatus::kBusy : DeviceStatus::kReady;
  } else {
    device.status = DeviceStatus::kReady;
  }

  device.last_seen = std::chrono::steady_clock::now();
  return device;
}

void DeviceDiscovery::SendMdnsQuery(int socket_fd) {
  // Construct standard DNS question for _googlecast._tcp.local PTR
  uint8_t query[512];
  size_t offset = 0;

  // Header: ID=0, Flags=0, QDCOUNT=1, ANCOUNT=0, NSCOUNT=0, ARCOUNT=0
  query[offset++] = 0x00; query[offset++] = 0x00; // ID
  query[offset++] = 0x00; query[offset++] = 0x00; // Standard query
  query[offset++] = 0x00; query[offset++] = 0x01; // QDCOUNT = 1
  query[offset++] = 0x00; query[offset++] = 0x00; // ANCOUNT = 0
  query[offset++] = 0x00; query[offset++] = 0x00; // NSCOUNT = 0
  query[offset++] = 0x00; query[offset++] = 0x00; // ARCOUNT = 0

  // QNAME: _googlecast._tcp.local
  const char* labels[] = {"_googlecast", "_tcp", "local"};
  for (const char* label : labels) {
    size_t len = std::strlen(label);
    query[offset++] = static_cast<uint8_t>(len);
    std::memcpy(&query[offset], label, len);
    offset += len;
  }
  query[offset++] = 0x00; // Root terminator

  // QTYPE: PTR (12)
  query[offset++] = 0x00; query[offset++] = 0x0C;
  // QCLASS: IN (1), Unicast-response bit = 0
  query[offset++] = 0x00; query[offset++] = 0x01;

  struct sockaddr_in dest_addr{};
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(kMdnsPort);
  inet_pton(AF_INET, kMdnsMulticastGroup, &dest_addr.sin_addr);

  sendto(socket_fd, reinterpret_cast<const char*>(query), offset, 0,
         reinterpret_cast<struct sockaddr*>(&dest_addr), sizeof(dest_addr));
}

void DeviceDiscovery::ProcessMdnsResponse(const uint8_t* buffer, size_t length, const std::string& sender_ip) {
  if (length < 12) return;

  // Very fast parser for mDNS TXT strings and service info
  // Search for known TXT keys: "id=", "fn=", "md=", "ca="
  std::vector<std::string> txt_entries;
  std::string full_str(reinterpret_cast<const char*>(buffer), length);

  size_t pos = 0;
  while (pos < length) {
    uint8_t len = buffer[pos++];
    if (len > 0 && pos + len <= length) {
      std::string piece(reinterpret_cast<const char*>(&buffer[pos]), len);
      if (piece.find("fn=") == 0 || piece.find("id=") == 0 || piece.find("md=") == 0 ||
          piece.find("ca=") == 0 || piece.find("st=") == 0 || piece.find("ve=") == 0) {
        txt_entries.push_back(piece);
      }
      pos += len;
    }
  }

  if (!txt_entries.empty()) {
    CastDevice dev = ParseFromMdnsData("", sender_ip, 8009, txt_entries);
    AddOrUpdateDevice(dev);
  }
}

void DeviceDiscovery::DiscoveryLoop() {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    LOG_ERROR << "Failed to create mDNS UDP socket";
    return;
  }

  int reuse = 1;
#if defined(SO_REUSEPORT)
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#endif
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

  struct sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_port = htons(kMdnsPort);
  bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(fd, reinterpret_cast<struct sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
    LOG_WARN << "Failed to bind mDNS port 5353 (will use ephemeral query socket)";
    close(fd);
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
  }

  // Join multicast group
  struct ip_mreq mreq{};
  inet_pton(AF_INET, kMdnsMulticastGroup, &mreq.imr_multiaddr);
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq), sizeof(mreq));

  // Set non-blocking / timeout
#if defined(_WIN32)
  DWORD timeout_ms = 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
#else
  struct timeval tv{};
  tv.tv_sec = 1;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

  auto last_query_time = std::chrono::steady_clock::now() - std::chrono::seconds(10);

  while (running_.load()) {
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_query_time).count() >= 4) {
      SendMdnsQuery(fd);
      last_query_time = now;
    }

    uint8_t buffer[4096];
    struct sockaddr_in src_addr{};
    socklen_t addr_len = sizeof(src_addr);

    int bytes_read = recvfrom(fd, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                              reinterpret_cast<struct sockaddr*>(&src_addr), &addr_len);

    if (bytes_read > 0) {
      char ip_str[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &src_addr.sin_addr, ip_str, sizeof(ip_str));
      ProcessMdnsResponse(buffer, static_cast<size_t>(bytes_read), std::string(ip_str));
    }

    // Check for offline devices (not seen for > 20 seconds)
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto& d : devices_) {
        if (std::chrono::duration_cast<std::chrono::seconds>(now - d.last_seen).count() > 20) {
          d.status = DeviceStatus::kOffline;
        }
      }
    }
  }

  close(fd);
}

} // namespace castcore
