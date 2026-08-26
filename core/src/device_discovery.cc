#include "castcore/device_discovery.h"
#include "castcore/logger.h"

#include <nlohmann/json.hpp>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <vector>
#include <thread>
#include <future>
#include <sstream>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <iphlpapi.h>
  #pragma comment(lib, "iphlpapi.lib")
  typedef int socklen_t;
  #define close closesocket
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <ifaddrs.h>
  #include <net/if.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <poll.h>
#endif

namespace castcore {

namespace {

constexpr const char* kCastServiceType = "_googlecast._tcp.local";
constexpr const char* kMdnsMulticastGroup = "224.0.0.251";
constexpr uint16_t kMdnsPort = 5353;

// Helper: Attempt non-blocking TCP connect with timeout in milliseconds
bool CheckTcpPort(const std::string& ip, uint16_t port, int timeout_ms) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;

#if defined(_WIN32)
  u_long mode = 1;
  ioctlsocket(fd, FIONBIO, &mode);
#else
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

  int res = connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
  if (res == 0) {
    close(fd);
    return true;
  }

#if defined(_WIN32)
  fd_set setW, setE;
  FD_ZERO(&setW);
  FD_ZERO(&setE);
  FD_SET(fd, &setW);
  FD_SET(fd, &setE);
  struct timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  int sel = select(0, nullptr, &setW, &setE, &tv);
  bool connected = (sel > 0 && FD_ISSET(fd, &setW) && !FD_ISSET(fd, &setE));
#else
  struct pollfd pfd{};
  pfd.fd = fd;
  pfd.events = POLLOUT;
  int poll_res = poll(&pfd, 1, timeout_ms);
  bool connected = false;
  if (poll_res > 0 && (pfd.revents & POLLOUT) && !(pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0) {
      connected = true;
    }
  }
#endif

  close(fd);
  return connected;
}

// Helper: Query Chromecast Eureka Info (HTTP 8008)
bool FetchEurekaInfo(const std::string& ip, std::string* out_name, std::string* out_model, std::string* out_id) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;

  struct timeval tv{};
  tv.tv_sec = 0;
  tv.tv_usec = 400000; // 400ms timeout
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(8008);
  inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

  if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(fd);
    return false;
  }

  std::string req = "GET /setup/eureka_info?params=name,device_info HTTP/1.1\r\n"
                    "Host: " + ip + ":8008\r\n"
                    "User-Agent: CastMirror\r\n"
                    "Connection: close\r\n\r\n";

  send(fd, req.data(), req.size(), 0);

  char buf[4096];
  std::string resp;
  while (true) {
    int r = recv(fd, buf, sizeof(buf) - 1, 0);
    if (r <= 0) break;
    buf[r] = '\0';
    resp += buf;
  }
  close(fd);

  size_t body_pos = resp.find("\r\n\r\n");
  if (body_pos == std::string::npos) return false;

  std::string json_str = resp.substr(body_pos + 4);
  try {
    auto j = nlohmann::json::parse(json_str);
    if (out_name) *out_name = j.value("name", "");
    if (out_model) {
      if (j.contains("device_info") && j["device_info"].is_object()) {
        *out_model = j["device_info"].value("model_name", "Chromecast");
      } else {
        *out_model = "Chromecast";
      }
    }
    if (out_id) {
      if (j.contains("device_info") && j["device_info"].is_object()) {
        *out_id = j["device_info"].value("cloud_device_id", ip);
      } else {
        *out_id = ip;
      }
    }
    return true;
  } catch (...) {
    return false;
  }
}

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
  std::thread([this]() {
    ProbeLocalSubnets();
  }).detach();
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
                                    (device.id.empty() && d.ip_address == device.ip_address) ||
                                    (d.ip_address == device.ip_address);
                           });

    if (it != devices_.end()) {
      it->name = device.name.empty() ? it->name : device.name;
      it->model_name = device.model_name.empty() ? it->model_name : device.model_name;
      it->ip_address = device.ip_address;
      it->port = device.port;
      it->status = device.status;
      it->capabilities = device.capabilities;
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
  uint8_t query[512];
  size_t offset = 0;

  query[offset++] = 0x00; query[offset++] = 0x00; // ID
  query[offset++] = 0x00; query[offset++] = 0x00; // Standard query
  query[offset++] = 0x00; query[offset++] = 0x01; // QDCOUNT = 1
  query[offset++] = 0x00; query[offset++] = 0x00; // ANCOUNT = 0
  query[offset++] = 0x00; query[offset++] = 0x00; // NSCOUNT = 0
  query[offset++] = 0x00; query[offset++] = 0x00; // ARCOUNT = 0

  const char* labels[] = {"_googlecast", "_tcp", "local"};
  for (const char* label : labels) {
    size_t len = std::strlen(label);
    query[offset++] = static_cast<uint8_t>(len);
    std::memcpy(&query[offset], label, len);
    offset += len;
  }
  query[offset++] = 0x00; // Root terminator

  query[offset++] = 0x00; query[offset++] = 0x0C; // QTYPE: PTR (12)
  query[offset++] = 0x80; query[offset++] = 0x01; // QCLASS: IN (1), QU bit (0x8000) for unicast response

  struct sockaddr_in dest_addr{};
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(kMdnsPort);
  inet_pton(AF_INET, kMdnsMulticastGroup, &dest_addr.sin_addr);

  sendto(socket_fd, reinterpret_cast<const char*>(query), offset, 0,
         reinterpret_cast<struct sockaddr*>(&dest_addr), sizeof(dest_addr));
}

void DeviceDiscovery::ProcessMdnsResponse(const uint8_t* buffer, size_t length, const std::string& sender_ip) {
  if (length < 12) return;

  std::vector<std::string> txt_entries;
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

void DeviceDiscovery::ProbeLocalSubnets() {
  std::vector<std::string> target_subnets;

#if !defined(_WIN32)
  struct ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) != -1) {
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
      if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
      if (ifa->ifa_flags & IFF_LOOPBACK) continue;
      if (!(ifa->ifa_flags & IFF_UP)) continue;

      auto* sa = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
      char ip_buf[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &sa->sin_addr, ip_buf, sizeof(ip_buf));
      std::string ip_str(ip_buf);
      size_t last_dot = ip_str.rfind('.');
      if (last_dot != std::string::npos) {
        target_subnets.push_back(ip_str.substr(0, last_dot + 1));
      }
    }
    freeifaddrs(ifaddr);
  }
#else
  // Windows fallback default subnets
  target_subnets.push_back("192.168.0.");
  target_subnets.push_back("192.168.1.");
#endif

  // Remove duplicates
  std::sort(target_subnets.begin(), target_subnets.end());
  target_subnets.erase(std::unique(target_subnets.begin(), target_subnets.end()), target_subnets.end());

  for (const auto& subnet_base : target_subnets) {
    // Probe 1..254 in parallel batches
    const int kBatchSize = 32;
    for (int start_i = 1; start_i <= 254; start_i += kBatchSize) {
      if (!running_.load()) break;
      std::vector<std::future<void>> futures;

      for (int i = start_i; i < start_i + kBatchSize && i <= 254; ++i) {
        std::string ip = subnet_base + std::to_string(i);
        futures.push_back(std::async(std::launch::async, [this, ip]() {
          if (CheckTcpPort(ip, 8009, 150)) {
            std::string name, model, id;
            if (!FetchEurekaInfo(ip, &name, &model, &id) || name.empty()) {
              name = "Cast Device (" + ip + ")";
              model = "Chromecast";
              id = ip;
            }
            CastDevice d;
            d.id = id;
            d.name = name;
            d.model_name = model;
            d.ip_address = ip;
            d.port = 8009;
            d.capabilities = kCapVideoOut | kCapAudioOut;
            d.status = DeviceStatus::kReady;
            AddOrUpdateDevice(d);
          }
        }));
      }

      for (auto& f : futures) {
        f.get();
      }
    }
  }
}

void DeviceDiscovery::DiscoveryLoop() {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd >= 0) {
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
      close(fd);
      fd = socket(AF_INET, SOCK_DGRAM, 0);
    }

    if (fd >= 0) {
      struct ip_mreq mreq{};
      inet_pton(AF_INET, kMdnsMulticastGroup, &mreq.imr_multiaddr);
      mreq.imr_interface.s_addr = htonl(INADDR_ANY);
      setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq), sizeof(mreq));

#if defined(_WIN32)
      DWORD timeout_ms = 1000;
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
#else
      struct timeval tv{};
      tv.tv_sec = 1;
      tv.tv_usec = 0;
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }
  }

  // Run initial subnet scan
  std::thread([this]() {
    ProbeLocalSubnets();
  }).detach();

  auto last_mdns_query = std::chrono::steady_clock::now() - std::chrono::seconds(10);
  auto last_subnet_scan = std::chrono::steady_clock::now();

  while (running_.load()) {
    auto now = std::chrono::steady_clock::now();

    // Send mDNS query every 4 seconds
    if (fd >= 0 && std::chrono::duration_cast<std::chrono::seconds>(now - last_mdns_query).count() >= 4) {
      SendMdnsQuery(fd);
      last_mdns_query = now;
    }

    // Periodic subnet scan every 30 seconds
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_subnet_scan).count() >= 30) {
      std::thread([this]() {
        ProbeLocalSubnets();
      }).detach();
      last_subnet_scan = now;
    }

    if (fd >= 0) {
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
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  }

  if (fd >= 0) {
    close(fd);
  }
}

} // namespace castcore
