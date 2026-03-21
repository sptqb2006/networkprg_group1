#pragma once
// TCP message framing: [4-byte big-endian length][payload]
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace monitor::net {

inline bool setRecvTimeout(int fd, int seconds) {
  struct timeval tv{};
  tv.tv_sec = seconds;
  tv.tv_usec = 0;
  return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
}

inline bool sendAll(int fd, const void *buf, size_t len) {
  const char *p = reinterpret_cast<const char *>(buf);
  while (len > 0) {
    ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
    if (n <= 0) return false;
    p += n; len -= n;
  }
  return true;
}

inline bool recvAll(int fd, void *buf, size_t len) {
  char *p = reinterpret_cast<char *>(buf);
  while (len > 0) {
    ssize_t n = ::recv(fd, p, len, 0);
    if (n <= 0) return false;
    p += n; len -= n;
  }
  return true;
}

inline bool sendMsg(int fd, const std::string &payload) {
  // Peek to detect closed connection before writing
  char peek = 0;
  ssize_t peeked = ::recv(fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
  if (peeked == 0) return false;
  if (peeked < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return false;

  uint32_t netLen = htonl(static_cast<uint32_t>(payload.size()));
  if (!sendAll(fd, &netLen, 4)) return false;
  return sendAll(fd, payload.data(), payload.size());
}

inline std::string recvMsg(int fd) {
  uint32_t netLen = 0;
  if (!recvAll(fd, &netLen, 4)) return {};
  uint32_t len = ntohl(netLen);
  if (len == 0 || len > 4 * 1024 * 1024) return {};
  std::string buf(len, '\0');
  if (!recvAll(fd, buf.data(), len)) return {};
  return buf;
}

} // namespace monitor::net
