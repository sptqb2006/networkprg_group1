#include "../../include/json_helper.hpp"
#include "../../include/metrics_collector.hpp"
#include "../../include/net_framing.hpp"
#include "../../include/protocol.hpp"

#include <netdb.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

using namespace monitor;

static std::atomic<bool> g_running{true};
static void sigHandler(int) { g_running = false; }

// Interruptible sleep — wakes early on shutdown
static void sleepSec(int sec) {
  for (int i = 0; i < sec * 10 && g_running; i++)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

struct AgentConfig {
  std::string serverHost  = "127.0.0.1";
  uint16_t    serverPort  = DEFAULT_PORT;
  std::string hostName;
  int         intervalSec  = 5;
  std::string authToken;
  int         reconnectSec = RECONNECT_INTERVAL_SEC;
  std::string diskPath     = "/";
};

static std::string trim(const std::string &s) {
  auto b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
  return (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
}

static void loadConfigFile(AgentConfig &cfg, const std::string &path) {
  std::ifstream in(path);
  if (!in) return;
  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
    try {
      if      (k == "AUTH_TOKEN")             cfg.authToken = v;
      else if (k == "RECONNECT_INTERVAL_SEC") cfg.reconnectSec = std::max(1, std::stoi(v));
    } catch (...) {}
  }
}

// TCP connect with DNS resolution
static int connectToServer(const std::string &host, uint16_t port) {
  addrinfo hints{}, *res = nullptr;
  hints.ai_family   = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0)
    return -1;
  int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd < 0) { freeaddrinfo(res); return -1; }
  if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
    close(fd); freeaddrinfo(res); return -1;
  }
  freeaddrinfo(res);
  return fd;
}

static void parseServerArg(AgentConfig &cfg, const std::string &sv) {
  auto p = sv.rfind(':');
  if (p == std::string::npos) { cfg.serverHost = sv; return; }
  cfg.serverHost = sv.substr(0, p);
  cfg.serverPort = (uint16_t)std::stoi(sv.substr(p + 1));
}

int main(int argc, char **argv) {
  AgentConfig cfg;
  std::string configPath = "config/agent.conf";

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    try {
      if ((a == "-server") && i+1 < argc) parseServerArg(cfg, argv[++i]);
      else if (a == "-name"     && i+1 < argc) cfg.hostName    = argv[++i];
      else if (a == "-interval" && i+1 < argc) cfg.intervalSec = std::max(1, std::stoi(argv[++i]));
      else if (a == "-config"   && i+1 < argc) configPath      = argv[++i];
      else if (a == "-disk"     && i+1 < argc) cfg.diskPath    = argv[++i];
      else if (a == "-fg") { /* foreground — kept for script compat */ }
    } catch (...) {
      std::cerr << "[agent] bad value for " << a << "\n"; return 1;
    }
  }

  loadConfigFile(cfg, configPath);

  if (cfg.hostName.empty()) {
    char buf[256];
    cfg.hostName = (gethostname(buf, sizeof(buf)) == 0) ? buf : "unknown";
  }

  signal(SIGPIPE, SIG_IGN);
  signal(SIGINT,  sigHandler);
  signal(SIGTERM, sigHandler);

  std::cerr << "[agent] host=" << cfg.hostName
            << " server=" << cfg.serverHost << ":" << cfg.serverPort
            << " interval=" << cfg.intervalSec << "s\n";

  // ── Collector state ────────────────────────────────────────────────
  metrics::AsyncCpuSampler cpuSampler;
  metrics::NetRaw prevNet{};

  // ── Connect → Auth → Send loop (reconnect on failure) ─────────────
  while (g_running) {
    int fd = connectToServer(cfg.serverHost, cfg.serverPort);
    if (fd < 0) {
      std::cerr << "[agent] connect failed, retry in " << cfg.reconnectSec << "s\n";
      sleepSec(cfg.reconnectSec);
      continue;
    }
    std::cerr << "[agent] connected\n";

    // Auth handshake (server rejects if token mismatch)
    if (!cfg.authToken.empty() &&
        !net::sendMsg(fd, "{\"auth\":\"" + json::escapeStr(cfg.authToken) + "\"}")) {
      close(fd); sleepSec(cfg.reconnectSec); continue;
    }

    // Collect → encode → send, repeat every intervalSec
    bool ok = true;
    while (g_running && ok) {
      auto m = metrics::collectWith(cpuSampler, prevNet, cfg.diskPath);
      ok = net::sendMsg(fd, json::encode(
        cfg.hostName, m.cpu, m.ram, m.disk, time(nullptr),
        m.cores, m.netRxKB, m.netTxKB, m.loadAvg, m.procCount));
      if (ok) sleepSec(cfg.intervalSec);
    }

    close(fd);
    if (g_running) {
      std::cerr << "[agent] disconnected, retry in " << cfg.reconnectSec << "s\n";
      sleepSec(cfg.reconnectSec);
    }
  }

  std::cerr << "[agent] stopped\n";
  return 0;
}
