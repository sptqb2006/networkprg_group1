<div align="center">
  <h1>🚀 Distributed System Monitor</h1>
  <p><b>A high-performance, real-time distributed telemetry and alerting system built in modern C++20.</b></p>
  <p>
    <i>Designed for scale, built for speed, tailored for absolute observability.</i>
  </p>
</div>

---

## 🌟 Project Overview

**Distributed System Monitor** is a lightweight yet immensely powerful telemetry solution designed to track CPU, Memory, and Disk usage across a vast fleet of servers. 

Rather than relying on resource-heavy web dashboards, this project embraces the hacker ethos: **a blazing-fast, pure TCP-socket-based architecture** featuring a split-screen `ncurses` command-line viewer. With millisecond-latency broadcast alerts, intelligent stale-node detection, and per-host custom thresholds, this system proves that enterprise-grade observability doesn't need to be bloated.

### 🏗 Architecture Flow

```text
 ┌───────────────┐                             ┌─────────────────────────┐
 │   Agent #1    ├──────.                      │  Viewer CLI (Admin)     │
 │ (web-1 nodes) │      │                      │ ┌─────────────────────┐ │
 └───────────────┘      │    JSON / TCP        │ │ > /history web-1 10 │ │
 ┌───────────────┐      │   (Port 8784)        │ │ [..] OK             │ │
 │   Agent #2    ├──────)─────▶ [ MONITOR SERVER ]─┼─────────────────────┼─│
 │ (db cluster)  │      │                      │ │ ▼ LIVE ALERTS       │ │
 └───────────────┘      │    Broadcast TCP     │ │ [!] CPU=95%(>80%)   │ │
 ┌───────────────┐      │   (Port 8785)        │ └─────────────────────┘ │
 │   Agent #N    ├──────'                      └─────────────────────────┘
 │ (edge nodes)  │                                          ▲
 └───────────────┘                                          │
                                                       More Viewers...
```

---

## 🔥 Enterprise-Grade Features

*   ⚡ **Ultra-Lightweight Agents:** Written in pure C++ polling `/proc`. Near-zero CPU overhead. Auto-reconnects seamlessly if the network drops.
*   📡 **Event-Driven Alert Broadcast:** Detection is instant. When a host breaches its threshold, the server immediately pushes an `ALERT_EVT` frame to all connected viewers without polling delays.
*   🎛 **Dual-Panel Ncurses Dashboard:** The `viewer_cli` features a gorgeous split-screen UI:
    *   **Top:** Interactive command prompt (`/hosts`, `/history`, `/log`) with command history (↑/↓).
    *   **Bottom:** Real-time scrolling feed of incoming alerts.
*   ⏱ **Smart State Machine:** Nodes transition smoothly: `ONLINE` ➔ `STALE` (high-latency/delayed packets) ➔ `OFFLINE` (dead).
*   🎯 **Granular Threshold Configuration:** Global limits (e.g., CPU=80%) with per-host overrides (e.g., `web-1.cpu=85`, `db.ram=95`) instantly loaded without recompiling.
*   🕒 **Time-Series Engine:** Queries operate on absolute time windows (e.g., "Give me the last 15 minutes of logs") instead of simple flat counts.

---

## 🛠 Getting Started

### 1. Build the System
Only requires a modern C++ compiler (`g++` / `clang++`), `make`, and `ncurses-dev`.
```bash
./build.sh
```
*(This complies all three binaries: `agent`, `monitor_server`, and `viewer_cli`)*

### 2. Configure Thresholds
Edit `config/thresholds.conf` to set your system limits:
```ini
CPU=80
RAM=90
DISK=85

# Override for specific critical hosts
database-primary.ram=95
render-worker.cpu=98
```

### 3. Launch the Core Infrastructure
Start the central nervous system:
```bash
./monitor_server -port 8784 -vport 8785 -config config/thresholds.conf
```

Deploy agents on your target machines (or locally for testing):
```bash
# Agent 1 will push metrics every 2 seconds
./agent -server 127.0.0.1:8784 -interval 2 -name backend-api-01
```

### 4. Connect the Admin Viewer
Gain absolute visibility into your cluster:
```bash
./viewer_cli -server 127.0.0.1:8785
```

---

## 💻 Viewer CLI Command Reference

Once inside the `viewer_cli`, type `/` to enter command mode:

| Command | Description |
|---|---|
| `/hosts` | Live matrix of all registered agents (Host, Status, Metrics, Last Seen). |
| `/history <host> [mins]` | Retrieves historical telemetry for `<host>` over the last `[mins]` minutes. |
| `/log [mins]` | Dumps the cluster-wide event & alert log for the past `[mins]` minutes. |
| `/clear` | Clears both the command output and the live alert panels. |
| `q` or `Esc` | Quit / Cancel command. |

**Keyboard Shortcuts:**
*   **`Up / Down`**: Scroll through command output (Normal mode) OR browse command history (Command mode).
*   **`PgUp / PgDn`**: Scroll through the bottom Live Alerts panel.

---

## 🚀 Potential & Future Scaling

This project is architected as the foundation for a massive-scale observability pipeline. Its pure TCP/JSON nature means it can be expanded boundlessly:
1.  **Prometheus/Grafana Bridge:** The server can easily expose a `/metrics` HTTP endpoint to allow scraping by Prometheus.
2.  **WebHook Integration:** Add Slack/Discord/PagerDuty webhook POST requests when a host hits `OFFLINE` or `ALERT`.
3.  **Encrypted Telemetry:** Wrap the TCP sockets in TLS (OpenSSL) for zero-trust cross-datacenter monitoring.
4.  **Database Persistence:** Route historical metric streams directly into TimescaleDB or InfluxDB for infinite data retention.

---
<div align="center">
  <i>Developed with ❤️ by group1_H@ckErUSTH</i>
</div>
