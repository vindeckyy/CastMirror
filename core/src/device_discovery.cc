#include "castcore/device_discovery.h"
#include "castcore/logger.h"
#include "castcore/config.h"

#include <nlohmann/json.hpp>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <vector>
#include <set>
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
  int poll_res = poll(&pfd, 1, timeout_ms > 0 ? timeout_ms : 350);
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
  if (ConfigStore::Instance().Get().subnet_scan_enabled) {
    subnet_thread_ = std::thread(&DeviceDiscovery::ProbeLocalSubnets, this);
  }
  return true;
}

void DeviceDiscovery::Stop() {
  if (!running_.exchange(false)) return;

  LOG_INFO << "Stopping Cast device discovery...";
  if (discovery_thread_.joinable()) {
    discovery_thread_.join();
  }
  if (subnet_thread_.joinable()) {
    subnet_thread_.join();
  }
}

bool DeviceDiscovery::IsRunning() const {
  return running_.load();
}

void DeviceDiscovery::TriggerScan() {
  force_mdns_query_ = true;
  if (ConfigStore::Instance().Get().subnet_scan_enabled) {
    std::thread([this]() {
      ProbeLocalSubnets();
    }).detach();
  }
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
  CastDevice normalized = device;
  const bool custom_endpoint = normalized.model_name == "Custom Chromecast" ||
                               normalized.ip_address.rfind("127.", 0) == 0;
  if (!custom_endpoint && normalized.port != 8009 && normalized.port != 8008) {
    LOG_WARN << "Ignoring non-Cast SRV port " << normalized.port
             << " for " << normalized.ip_address << "; using Cast control port 8009";
    normalized.port = 8009;
  }


  DevicesCallback cb;
  std::vector<CastDevice> current;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(devices_.begin(), devices_.end(),
                           [&](const CastDevice& d) {
                             return (!normalized.id.empty() && d.id == normalized.id) ||
                                    (normalized.id.empty() && d.ip_address == normalized.ip_address) ||
                                    (d.ip_address == normalized.ip_address);
                           });

    if (it != devices_.end()) {
      it->id = normalized.id.empty() ? it->id : normalized.id;
      it->name = normalized.name.empty() ? it->name : normalized.name;
      it->model_name = normalized.model_name.empty() ? it->model_name : normalized.model_name;
      it->ip_address = normalized.ip_address;
      it->port = normalized.port;
      it->status = normalized.status;
      it->capabilities = normalized.capabilities;
      it->last_seen = std::chrono::steady_clock::now();
    } else {
      devices_.push_back(normalized);
      devices_.back().last_seen = std::chrono::steady_clock::now();
      LOG_INFO << "Discovered Cast Device: " << normalized.name
               << " (" << normalized.model_name << ") at "
               << normalized.ip_address << ":" << normalized.port;
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
  // Standard Cast V2 control channel always connects on port 8009 (or 8008).
  // Auxiliary SRV records (e.g. streaming ports 10001, airplay 7000) must not override control port.
  device.port = (port == 8009 || port == 8008) ? port : 8009;
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
  auto build_query = [](const std::vector<const char*>& labels, uint16_t qclass, uint8_t* out_buf) -> size_t {
    size_t offset = 0;
    out_buf[offset++] = 0x00; out_buf[offset++] = 0x00; // ID
    out_buf[offset++] = 0x00; out_buf[offset++] = 0x00; // Standard query
    out_buf[offset++] = 0x00; out_buf[offset++] = 0x01; // QDCOUNT = 1
    out_buf[offset++] = 0x00; out_buf[offset++] = 0x00; // ANCOUNT = 0
    out_buf[offset++] = 0x00; out_buf[offset++] = 0x00; // NSCOUNT = 0
    out_buf[offset++] = 0x00; out_buf[offset++] = 0x00; // ARCOUNT = 0

    for (const char* label : labels) {
      size_t len = std::strlen(label);
      out_buf[offset++] = static_cast<uint8_t>(len);
      std::memcpy(&out_buf[offset], label, len);
      offset += len;
    }
    out_buf[offset++] = 0x00; // Root terminator
    out_buf[offset++] = 0x00; out_buf[offset++] = 0x0C; // QTYPE: PTR (12)
    out_buf[offset++] = static_cast<uint8_t>((qclass >> 8) & 0xFF);
    out_buf[offset++] = static_cast<uint8_t>(qclass & 0xFF);
    return offset;
  };

  struct sockaddr_in dest_addr{};
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(kMdnsPort);
  inet_pton(AF_INET, kMdnsMulticastGroup, &dest_addr.sin_addr);

  uint8_t q_buf[512];
  std::vector<std::vector<const char*>> service_targets = {
      {"_googlecast", "_tcp", "local"},
      {"_googlezone", "_tcp", "local"}
  };

  // Set Multicast TTL and Loopback
  unsigned char ttl = 255;
  setsockopt(socket_fd, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
  unsigned char loop = 1;
  setsockopt(socket_fd, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop), sizeof(loop));

#if !defined(_WIN32)
  // Transmit on each non-loopback network interface
  struct ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) != -1) {
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
      if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
      if (ifa->ifa_flags & IFF_LOOPBACK) continue;
      if (!(ifa->ifa_flags & IFF_UP)) continue;

      auto* sa = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
      setsockopt(socket_fd, IPPROTO_IP, IP_MULTICAST_IF, &sa->sin_addr, sizeof(sa->sin_addr));

      for (const auto& target : service_targets) {
        size_t len = build_query(target, 0x0001, q_buf); // QM (Multicast response)
        sendto(socket_fd, reinterpret_cast<const char*>(q_buf), len, 0,
               reinterpret_cast<struct sockaddr*>(&dest_addr), sizeof(dest_addr));
        size_t qlen = build_query(target, 0x8001, q_buf); // QU (Unicast response)
        sendto(socket_fd, reinterpret_cast<const char*>(q_buf), qlen, 0,
               reinterpret_cast<struct sockaddr*>(&dest_addr), sizeof(dest_addr));
      }
    }
    freeifaddrs(ifaddr);
  }
#endif

  // Default interface send
  struct in_addr any_addr{};
  any_addr.s_addr = htonl(INADDR_ANY);
  setsockopt(socket_fd, IPPROTO_IP, IP_MULTICAST_IF, &any_addr, sizeof(any_addr));

  for (const auto& target : service_targets) {
    size_t len = build_query(target, 0x0001, q_buf);
    sendto(socket_fd, reinterpret_cast<const char*>(q_buf), len, 0,
           reinterpret_cast<struct sockaddr*>(&dest_addr), sizeof(dest_addr));
    size_t qlen = build_query(target, 0x8001, q_buf);
    sendto(socket_fd, reinterpret_cast<const char*>(q_buf), qlen, 0,
           reinterpret_cast<struct sockaddr*>(&dest_addr), sizeof(dest_addr));
  }
}

namespace {

size_t SkipDnsName(const uint8_t* buf, size_t len, size_t offset) {
  size_t jumped = 0;
  while (offset < len) {
    uint8_t l = buf[offset];
    if (l == 0) {
      return offset + 1;
    }
    if ((l & 0xC0) == 0xC0) {
      return offset + 2;
    }
    offset += 1 + l;
    jumped++;
    if (jumped > 100) break;
  }
  return offset;
}

} // namespace

void DeviceDiscovery::ProcessMdnsResponse(const uint8_t* buffer, size_t length, const std::string& sender_ip) {
  if (length < 12) return;

  std::vector<std::string> txt_entries;
  uint16_t parsed_port = 8009;
  std::string parsed_name;

  // Structured DNS Resource Record parser
  uint16_t qdcount = (buffer[4] << 8) | buffer[5];
  uint16_t ancount = (buffer[6] << 8) | buffer[7];
  uint16_t nscount = (buffer[8] << 8) | buffer[9];
  uint16_t arcount = (buffer[10] << 8) | buffer[11];
  size_t total_records = ancount + nscount + arcount;

  size_t offset = 12;
  for (uint16_t q = 0; q < qdcount && offset < length; ++q) {
    offset = SkipDnsName(buffer, length, offset);
    offset += 4; // QTYPE (2) + QCLASS (2)
  }

  for (size_t r = 0; r < total_records && offset < length; ++r) {
    offset = SkipDnsName(buffer, length, offset);
    if (offset + 10 > length) break;

    uint16_t rtype = (buffer[offset] << 8) | buffer[offset + 1];
    // uint16_t rclass = (buffer[offset + 2] << 8) | buffer[offset + 3];
    // uint32_t ttl = (buffer[offset + 4] << 24) | ...
    uint16_t rdlength = (buffer[offset + 8] << 8) | buffer[offset + 9];
    offset += 10;

    if (offset + rdlength > length) break;

    if (rtype == 16) { // TXT Record
      size_t txt_pos = offset;
      size_t txt_end = offset + rdlength;
      while (txt_pos < txt_end) {
        uint8_t tlen = buffer[txt_pos++];
        if (tlen > 0 && txt_pos + tlen <= txt_end) {
          std::string entry(reinterpret_cast<const char*>(&buffer[txt_pos]), tlen);
          txt_entries.push_back(entry);
          txt_pos += tlen;
        }
      }
    } else if (rtype == 33 && rdlength >= 6) { // SRV Record
      uint16_t srv_port = (buffer[offset + 4] << 8) | buffer[offset + 5];
      if (srv_port == 8009 || srv_port == 8008) {
        parsed_port = srv_port;
      }
    }
    offset += rdlength;
  }

  // Fallback sliding-window scan if structured parser didn't find any TXT record
  if (txt_entries.empty()) {
    const char* const kKnownKeys[] = {"fn=", "id=", "md=", "ca=", "st=", "ve=", "rs=", "bs="};
    std::set<std::string> seen_keys;
    for (size_t i = 0; i < length; ++i) {
      for (const char* key : kKnownKeys) {
        size_t klen = std::strlen(key);
        if (i + klen <= length && std::memcmp(&buffer[i], key, klen) == 0) {
          std::string prefix(key);
          if (seen_keys.count(prefix)) continue;

          size_t val_start = i;
          size_t val_end = i + klen;
          while (val_end < length && buffer[val_end] >= 32 && buffer[val_end] <= 126) {
            val_end++;
          }
          if (val_end > val_start) {
            std::string entry(reinterpret_cast<const char*>(&buffer[val_start]), val_end - val_start);
            txt_entries.push_back(entry);
            seen_keys.insert(prefix);
          }
        }
      }
    }
  }

  if (!txt_entries.empty()) {
    CastDevice dev = ParseFromMdnsData(parsed_name, sender_ip, parsed_port, txt_entries);
    AddOrUpdateDevice(dev);
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
      // Join on default interface
      struct ip_mreq mreq{};
      inet_pton(AF_INET, kMdnsMulticastGroup, &mreq.imr_multiaddr);
      mreq.imr_interface.s_addr = htonl(INADDR_ANY);
      setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq), sizeof(mreq));

#if !defined(_WIN32)
      // Join on each active non-loopback network interface
      struct ifaddrs* ifaddr = nullptr;
      if (getifaddrs(&ifaddr) != -1) {
        for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
          if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
          if (ifa->ifa_flags & IFF_LOOPBACK) continue;
          if (!(ifa->ifa_flags & IFF_UP)) continue;

          auto* sa = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
          struct ip_mreq if_mreq{};
          inet_pton(AF_INET, kMdnsMulticastGroup, &if_mreq.imr_multiaddr);
          if_mreq.imr_interface = sa->sin_addr;
          setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&if_mreq), sizeof(if_mreq));
        }
        freeifaddrs(ifaddr);
      }
#endif

      unsigned char ttl = 255;
      setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
      unsigned char loop = 1;
      setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop), sizeof(loop));

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

  auto last_mdns_query = std::chrono::steady_clock::now() - std::chrono::seconds(10);

  while (running_.load()) {
    auto now = std::chrono::steady_clock::now();

    // Send mDNS query every 3 seconds or immediately on TriggerScan
    if (fd >= 0 && (force_mdns_query_.exchange(false) ||
                    std::chrono::duration_cast<std::chrono::seconds>(now - last_mdns_query).count() >= 3)) {
      SendMdnsQuery(fd);
      last_mdns_query = now;
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
          if (CheckTcpPort(ip, 8009, 350)) {
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

} // namespace castcore
