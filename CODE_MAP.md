# Code Map -- Distributed System Monitor

This file provides a quick reference to locate every core function and structure
across the project. Use it as a navigation guide when reading or modifying code.

## File Overview

| File | Role |
|------|------|
| `src/agent/agent.cpp` | Lightweight agent that reads OS metrics and pushes to server |
| `src/server/monitor_server.cpp` | Central server: accepts agents, stores metrics, broadcasts |
| `src/viewer/viewer_cli.cpp` | Interactive ncurses viewer with dual-panel UI |
| `include/metrics_store.hpp` | In-memory metrics database, history, logs |
| `include/thresholds.hpp` | Threshold config loader (global + per-host overrides) |
| `include/ansi_viewer.hpp` | ANSI escape renderer for nc-based remote viewers |
| `include/protocol.hpp` | JSON parser, message framing, metric payload struct |
| `config/thresholds.conf` | Alert threshold configuration |
| `config/server.conf` | Server runtime configuration |
| `config/agent.conf` | Agent runtime configuration |

---

## src/agent/agent.cpp

| Line | Symbol | Description |
|------|--------|-------------|
| 20 | `g_running` | Global atomic flag for graceful shutdown |
| 21 | `sigHandler()` | SIGINT/SIGTERM handler |
| 24 | `sleepSec()` | Interruptible sleep (checks g_running) |
| 29 | `AgentConfig` | Struct: server host, port, interval, name, auth token |
| 39 | `trim()` | String whitespace trimmer |
| 44 | `loadConfigFile()` | Reads agent.conf key=value pairs |
| 62 | `connectToServer()` | TCP connect to monitor server |
| 77 | `parseServerArg()` | Parses `-server host:port` argument |
| 84 | `main()` | Entry point: parse args, connect loop, send metrics |

---

## src/server/monitor_server.cpp

### Global State (line 36-52)
| Line | Symbol | Description |
|------|--------|-------------|
| 36 | `g_store` | MetricsStore instance (central database) |
| 37 | `g_thresh` | Loaded thresholds from config |
| 38 | `g_stats` | Server statistics counters |
| 39 | `g_running` | Atomic shutdown flag |
| 42 | `g_fdMtx / g_fdHost / g_fdIP` | FD-to-hostname/IP mapping (mutex protected) |
| 46 | `g_viewerPushMtx / g_viewerPushFds` | Viewer push socket list (for broadcast) |
| 52 | `g_alerter` | Webhook alerter instance pointer |

### Configuration (line 54-71)
| Line | Symbol | Description |
|------|--------|-------------|
| 54 | `ServerConfig` | Struct: ports, auth, stale/offline timeouts, webhook, etc. |
| 71 | `g_cfg` | Global server config instance |

### Utility Functions (line 73-131)
| Line | Symbol | Description |
|------|--------|-------------|
| 73 | `trim()` | String whitespace trimmer |
| 78 | `loadServerConfig()` | Reads server.conf, parses MAX_HISTORY_SAMPLES, MAX_LOG_ENTRIES |
| 112 | `validateStatePath()` | Ensures state file directory exists |
| 121 | `spawnThread()` | Thread pool: creates and tracks threads |
| 126 | `joinAllThreads()` | Joins all spawned threads on shutdown |

### Core Network Handlers (line 133-262)
| Line | Symbol | Description |
|------|--------|-------------|
| 133 | `handleClient()` | Per-agent TCP handler: parse JSON metrics, call upsert(), broadcast ALERT_EVT |
| 199 | *(inside handleClient)* | ALERT_EVT broadcast logic: builds JSON, sends to all viewer FDs |

### Background Loops (line 263-326)
| Line | Symbol | Description |
|------|--------|-------------|
| 263 | `persistLoop()` | Periodic state backup to disk |
| 273 | `staleCheckerLoop()` | Marks hosts STALE/OFFLINE based on timeout |
| 283 | `renderLoop()` | Renders dashboard frame, broadcasts to viewer FDs every 2s |

### Viewer & Accept Loops (line 327-441)
| Line | Symbol | Description |
|------|--------|-------------|
| 327 | `viewerHandler()` | Handles CMD queries from viewer (hosts/history/log) |
| 385 | `createListenSocket()` | Creates TCP listen socket with SO_REUSEADDR |
| 400 | `acceptLoop()` | Accepts agent connections, spawns handleClient threads |
| 430 | `viewerAcceptLoop()` | Accepts viewer connections, registers push FDs or CMD handler |

### Entry Point (line 444)
| Line | Symbol | Description |
|------|--------|-------------|
| 444 | `main()` | Parse CLI args, load configs, start all loops |

---

## src/viewer/viewer_cli.cpp

### Utility Functions (line 24-121)
| Line | Symbol | Description |
|------|--------|-------------|
| 24 | `trim()` | String whitespace trimmer |
| 30 | `connectTo()` | TCP connect helper |
| 45 | `queryServer()` | Send CMD request, receive JSON response |
| 64 | `splitJsonObjects()` | Parse JSON array into individual objects |
| 83 | `jstr()` | Extract string value from JSON object |
| 93 | `jnum()` | Extract numeric value from JSON object |
| 103 | `nowStr()` | Format time_t as HH:MM:SS |
| 109 | `fmtTs()` | Format timestamp string |
| 116 | `statusColor()` | Map status string to ncurses color pair |

### Ncurses Setup (line 125-155)
| Line | Symbol | Description |
|------|--------|-------------|
| 125 | `ColorPairs` | Enum: CP_DEFAULT, CP_GREEN, CP_RED, ... CP_SEP |
| 132 | `initViewerColors()` | Initialize ncurses color pairs (256-color aware) |

### Broadcast Receiver (line 157-240)
| Line | Symbol | Description |
|------|--------|-------------|
| 157 | `g_alertMtx / g_alertLines` | Mutex-protected alert line buffer |
| 161 | `g_bcastConnected / g_running / g_needsRedraw` | Atomic state flags |
| 166 | `formatAlertEvt()` | Parse ALERT_EVT JSON into display string |
| 189 | `broadcastReceiver()` | Background thread: TCP connect, recv frames, parse ALERT_EVT |

### Draw Helper (line 242)
| Line | Symbol | Description |
|------|--------|-------------|
| 242 | `wDrawLine()` | Safe ncurses line draw with column limit |

### Main Loop (line 247)
| Line | Symbol | Description |
|------|--------|-------------|
| 247 | `main()` | Entry point: parse args, init ncurses, spawn broadcast thread |
| 315 | *(resize handler)* | Destroy/recreate 6 WINDOW* for vertical split layout |
| 353 | *(input handler)* | Key dispatch: normal mode (scroll) vs command mode (history) |
| 405 | *(command executor)* | Execute /hosts, /history, /log, /clear, /help |
| 559 | *(redraw section)* | Draw all 6 windows: hdr, cmdWin, sepWin, alertWin, stat, input |

---

## include/metrics_store.hpp

### Data Structures (line 60-101)
| Line | Symbol | Description |
|------|--------|-------------|
| 60 | `HistorySample` | Struct: timestamp + CPU/RAM/Disk/Load/Net metrics |
| 67 | `HostState` | Struct: host info, status, history deque, push() method |
| 93 | `LogEventType` | Enum: CONNECT, METRIC, ALERT, DISCONNECT, STALE |
| 95 | `LogEvent` | Struct: timestamp, host, IP, metrics, type, detail |

### MetricsStore Class (line 103)
| Line | Method | Description |
|------|--------|-------------|
| 105 | `upsert()` | Insert/update host metrics, compute ALERT/WARN status |
| 140 | `setOnline()` | Mark host as ONLINE with IP and FD |
| 149 | `setOffline()` | Mark host as OFFLINE, log DISCONNECT event |
| 162 | `checkStale()` | Scan all hosts, mark STALE/OFFLINE by timeout |
| 188 | `hostsJson()` | Serialize all hosts to JSON array |
| 226 | `historyJson()` | Time-filtered history for a host (last N minutes) |
| 254 | `logJson()` | Time-filtered event log (last N minutes) |
| 299 | `saveState() / loadState()` | Persist/restore state to/from file |
| 420 | `setMaxEntries()` | Dynamically set history/log size limits |

---

## include/thresholds.hpp

| Line | Symbol | Description |
|------|--------|-------------|
| 12 | `Thresholds` | Struct: global cpu/ram/disk + perHost map |
| 18 | `getCPU() / getRAM() / getDisk()` | Per-host threshold lookup with fallback |
| 32 | `toLower()` | Case-insensitive key normalization |
| 38 | `loadThresholds()` | Parse thresholds.conf (global + host.metric=value) |

---

## include/ansi_viewer.hpp

| Line | Symbol | Description |
|------|--------|-------------|
| 12 | ANSI constants | RST, BOLD, ARED, AGRN, AYEL, etc. |
| 24 | `fmtTime()` | Format time for ANSI display |
| 31 | `pctAnsi()` | Choose color based on metric vs threshold |
| 43 | `makeBar()` | ASCII progress bar renderer |
| 53 | `padRight()` | Right-pad string to width |
| 58 | `statusSymbol()` | Host status to colored symbol string |
| 69 | `renderFrame()` | Render full dashboard frame as ANSI string |
