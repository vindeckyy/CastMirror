#include <gtest/gtest.h>
#include "castcore/http_fallback_server.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <chrono>

using namespace castcore;

TEST(HttpFallbackTest, FormatCafLoadMessage) {
  std::string url = "http://192.168.1.50:8088/live.mp4";
  std::string msg = HttpFallbackServer::FormatCafLoadMessage(url, "video/mp4", 42);

  EXPECT_NE(msg.find("\"type\":\"LOAD\""), std::string::npos);
  EXPECT_NE(msg.find("\"requestId\":42"), std::string::npos);
  EXPECT_NE(msg.find("\"contentId\":\"http://192.168.1.50:8088/live.mp4\""), std::string::npos);
  EXPECT_NE(msg.find("\"streamType\":\"LIVE\""), std::string::npos);
  EXPECT_NE(msg.find("\"contentType\":\"video/mp4\""), std::string::npos);
}

TEST(HttpFallbackTest, ServerLifecycleAndStreamDelivery) {
  HttpFallbackServer server;
  // Port 0 binds to an ephemeral port
  ASSERT_TRUE(server.Start(0));
  EXPECT_TRUE(server.IsRunning());
  uint16_t port = server.GetPort();
  EXPECT_GT(port, 0);

  std::string stream_url = server.GetStreamUrl("192.168.1.10");
  EXPECT_NE(stream_url.find("http://192.168.1.10:"), std::string::npos);
  EXPECT_NE(stream_url.find("/live.mp4"), std::string::npos);

  // Push a keyframe
  EncodedFrame kf;
  kf.dependency = FrameDependency::kKeyFrame;
  kf.data = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1E};
  server.PushVideoFrame(kf);

  // Connect as an HTTP client
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(sock, 0);

  struct sockaddr_in saddr{};
  saddr.sin_family = AF_INET;
  saddr.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &saddr.sin_addr);

  ASSERT_EQ(connect(sock, (struct sockaddr*)&saddr, sizeof(saddr)), 0);

  const char* req = "GET /live.mp4 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  send(sock, req, strlen(req), 0);

  char buf[1024];
  int n = recv(sock, buf, sizeof(buf) - 1, 0);
  EXPECT_GT(n, 0);
  buf[n] = '\0';

  std::string response(buf, n);
  EXPECT_NE(response.find("200 OK"), std::string::npos);
  EXPECT_NE(response.find("Access-Control-Allow-Origin: *"), std::string::npos);

  close(sock);
  server.Stop();
  EXPECT_FALSE(server.IsRunning());
}
