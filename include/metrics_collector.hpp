#pragma once
// Metrics collector for Linux — reads /proc/stat, /proc/meminfo, statvfs(),
// /proc/net/dev, /proc/loadavg. Async CPU sampling via background thread.
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/statvfs.h>
#include <thread>
#include <vector>

namespace monitor::metrics {

struct Sample {
  float cpu = 0.0f;
  float ram = 0.0f;
  float disk = 0.0f;
  std::vector<float> cores;
  float netRxKB = 0.0f;
  float netTxKB = 0.0f;
  float loadAvg = 0.0f;
  int procCount = 0;
};

struct CpuState {
  unsigned long long user=0, nice=0, sys=0, idle=0,
                     iowait=0, irq=0, softirq=0, steal=0;
};

inline CpuState parseCpuLine(const std::string &line) {
  CpuState s;
  const char *p = line.c_str();
  while (*p && *p != ' ') p++;
  sscanf(p, " %llu %llu %llu %llu %llu %llu %llu %llu",
         &s.user, &s.nice, &s.sys, &s.idle,
         &s.iowait, &s.irq, &s.softirq, &s.steal);
  return s;
}

inline std::vector<CpuState> readAllCpuStates() {
  std::vector<CpuState> states;
  std::ifstream f("/proc/stat");
  std::string line;
  while (std::getline(f, line))
    if (line.compare(0, 3, "cpu") == 0) states.push_back(parseCpuLine(line));
    else break;
  return states;
}

inline float cpuPercent(const CpuState &p, const CpuState &c) {
  auto pi = p.idle + p.iowait, ci = c.idle + c.iowait;
  auto pt = p.user+p.nice+p.sys+pi+p.irq+p.softirq+p.steal;
  auto ct = c.user+c.nice+c.sys+ci+c.irq+c.softirq+c.steal;
  auto dT = ct - pt, dI = ci - pi;
  if (dT == 0) return 0.0f;
  return 100.0f * (1.0f - (float)dI / (float)dT);
}

// Background thread measures CPU every ~900ms; read() returns cached result.
class AsyncCpuSampler {
public:
  AsyncCpuSampler() {
    prev_ = readAllCpuStates();
    running_ = true;
    thread_ = std::thread([this]() { loop(); });
  }
  ~AsyncCpuSampler() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
  }

  AsyncCpuSampler(const AsyncCpuSampler &) = delete;
  AsyncCpuSampler &operator=(const AsyncCpuSampler &) = delete;
  AsyncCpuSampler(AsyncCpuSampler &&) = delete;
  AsyncCpuSampler &operator=(AsyncCpuSampler &&) = delete;

  std::pair<float, std::vector<float>> read() {
    std::lock_guard<std::mutex> lk(mtx_);
    return {totalCpu_, coreCpu_};
  }

private:
  void loop() {
    while (running_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(900));
      auto cur = readAllCpuStates();
      std::lock_guard<std::mutex> lk(mtx_);
      if (cur.empty() || prev_.empty()) { prev_ = cur; continue; }
      totalCpu_ = cpuPercent(prev_[0], cur[0]);
      coreCpu_.clear();
      size_t n = std::min(prev_.size(), cur.size());
      for (size_t i = 1; i < n; i++)
        coreCpu_.push_back(cpuPercent(prev_[i], cur[i]));
      prev_ = cur;
    }
  }

  std::atomic<bool> running_{false};
  std::thread thread_;
  std::mutex mtx_;
  std::vector<CpuState> prev_;
  float totalCpu_ = 0.0f;
  std::vector<float> coreCpu_;
};

inline float ramPercent() {
  std::ifstream f("/proc/meminfo");
  unsigned long long total=0, free_=0, buffers=0, cached=0, sreclaimable=0, shmem=0;
  std::string line;
  while (std::getline(f, line)) {
    unsigned long long v; char name[64];
    if (sscanf(line.c_str(), "%63s %llu", name, &v) == 2) {
      std::string n(name);
      if      (n=="MemTotal:")      total=v;
      else if (n=="MemFree:")       free_=v;
      else if (n=="Buffers:")       buffers=v;
      else if (n=="Cached:")        cached=v;
      else if (n=="SReclaimable:")  sreclaimable=v;
      else if (n=="Shmem:")         shmem=v;
    }
  }
  if (total == 0) return 0.0f;
  unsigned long long used = total - free_ - buffers - cached - sreclaimable + shmem;
  return 100.0f * (float)used / (float)total;
}

inline float diskPercent(const std::string &path = "/") {
  struct statvfs st;
  if (statvfs(path.c_str(), &st) != 0) return 0.0f;
  unsigned long long total = st.f_blocks * st.f_frsize;
  unsigned long long avail = st.f_bavail * st.f_frsize;
  if (total == 0) return 0.0f;
  return 100.0f * (1.0f - (float)avail / (float)total);
}

struct NetRaw { unsigned long long rx=0, tx=0; };

inline NetRaw readNetRaw() {
  NetRaw r;
  std::ifstream f("/proc/net/dev");
  std::string line;
  std::getline(f, line); std::getline(f, line); // skip headers
  while (std::getline(f, line)) {
    auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string iface = line.substr(0, colon);
    auto b = iface.find_first_not_of(" \t");
    if (b != std::string::npos) iface = iface.substr(b);
    if (iface == "lo") continue;
    // Fields: rx_bytes rx_packets(x6 ignored) tx_bytes
    unsigned long long rx,t1,t2,t3,t4,t5,t6,t7,tx;
    if (sscanf(line.c_str()+colon+1,
               " %llu %llu %llu %llu %llu %llu %llu %llu %llu",
               &rx,&t1,&t2,&t3,&t4,&t5,&t6,&t7,&tx) >= 9) {
      r.rx += rx;
      r.tx += tx;
    }
  }
  return r;
}

struct SysInfo { float loadAvg=0; int procCount=0; };

inline SysInfo readSysInfo() {
  SysInfo s;
  std::ifstream f("/proc/loadavg");
  if (f) {
    // Format: "1.02 0.88 0.77 running/total pid"
    int procs=0;
    char sep;
    f >> s.loadAvg;
    float la5, la15;
    f >> la5 >> la15;
    f >> procs >> sep >> s.procCount;
  }
  return s;
}

inline Sample collectWith(AsyncCpuSampler &cpuSampler,
                          NetRaw &prevNet,
                          const std::string &diskPath = "/") {
  Sample s;
  auto [total, cores] = cpuSampler.read();
  s.cpu = total;
  s.cores = cores;
  s.ram = ramPercent();
  s.disk = diskPercent(diskPath);

  NetRaw cur = readNetRaw();
  if (prevNet.rx == 0 && prevNet.tx == 0)
    prevNet = cur;
  float deltaRx = (float)(cur.rx >= prevNet.rx ? cur.rx - prevNet.rx : 0);
  float deltaTx = (float)(cur.tx >= prevNet.tx ? cur.tx - prevNet.tx : 0);
  s.netRxKB = deltaRx / 1024.0f;
  s.netTxKB = deltaTx / 1024.0f;
  prevNet = cur;

  auto si = readSysInfo();
  s.loadAvg = si.loadAvg;
  s.procCount = si.procCount;

  return s;
}

} // namespace monitor::metrics
