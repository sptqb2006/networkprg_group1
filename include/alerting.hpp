#pragma once
// Alert dispatcher: HTTP POST webhook on status transitions (Slack/Discord/Teams).
// Per-host cooldown, recovery notifications, non-blocking (detached thread).
#include "logger.hpp"
#include "protocol.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace monitor {

struct AlertConfig {
  std::string webhookUrl;
  int  cooldownSec = 300;
  bool enabled     = false;
};

struct ParsedUrl {
  std::string host, path;
  uint16_t    port = 80;
  bool        ok   = false;
};

inline ParsedUrl parseUrl(const std::string &url) {
  ParsedUrl p;
  std::string u = url;
  bool isHttps = false;
  if (u.rfind("https://", 0) == 0) { u = u.substr(8); isHttps = true; }
  else if (u.rfind("http://", 0) == 0) { u = u.substr(7); }
  else return p;

  p.port = isHttps ? 443 : 80;
  auto slash = u.find('/');
  std::string hostpart = (slash==std::string::npos) ? u : u.substr(0, slash);
  p.path = (slash==std::string::npos) ? "/" : u.substr(slash);

  auto colon = hostpart.rfind(':');
  if (colon != std::string::npos) {
    try { p.port = (uint16_t)std::stoi(hostpart.substr(colon+1)); } catch(...) {}
    p.host = hostpart.substr(0, colon);
  } else {
    p.host = hostpart;
  }
  p.ok = !p.host.empty();
  return p;
}

// Fire-and-forget HTTP POST (no TLS)
inline void httpPost(const std::string &url, const std::string &jsonBody) {
  auto p = parseUrl(url);
  if (!p.ok) { LOG_WARN("alerting: invalid webhook URL: " + url); return; }
  if (p.port == 443) {
    LOG_WARN("alerting: HTTPS not supported, use HTTP or tunnel");
    return;
  }

  addrinfo hints{}, *res = nullptr;
  hints.ai_family   = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(p.host.c_str(), std::to_string(p.port).c_str(), &hints, &res) != 0) {
    LOG_WARN("alerting: cannot resolve host: " + p.host);
    return;
  }
  int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd < 0) { freeaddrinfo(res); return; }

  struct timeval tv{}; tv.tv_sec = 5;
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  bool connected = (connect(fd, res->ai_addr, res->ai_addrlen) == 0);
  freeaddrinfo(res);
  if (!connected) { close(fd); LOG_WARN("alerting: connect failed to " + p.host); return; }

  std::string req =
    "POST " + p.path + " HTTP/1.1\r\n"
    "Host: " + p.host + "\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: " + std::to_string(jsonBody.size()) + "\r\n"
    "Connection: close\r\n\r\n" + jsonBody;

  if (send(fd, req.c_str(), req.size(), MSG_NOSIGNAL) <= 0)
    LOG_WARN("alerting: send failed");
  else
    LOG_INFO("alerting: webhook sent to " + p.host + p.path);

  close(fd);
}

inline const char *statusName(HostStatus s) {
  switch(s) {
  case HostStatus::ALERT:   return "ALERT";
  case HostStatus::WARNING: return "WARN";
  case HostStatus::STALE:   return "STALE";
  case HostStatus::ONLINE:  return "ONLINE";
  case HostStatus::OFFLINE: return "OFFLINE";
  }
  return "UNKNOWN";
}

class Alerter {
public:
  explicit Alerter(AlertConfig cfg) : cfg_(std::move(cfg)) {}

  bool maybeAlert(const std::string &host, HostStatus prev, HostStatus cur,
                  float cpu, float ram, float disk) {
    if (!cfg_.enabled || cfg_.webhookUrl.empty()) return false;

    bool wasOk      = (prev == HostStatus::ONLINE || prev == HostStatus::WARNING);
    bool nowBad     = (cur  == HostStatus::ALERT  || cur  == HostStatus::OFFLINE ||
                       cur  == HostStatus::STALE);
    bool recovery   = (!wasOk && cur == HostStatus::ONLINE);

    if (!nowBad && !recovery) return false;
    if (onCooldown(host, recovery)) return false;

    std::string emoji  = (cur==HostStatus::ALERT)?"🔴":(cur==HostStatus::OFFLINE||cur==HostStatus::STALE)?"⚫":"🟢";
    std::string title  = recovery
      ? "✅ Recovery: " + host + " is back ONLINE"
      : emoji + " Alert: " + host + " → " + statusName(cur);

    char detail[128];
    snprintf(detail, sizeof(detail), "CPU %.1f%%  RAM %.1f%%  DISK %.1f%%", cpu, ram, disk);

    std::string body = "{\"text\":\"" + title + "\\n" + std::string(detail) + "\"}";

    std::thread([this, body]() { httpPost(cfg_.webhookUrl, body); }).detach();

    updateCooldown(host);
    return true;
  }

private:
  bool onCooldown(const std::string &host, bool) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = lastAlert_.find(host);
    if (it == lastAlert_.end()) return false;
    double elapsed = std::difftime(time(nullptr), it->second);
    return elapsed < cfg_.cooldownSec;
  }

  void updateCooldown(const std::string &host) {
    std::lock_guard<std::mutex> lk(mtx_);
    lastAlert_[host] = time(nullptr);
  }

  AlertConfig cfg_;
  std::mutex  mtx_;
  std::unordered_map<std::string, time_t> lastAlert_;
};

} // namespace monitor
