#include "castcore/device_discovery.h"
#include "castcore/logger.h"
#include "castcore/config.h"
#include "castcore/net_platform.h"

#include <nlohmann/json.hpp>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <vector>
#include <set>
#include <thread>
#include <future>
#include <sstream>
#include <random>

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

// Jitter for the subnet-probe retry backoff.
//
// This used to be `rand()`, which is a process-global: concurrent discovery
// calls contend on the same state, it is not reproducible under
// ThreadSanitizer, and seeding it deterministically would silently change the
// production backoff. A per-thread engine keeps the same 10-34 ms spread while
// removing the shared state. Discovery runs on a dedicated thread, so the
// seeding cost is paid once.
int RetryJitterMs(int base_ms, int spread_ms) {
  static thread_local std::mt19937 engine(std::random_device{}());
  std::uniform_int_distribution<int> dist(0, spread_ms > 0 ? spread_ms - 1 : 0);
  return base_ms + dist(engine);
}

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
bool FetchEurekaInfo(const std::string& ip, std::string* out_name, std::string* out_model,
                     std::string* out_id) {
  // Phase 2: 400ms timeout + 2 retries with jitter, strict JSON parse
  constexpr int kMaxAttempts = 3;  // initial + 2 retries
  constexpr int kTimeoutUs = 400000;  // 400ms
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      if (attempt + 1 < kMaxAttempts) {
        int jitter_ms = RetryJitterMs(attempt * 17, 30);
        std::this_thread::sleep_for(std::chrono::milliseconds(jitter_ms));
        continue;
      }
      return false;
    }

#if defined(_WIN32)
    // Windows SO_RCVTIMEO/SO_SNDTIMEO take a DWORD in milliseconds, not a
    // struct timeval — passing a timeval is read as timeout=0 (infinite) and
    // can hang the probe forever.
    DWORD sock_timeout_ms = static_cast<DWORD>(kTimeoutUs / 1000);
    setsockopt(fd,
               SOL_SOCKET,
               SO_RCVTIMEO,
               reinterpret_cast<const char*>(&sock_timeout_ms),
               sizeof(sock_timeout_ms));
    setsockopt(fd,
               SOL_SOCKET,
               SO_SNDTIMEO,
               reinterpret_cast<const char*>(&sock_timeout_ms),
               sizeof(sock_timeout_ms));
#else
    struct timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = kTimeoutUs;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif

#if defined(_WIN32)
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8008);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    int conn_res = connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    bool connected = false;
    if (conn_res == 0) {
      connected = true;
    } else {
#if defined(_WIN32)
      fd_set setW, setE;
      FD_ZERO(&setW);
      FD_ZERO(&setE);
      FD_SET(fd, &setW);
      FD_SET(fd, &setE);
      struct timeval ctv{};
      ctv.tv_sec = 0;
      ctv.tv_usec = kTimeoutUs;
      int sel = select(0, nullptr, &setW, &setE, &ctv);
      if (sel > 0 && FD_ISSET(fd, &setW) && !FD_ISSET(fd, &setE)) {
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) == 0 &&
            err == 0) {
          connected = true;
        }
      }
#else
      // Non-blocking connect: poll for writability with timeout
      struct pollfd pfd{};
      pfd.fd = fd;
      pfd.events = POLLOUT;
      int pr = poll(&pfd, 1, kTimeoutUs / 1000);
      if (pr > 0 && (pfd.revents & POLLOUT)) {
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
          connected = true;
        }
      } else if (pr > 0) {
        // Check error
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err == 0) connected = true;
      }
      // Restore blocking for send/recv timeout handling
      if (flags >= 0) fcntl(fd, F_SETFL, flags);
#endif
    }

    if (!connected) {
      close(fd);
      if (attempt + 1 < kMaxAttempts) {
        int jitter_ms = RetryJitterMs(20 + attempt * 15, 40);
        std::this_thread::sleep_for(std::chrono::milliseconds(jitter_ms));
        continue;
      }
      return false;
    }

#if defined(_WIN32)
    mode = 0;
    ioctlsocket(fd, FIONBIO, &mode);
#endif

    std::string req =
        "GET /setup/eureka_info?params=name,device_info HTTP/1.1\r\n"
        "Host: " +
        ip +
        ":8008\r\n"
        "User-Agent: CastMirror\r\n"
        "Connection: close\r\n\r\n";

    if (send(fd, req.data(), req.size(), 0) < 0) {
      close(fd);
      if (attempt + 1 < kMaxAttempts) {
        std::this_thread::sleep_for(std::chrono::milliseconds(RetryJitterMs(20, 30)));
        continue;
      }
      return false;
    }

    char buf[4096];
    std::string resp;
    bool recv_ok = false;
    while (true) {
      int r = recv(fd, buf, sizeof(buf) - 1, 0);
      if (r > 0) {
        buf[r] = '\0';
        resp += buf;
        recv_ok = true;
      } else if (r == 0) {
        break;
      } else {
#if defined(_WIN32)
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK || err == WSAETIMEDOUT) break;
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
#endif
        break;
      }
    }
    close(fd);

    if (!recv_ok) {
      if (attempt + 1 < kMaxAttempts) {
        std::this_thread::sleep_for(std::chrono::milliseconds(RetryJitterMs(15, 35)));
        continue;
      }
      return false;
    }

    size_t body_pos = resp.find("\r\n\r\n");
    if (body_pos == std::string::npos) {
      if (attempt + 1 < kMaxAttempts) {
        std::this_thread::sleep_for(std::chrono::milliseconds(RetryJitterMs(10, 20)));
        continue;
      }
      return false;
    }

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
    } catch (const nlohmann::json::exception&) {
      if (attempt + 1 < kMaxAttempts) {
        std::this_thread::sleep_for(std::chrono::milliseconds(RetryJitterMs(10, 25)));
        continue;
      }
      return false;
    } catch (...) {
      if (attempt + 1 < kMaxAttempts) {
        std::this_thread::sleep_for(std::chrono::milliseconds(RetryJitterMs(10, 25)));
        continue;
      }
      return false;
    }
  }
  return false;
}

}  // namespace

DeviceDiscovery::DeviceDiscovery() = default;

DeviceDiscovery::~DeviceDiscovery() {
  Stop();
}

bool DeviceDiscovery::Start() {
  if (running_.exchange(true)) return true;

  // Winsock must be up before the discovery thread calls socket(); on POSIX
  // this is a no-op. Without it every socket() call fails with
  // WSANOTINITIALISED and discovery silently finds nothing.
  if (!EnsureSocketInit()) {
    LOG_ERROR << "Socket subsystem initialization failed; discovery cannot run";
    running_ = false;
    return false;
  }

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
    std::thread([this]() { ProbeLocalSubnets(); }).detach();
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
  const bool custom_endpoint =
      normalized.model_name == "Custom Chromecast" || normalized.ip_address.rfind("127.", 0) == 0;
  if (!custom_endpoint && normalized.port != 8009 && normalized.port != 8008) {
    LOG_WARN << "Ignoring non-Cast SRV port " << normalized.port << " for " << normalized.ip_address
             << "; using Cast control port 8009";
    normalized.port = 8009;
  }

  DevicesCallback cb;
  std::vector<CastDevice> current;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(devices_.begin(), devices_.end(), [&](const CastDevice& d) {
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
      LOG_INFO << "Discovered Cast Device: " << normalized.name << " (" << normalized.model_name
               << ") at " << normalized.ip_address << ":" << normalized.port;
    }

    current = devices_;
    cb = callback_;
  }

  if (cb) {
    cb(current);
  }
}

size_t DeviceDiscovery::ExpireStaleDevices(std::chrono::steady_clock::time_point now,
                                           std::chrono::seconds ttl) {
  DevicesCallback cb;
  std::vector<CastDevice> current;
  size_t removed = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = devices_.begin(); it != devices_.end();) {
      const bool from_mdns = mdns_ids_.count(it->id) > 0;
      if (from_mdns && now - it->last_seen > ttl) {
        LOG_INFO << "Device " << it->name << " has not been seen for " << ttl.count()
                 << " s; removing it from the list";
        mdns_ids_.erase(it->id);
        it = devices_.erase(it);
        ++removed;
      } else {
        ++it;
      }
    }
    if (removed > 0) {
      current = devices_;
      cb = callback_;
    }
  }
  if (cb) cb(current);
  return removed;
}

void DeviceDiscovery::RemoveDevice(const std::string& device_id) {
  DevicesCallback cb;
  std::vector<CastDevice> current;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::remove_if(
        devices_.begin(), devices_.end(), [&](const CastDevice& d) { return d.id == device_id; });
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

std::map<std::string, std::string> DeviceDiscovery::ParseTxtRecord(
    const std::vector<std::string>& txt_entries) {
  std::map<std::string, std::string> result;
  constexpr size_t kMaxEntrySize = 255;
  constexpr size_t kMaxKeySize = 64;
  constexpr size_t kMaxValSize = 255;
  for (const auto& entry : txt_entries) {
    if (entry.empty()) continue;
    // Oversize guard: TXT entry length byte max 255
    if (entry.size() > kMaxEntrySize) {
      // Oversize entries are truncated/ignored per RFC 6763; keep robustness by skipping
      continue;
    }
    auto eq = entry.find('=');
    if (eq != std::string::npos) {
      std::string key = entry.substr(0, eq);
      std::string val = entry.substr(eq + 1);
      if (key.empty()) continue;
      if (key.size() > kMaxKeySize) continue;
      if (val.size() > kMaxValSize) {
        // Oversize value: truncate to limit or skip; we skip to avoid overflow
        continue;
      }
      // Duplicate handling: keep first occurrence, ignore subsequent duplicates
      if (result.find(key) != result.end()) {
        continue;
      }
      result[key] = val;
    } else {
      // Key without value
      if (entry.size() > kMaxKeySize) continue;
      if (result.find(entry) != result.end()) continue;
      result[entry] = "";
    }
  }
  return result;
}

CastDevice DeviceDiscovery::ParseFromMdnsData(const std::string& name, const std::string& ip,
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
  auto build_query =
      [](const std::vector<const char*>& labels, uint16_t qclass, uint8_t* out_buf) -> size_t {
    size_t offset = 0;
    out_buf[offset++] = 0x00;
    out_buf[offset++] = 0x00;  // ID
    out_buf[offset++] = 0x00;
    out_buf[offset++] = 0x00;  // Standard query
    out_buf[offset++] = 0x00;
    out_buf[offset++] = 0x01;  // QDCOUNT = 1
    out_buf[offset++] = 0x00;
    out_buf[offset++] = 0x00;  // ANCOUNT = 0
    out_buf[offset++] = 0x00;
    out_buf[offset++] = 0x00;  // NSCOUNT = 0
    out_buf[offset++] = 0x00;
    out_buf[offset++] = 0x00;  // ARCOUNT = 0

    for (const char* label : labels) {
      size_t len = std::strlen(label);
      out_buf[offset++] = static_cast<uint8_t>(len);
      std::memcpy(&out_buf[offset], label, len);
      offset += len;
    }
    out_buf[offset++] = 0x00;  // Root terminator
    out_buf[offset++] = 0x00;
    out_buf[offset++] = 0x0C;  // QTYPE: PTR (12)
    out_buf[offset++] = static_cast<uint8_t>((qclass >> 8) & 0xFF);
    out_buf[offset++] = static_cast<uint8_t>(qclass & 0xFF);
    return offset;
  };

  struct sockaddr_in dest_addr{};
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(kMdnsPort);
  inet_pton(AF_INET, kMdnsMulticastGroup, &dest_addr.sin_addr);

  uint8_t q_buf[512];
  std::vector<std::vector<const char*>> service_targets = {{"_googlecast", "_tcp", "local"},
                                                           {"_googlezone", "_tcp", "local"}};

  // Set Multicast TTL and Loopback
  unsigned char ttl = 255;
  setsockopt(
      socket_fd, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
  unsigned char loop = 1;
  setsockopt(
      socket_fd, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop), sizeof(loop));

  // Transmit on each active non-loopback IPv4 interface. This matters on
  // multi-NIC machines (Ethernet + WiFi + VPN/virtual adapters): the default
  // route can point at a virtual adapter while the Chromecasts live on the
  // physical LAN, so only sending on the default interface loses devices.
  for (const auto& iface : EnumerateIPv4Interfaces()) {
    if (!iface.is_up || iface.is_loopback) continue;

    struct in_addr if_addr{};
    if (inet_pton(AF_INET, iface.address.c_str(), &if_addr) <= 0) continue;
    setsockopt(socket_fd,
               IPPROTO_IP,
               IP_MULTICAST_IF,
               reinterpret_cast<const char*>(&if_addr),
               sizeof(if_addr));

    for (const auto& target : service_targets) {
      size_t len = build_query(target, 0x0001, q_buf);  // QM (Multicast response)
      sendto(socket_fd,
             reinterpret_cast<const char*>(q_buf),
             len,
             0,
             reinterpret_cast<struct sockaddr*>(&dest_addr),
             sizeof(dest_addr));
      size_t qlen = build_query(target, 0x8001, q_buf);  // QU (Unicast response)
      sendto(socket_fd,
             reinterpret_cast<const char*>(q_buf),
             qlen,
             0,
             reinterpret_cast<struct sockaddr*>(&dest_addr),
             sizeof(dest_addr));
    }
  }

  // Default interface send
  struct in_addr any_addr{};
  any_addr.s_addr = htonl(INADDR_ANY);
  setsockopt(socket_fd,
             IPPROTO_IP,
             IP_MULTICAST_IF,
             reinterpret_cast<const char*>(&any_addr),
             sizeof(any_addr));

  for (const auto& target : service_targets) {
    size_t len = build_query(target, 0x0001, q_buf);
    sendto(socket_fd,
           reinterpret_cast<const char*>(q_buf),
           len,
           0,
           reinterpret_cast<struct sockaddr*>(&dest_addr),
           sizeof(dest_addr));
    size_t qlen = build_query(target, 0x8001, q_buf);
    sendto(socket_fd,
           reinterpret_cast<const char*>(q_buf),
           qlen,
           0,
           reinterpret_cast<struct sockaddr*>(&dest_addr),
           sizeof(dest_addr));
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

}  // namespace

void DeviceDiscovery::ProcessMdnsResponse(const uint8_t* buffer, size_t length,
                                          const std::string& sender_ip) {
  if (length < 12) return;
  // Only responses describe devices. Other hosts' queries can carry known-answer
  // records, and our own multicast queries loop back to this socket.
  if ((buffer[2] & 0x80) == 0) return;

  std::vector<std::string> txt_entries;
  uint16_t parsed_port = 8009;
  std::string parsed_name;
  // The device's own address when the response carries an A record; the packet
  // source can be a reflector or a different interface of a multi-homed device.
  std::string parsed_ip;

  // Structured DNS Resource Record parser
  uint16_t qdcount = (buffer[4] << 8) | buffer[5];
  uint16_t ancount = (buffer[6] << 8) | buffer[7];
  uint16_t nscount = (buffer[8] << 8) | buffer[9];
  uint16_t arcount = (buffer[10] << 8) | buffer[11];
  size_t total_records = ancount + nscount + arcount;

  size_t offset = 12;
  for (uint16_t q = 0; q < qdcount && offset < length; ++q) {
    offset = SkipDnsName(buffer, length, offset);
    offset += 4;  // QTYPE (2) + QCLASS (2)
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

    if (rtype == 16) {  // TXT Record
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
    } else if (rtype == 1 && rdlength == 4 && parsed_ip.empty()) {  // A Record
      char ip_buf[INET_ADDRSTRLEN];
      if (inet_ntop(AF_INET, &buffer[offset], ip_buf, sizeof(ip_buf))) {
        parsed_ip = ip_buf;
      }
    } else if (rtype == 33 && rdlength >= 6) {  // SRV Record
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
            std::string entry(reinterpret_cast<const char*>(&buffer[val_start]),
                              val_end - val_start);
            txt_entries.push_back(entry);
            seen_keys.insert(prefix);
          }
        }
      }
    }
  }

  if (!txt_entries.empty()) {
    // A Cast device always publishes its id or friendly name. Unrelated mDNS
    // traffic (printers, AirPlay, other TXT records on the LAN) has neither and
    // must not show up as a phantom TV.
    const auto keys = ParseTxtRecord(txt_entries);
    if (!keys.count("id") && !keys.count("fn")) return;
    CastDevice dev = ParseFromMdnsData(
        parsed_name, parsed_ip.empty() ? sender_ip : parsed_ip, parsed_port, txt_entries);
    AddOrUpdateDevice(dev);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      mdns_ids_.insert(dev.id);
    }
  }
}

void DeviceDiscovery::DiscoveryLoop() {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    LOG_ERROR << "Failed to create mDNS discovery socket: " << SocketErrorString();
  }
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
      LOG_WARN << "mDNS bind to port " << kMdnsPort << " failed: " << SocketErrorString()
               << " — discovery will rely on unicast replies and subnet probing";
      close(fd);
      fd = socket(AF_INET, SOCK_DGRAM, 0);
      if (fd < 0) {
        LOG_ERROR << "Failed to recreate discovery socket: " << SocketErrorString();
      }
    }

    if (fd >= 0) {
      // Join on default interface
      struct ip_mreq mreq{};
      inet_pton(AF_INET, kMdnsMulticastGroup, &mreq.imr_multiaddr);
      mreq.imr_interface.s_addr = htonl(INADDR_ANY);
      setsockopt(
          fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq), sizeof(mreq));

      // Join on each active non-loopback IPv4 interface so multicast replies
      // reach us no matter which NIC the Chromecasts are on.
      for (const auto& iface : EnumerateIPv4Interfaces()) {
        if (!iface.is_up || iface.is_loopback) continue;
        struct ip_mreq if_mreq{};
        inet_pton(AF_INET, kMdnsMulticastGroup, &if_mreq.imr_multiaddr);
        if (inet_pton(AF_INET, iface.address.c_str(), &if_mreq.imr_interface) <= 0) continue;
        setsockopt(fd,
                   IPPROTO_IP,
                   IP_ADD_MEMBERSHIP,
                   reinterpret_cast<const char*>(&if_mreq),
                   sizeof(if_mreq));
      }

      unsigned char ttl = 255;
      setsockopt(
          fd, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
      unsigned char loop = 1;
      setsockopt(
          fd, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop), sizeof(loop));

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
  auto last_expiry = std::chrono::steady_clock::now();

  while (running_.load()) {
    auto now = std::chrono::steady_clock::now();

    // Send mDNS query every 3 seconds or immediately on TriggerScan
    if (fd >= 0 &&
        (force_mdns_query_.exchange(false) ||
         std::chrono::duration_cast<std::chrono::seconds>(now - last_mdns_query).count() >= 3)) {
      SendMdnsQuery(fd);
      last_mdns_query = now;
    }

    // Devices answer every 3 s query while they are on. Ten minutes of silence
    // means switched off or gone, not a dropped packet.
    if (now - last_expiry >= std::chrono::seconds(30)) {
      ExpireStaleDevices(now, std::chrono::minutes(10));
      last_expiry = now;
    }

    if (fd >= 0) {
      uint8_t buffer[4096];
      struct sockaddr_in src_addr{};
      socklen_t addr_len = sizeof(src_addr);

      int bytes_read = recvfrom(fd,
                                reinterpret_cast<char*>(buffer),
                                sizeof(buffer),
                                0,
                                reinterpret_cast<struct sockaddr*>(&src_addr),
                                &addr_len);

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

  // Probe the /24 containing each active non-loopback interface. This is the
  // same heuristic used on every platform: Chromecasts are almost always on
  // the host's own subnet, and a /24 keeps the probe bounded (~4s at 64/s).
  for (const auto& iface : EnumerateIPv4Interfaces()) {
    if (!iface.is_up || iface.is_loopback) continue;
    size_t last_dot = iface.address.rfind('.');
    if (last_dot != std::string::npos) {
      target_subnets.push_back(iface.address.substr(0, last_dot + 1));
    }
  }

  // Remove duplicates
  std::sort(target_subnets.begin(), target_subnets.end());
  target_subnets.erase(std::unique(target_subnets.begin(), target_subnets.end()),
                       target_subnets.end());

  for (const auto& subnet_base : target_subnets) {
    // Phase 2: rate-limit subnet probe concurrency 32 rate 64 hosts/sec
    const int kBatchSize = 32;
    constexpr double kRatePerSec = 64.0;
    const auto kMinBatchInterval = std::chrono::milliseconds(
        static_cast<int>(1000.0 * kBatchSize / kRatePerSec));  // 500ms per 32
    for (int start_i = 1; start_i <= 254; start_i += kBatchSize) {
      if (!running_.load()) break;
      auto batch_start = std::chrono::steady_clock::now();
      std::vector<std::future<void>> futures;
      futures.reserve(kBatchSize);

      int batch_end = std::min(start_i + kBatchSize - 1, 254);
      for (int i = start_i; i <= batch_end; ++i) {
        std::string ip = subnet_base + std::to_string(i);
        // Ignore 127.0.0.0/8 loopback and own IPs already filtered via IFF_LOOPBACK, but double-check
        if (ip.rfind("127.", 0) == 0) continue;
        // Skip own interface IPs: compare against discovered subnet bases (own subnet would be probe to self)
        // We already avoid duplicate subnet bases, but skip broadcast and network address
        futures.push_back(std::async(std::launch::async, [this, ip]() {
          if (!running_.load()) return;
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
        try {
          f.get();
        } catch (...) {}
      }
      // Rate limit: ensure at most 64 hosts/sec
      auto batch_elapsed = std::chrono::steady_clock::now() - batch_start;
      if (batch_elapsed < kMinBatchInterval) {
        auto sleep_dur = kMinBatchInterval - batch_elapsed;
        // Sleep in small chunks to allow early exit if Stop() called
        auto sleep_until = std::chrono::steady_clock::now() + sleep_dur;
        while (running_.load() && std::chrono::steady_clock::now() < sleep_until) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
      }
    }
  }
}

}  // namespace castcore
