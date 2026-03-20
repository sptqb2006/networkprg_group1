# Distributed System Monitor

A **btop++-style** distributed monitoring tool written in C++20 — real-time ncurses dashboard, multi-host, no external dependencies beyond ncurses.

---

## 1. Install Dependencies

```bash
# Debian / Ubuntu
sudo apt install g++ libncurses-dev

# Fedora / RHEL
sudo dnf install gcc-c++ ncurses-devel

# Arch Linux
sudo pacman -S gcc ncurses
```

Requires GCC 10+ or Clang 12+. The **agent** is Linux-only (reads `/proc`). The server and viewer work on any Linux machine with ncurses.

---

## 2. Build

```bash
./build.sh
```

This produces three binaries in the project root: `monitor_server`, `agent`, and `viewer_cli`.

Alternatively, use CMake:
```bash
mkdir build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)
```

---

## 3. Run

```bash
# Terminal 1 — start the server (opens the dashboard)
./monitor_server

# Terminal 2 — connect an agent for each machine you want to monitor
./agent -server 127.0.0.1:8784 -name web-1

# Terminal 3 — (optional) open the interactive viewer
./viewer_cli -server 127.0.0.1:8785
```

> **Terminal size:** minimum 60×15, recommended 120×35 or larger.

### Try the demo instead

```bash
./run_demo_terminals.sh              # starts server + 2 agents + viewer automatically
./run_demo_terminals.sh --agents 4   # more agents
./stop_agents.sh                     # kill background agents when done
```

---

## 4. Dashboard Controls

| Key | Action |
|-----|--------|
| `Q` | Quit |
| `Tab` / `Shift+Tab` | Next / previous host detail |
| `Esc` | Back to overview |
| `↑` `↓` / `PgUp` `PgDn` | Scroll log or history |
| `U` | Cycle themes (6 built-in, requires 256-color terminal) |
| `/` | Open command bar (`/help`, `/viewer <host>`, `/history <host>`) |

---

## 5. Viewer CLI Commands

Press `/` inside the viewer:

| Command | Description |
|---------|-------------|
| `/hosts` | List all hosts with status, CPU, RAM, disk, load |
| `/history <host> [n]` | Last `n` metric samples (default 30) |
| `/log [n]` | Last `n` event log entries (default 50) |
| `q` | Quit |

---

## 6. Host Status

| Symbol | Meaning |
|--------|---------|
| `● OK` | Online, all metrics below threshold |
| `◐ WARN` | A metric is nearing its threshold (≥ 80%) |
| `● ALERT` | A metric exceeds its threshold |
| `◌ STALE` | No data received for `STALE_SEC` seconds (default 30s) |
| `○ OFFLINE` | No data received for `OFFLINE_SEC` seconds (default 90s) |

---

## 7. Configuration

### Alert thresholds — `config/thresholds.conf`

```ini
CPU=80
RAM=90
DISK=85

# Per-host overrides
web-1.cpu=85
db-server.ram=95
```

### Server settings — `config/server.conf`

Key options (see the file for all settings):

```ini
MAX_AGENTS_PER_IP=2
STALE_SEC=30
OFFLINE_SEC=90
#AUTH_TOKEN=changeme        # uncomment to require auth
#ALERT_WEBHOOK_URL=https://hooks.slack.com/...
HTTP_API_PORT=8786          # 0 = disabled
```

### Agent settings — `config/agent.conf`

```ini
MAX_CONNECT_RETRIES=12      # 0 = retry forever
RECONNECT_INTERVAL_SEC=5
#AUTH_TOKEN=changeme        # must match server if set
```

---

## 8. Common Flags

**Server:**
```bash
./monitor_server -port 8784 -vport 8785 -config config/thresholds.conf -server-config config/server.conf
```

**Agent:**
```bash
./agent -server <ip>:8784 -name <hostname> -interval 2 -disk / -fg
```
`-fg` keeps the agent in the foreground (default is daemon mode).

---

## Going Further

- **systemd:** unit files are in `deploy/` for running server and agents as services.
- **Docker:** a multi-stage `Dockerfile` is included; use `--pid host` for the agent container so it can read `/proc`.
- **HTTP API / Prometheus:** enable with `HTTP_API_PORT=8786` in `server.conf` — endpoints at `/api/hosts`, `/api/history/<host>`, `/metrics`, `/healthz`.
- **Security:** for internet-facing use, set `AUTH_TOKEN` in both config files and put the ports behind a VPN or SSH tunnel (TLS is not built in).
