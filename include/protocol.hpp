#pragma once
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace monitor {

static constexpr uint16_t DEFAULT_PORT = 8784;
static constexpr uint16_t DEFAULT_VPORT = 8785;
static constexpr int RECONNECT_INTERVAL_SEC = 5;
static constexpr int DEFAULT_MAX_HISTORY = 60;
static constexpr int DEFAULT_MAX_LOG_ENTRIES = 500;
static constexpr int RECV_TIMEOUT_SEC = 30;
static constexpr int DEFAULT_STALE_SEC   = 30;
static constexpr int DEFAULT_OFFLINE_SEC = 90;

struct MetricPayload {
  std::string host;
  float cpu = 0.0f;
  float ram = 0.0f;
  float disk = 0.0f;
  time_t timestamp = 0;
  std::string ip;
  std::vector<float> cores;
  float netRxKB = 0.0f;
  float netTxKB = 0.0f;
  float loadAvg = 0.0f;
  int procCount = 0;
};

// Severity order (lower = more urgent): ALERT < WARNING < STALE < ONLINE < OFFLINE
enum class HostStatus { ALERT, WARNING, STALE, ONLINE, OFFLINE };

inline const char *statusStr(HostStatus s) {
  switch (s) {
  case HostStatus::ONLINE:   return "OK";
  case HostStatus::WARNING:  return "WARN";
  case HostStatus::ALERT:    return "ALERT";
  case HostStatus::STALE:    return "STALE";
  case HostStatus::OFFLINE:  return "OFFLINE";
  }
  return "UNKNOWN";
}

} // namespace monitor
