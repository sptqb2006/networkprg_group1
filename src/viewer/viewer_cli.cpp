#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef HAVE_NCURSESW
#  include <ncursesw/curses.h>
#else
#  include <curses.h>
#endif

static std::string trim(const std::string &s) {
  auto b = s.find_first_not_of(" \t\r\n");
  auto e = s.find_last_not_of(" \t\r\n");
  return (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
}

static int connectTo(const std::string &host, uint16_t port) {
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_INET;
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

static std::string queryServer(const std::string &serverHost, uint16_t serverPort,
                               const std::string &cmd) {
  int fd = connectTo(serverHost, serverPort);
  if (fd < 0) return "{\"error\":\"cannot connect\"}";

  std::string req = "CMD " + cmd + "\n";
  (void)send(fd, req.c_str(), req.size(), 0);

  std::string resp;
  char buf[4096];
  struct timeval tv{}; tv.tv_sec = 5;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  int n;
  while ((n = recv(fd, buf, sizeof(buf), 0)) > 0)
    resp.append(buf, n);
  close(fd);
  while (!resp.empty() && (resp.back()=='\n'||resp.back()=='\r'))
    resp.pop_back();
  return resp;
}

static std::vector<std::string> splitJsonObjects(const std::string &json) {
  std::vector<std::string> out;
  if (json.size() < 2 || json.front() != '[') return out;
  int depth = 0;
  size_t start = std::string::npos;
  for (size_t i = 0; i < json.size(); i++) {
    char c = json[i];
    if (c == '{') { if (depth == 0) start = i; depth++; }
    else if (c == '}') {
      depth--;
      if (depth == 0 && start != std::string::npos) {
        out.push_back(json.substr(start, i - start + 1));
        start = std::string::npos;
      }
    }
  }
  return out;
}

static std::string jstr(const std::string &obj, const std::string &key) {
  std::string pat = "\"" + key + "\":\"";
  auto pos = obj.find(pat);
  if (pos == std::string::npos) return "";
  pos += pat.size();
  auto end = obj.find('"', pos);
  if (end == std::string::npos) return "";
  return obj.substr(pos, end - pos);
}

static std::string jnum(const std::string &obj, const std::string &key) {
  std::string pat = "\"" + key + "\":";
  auto pos = obj.find(pat);
  if (pos == std::string::npos) return "-";
  pos += pat.size();
  auto end = obj.find_first_of(",}", pos);
  if (end == std::string::npos) return "-";
  return obj.substr(pos, end - pos);
}

static std::string nowStr(time_t t) {
  char b[32];
  strftime(b, sizeof(b), "%H:%M:%S", localtime(&t));
  return b;
}

static std::string fmtTs(const std::string &tsStr) {
  try {
    time_t t = (time_t)std::stoll(tsStr);
    return nowStr(t);
  } catch(...) { return tsStr; }
}

static void initViewerColors() {
  start_color();
  use_default_colors();
  init_pair(1, COLOR_WHITE,   -1);      // default text
  init_pair(2, COLOR_GREEN,   -1);      // OK status
  init_pair(3, COLOR_RED,     -1);      // ALERT / alert text
  init_pair(4, COLOR_YELLOW,  -1);      // WARN
  init_pair(5, COLOR_CYAN,    -1);      // info / cmd
  init_pair(6, COLOR_MAGENTA, -1);      // STALE
  init_pair(7, COLOR_WHITE,   COLOR_BLUE);  // header bar
  init_pair(8, COLOR_RED,     COLOR_BLACK); // alert panel header
  init_pair(9, COLOR_CYAN,    COLOR_BLACK); // separator
  if (COLORS >= 256) {
    init_pair(2,  46,  -1);
    init_pair(3, 196,  -1);
    init_pair(4, 202,  -1);
    init_pair(5, 153,  -1);
    init_pair(6, 141,  -1);
    init_pair(7,  15,  17);
    init_pair(8, 196, 234);
    init_pair(9, 153, 234);
  }
}

static int statusColor(const std::string &st) {
  if (st == "ALERT")   return 3;
  if (st == "WARN")    return 4;
  if (st == "STALE")   return 6;
  if (st == "OK")      return 2;
  return 1;
}

// ── Broadcast receiver globals ──────────────────────────────────────────────
static std::mutex g_alertMtx;
static std::vector<std::string> g_alertLines;  // formatted alert strings
static const int MAX_ALERT_LINES = 200;

static std::atomic<bool> g_bcastConnected{false};
static std::atomic<bool> g_running{true};
static std::atomic<int>  g_alertCount{0};

// Parse an ALERT_EVT line and return a formatted display string
static std::string formatAlertEvt(const std::string &json) {
  std::string host = jstr(json, "host");
  std::string ts = fmtTs(jnum(json, "timestamp"));

  std::string detail;
  // Check each metric
  std::string cpuVal = jnum(json, "cpu");
  std::string cpuTh  = jnum(json, "cpu_threshold");
  if (cpuVal != "-" && cpuTh != "-")
    detail += "CPU=" + cpuVal + "%(>" + cpuTh + "%) ";

  std::string ramVal = jnum(json, "ram");
  std::string ramTh  = jnum(json, "ram_threshold");
  if (ramVal != "-" && ramTh != "-")
    detail += "RAM=" + ramVal + "%(>" + ramTh + "%) ";

  std::string diskVal = jnum(json, "disk");
  std::string diskTh  = jnum(json, "disk_threshold");
  if (diskVal != "-" && diskTh != "-")
    detail += "DISK=" + diskVal + "%(>" + diskTh + "%) ";

  char buf[256];
  snprintf(buf, sizeof(buf), " %s  %-14s  %s",
           ts.c_str(), host.substr(0, 14).c_str(), detail.c_str());
  return buf;
}

static void broadcastReceiver(const std::string &host, uint16_t port) {
  while (g_running) {
    int fd = connectTo(host, port);
    if (fd < 0) {
      g_bcastConnected = false;
      for (int i = 0; i < 20 && g_running; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    g_bcastConnected = true;

    struct timeval tv{}; tv.tv_sec = 5;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string buffer;
    char buf[8192];
    while (g_running) {
      int n = recv(fd, buf, sizeof(buf), 0);
      if (n <= 0) {
        if (n == 0) break; // server closed
        continue;          // timeout — retry
      }
      buffer.append(buf, n);

      // Process complete lines
      size_t pos;
      while ((pos = buffer.find('\n')) != std::string::npos) {
        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);

        // Check if this is an ALERT_EVT
        if (line.rfind("ALERT_EVT ", 0) == 0) {
          std::string json = line.substr(10); // skip "ALERT_EVT "
          std::string formatted = formatAlertEvt(json);
          {
            std::lock_guard<std::mutex> lk(g_alertMtx);
            g_alertLines.push_back(formatted);
            if ((int)g_alertLines.size() > MAX_ALERT_LINES)
              g_alertLines.erase(g_alertLines.begin());
          }
          g_alertCount++;
        }
        // Dashboard frames (starting with \033[2J) are silently consumed
      }
      // Clear stale data (dashboard frames without newlines)
      if (buffer.size() > 65536) buffer.clear();
    }
    close(fd);
    g_bcastConnected = false;
  }
}

int main(int argc, char **argv) {
  std::string host = "127.0.0.1";
  uint16_t port = 8785;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-server" && i + 1 < argc) {
      std::string sv = argv[++i];
      auto p = sv.rfind(':');
      if (p == std::string::npos) host = sv;
      else {
        host = sv.substr(0, p);
        try { port = (uint16_t)std::stoi(sv.substr(p + 1)); }
        catch(...) { std::cerr << "Bad port value\n"; return 1; }
      }
    }
  }

  initscr(); noecho(); cbreak();
  keypad(stdscr, TRUE); nodelay(stdscr, TRUE); curs_set(0);
  initViewerColors();

  // Start broadcast receiver thread
  std::thread bcastThread(broadcastReceiver, host, port);
  bcastThread.detach();

  std::vector<std::string> outputLines;
  std::string lastCmd;
  bool cmdMode = false;
  std::string cmd;

  // Command history
  std::vector<std::string> cmdHistory;
  int histIdx = -1;

  // Alert scroll offset (0 = show latest)
  int alertScroll = 0;

  outputLines = {
    "viewer_cli — Monitor Query Client",
    "",
    "Commands:",
    "  /hosts              - List all hosts with current status",
    "  /history <host> [n] - Metric history for last n mins (default 30)",
    "  /log [n]            - Event log for last n mins (default 50)",
    "  /help               - Show this help",
    "  /clear              - Clear output / clear alerts",
    "",
    "Press '/' to enter a command, Esc to cancel, q to quit.",
    "Up/Down arrows recall command history.",
  };

  while (true) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    erase();

    // ── Layout calculation ──────────────────────────────────────────────────
    // Row 0:         Header bar
    // Row 1..sepRow-1: Command output panel
    // Row sepRow:    Separator line (Alert panel title)
    // Row sepRow+1..rows-3: Alert panel
    // Row rows-2:   Status bar
    // Row rows-1:   Command input line

    // Alert panel gets roughly 1/3 of usable space, minimum 4 rows
    int usableRows = rows - 4; // header + sep + status + cmdline
    int alertPanelH = std::max(4, usableRows / 3);
    int cmdPanelH   = usableRows - alertPanelH;
    if (cmdPanelH < 3) { cmdPanelH = 3; alertPanelH = usableRows - cmdPanelH; }

    int sepRow = 1 + cmdPanelH;

    // ── Header bar ──────────────────────────────────────────────────────────
    attron(COLOR_PAIR(7) | A_BOLD);
    for (int i = 0; i < cols; i++) mvaddch(0, i, ' ');
    char hdrBuf[256];
    snprintf(hdrBuf, sizeof(hdrBuf),
             " ◈ MONITOR VIEWER  server=%s:%d  group1_H@ckErUSTH",
             host.c_str(), port);
    mvaddstr(0, 1, hdrBuf);
    mvaddstr(0, cols - 9, nowStr(time(nullptr)).c_str());
    attroff(COLOR_PAIR(7) | A_BOLD);

    // ── Command output panel (top) ──────────────────────────────────────────
    int startLine = 0;
    if ((int)outputLines.size() > cmdPanelH)
      startLine = (int)outputLines.size() - cmdPanelH;
    for (int i = 0; i < cmdPanelH && startLine + i < (int)outputLines.size(); i++) {
      const std::string &ln = outputLines[startLine + i];
      mvaddnstr(1 + i, 0, ln.c_str(), cols - 1);
    }

    // ── Separator line / Alert panel header ─────────────────────────────────
    attron(COLOR_PAIR(8) | A_BOLD);
    for (int i = 0; i < cols; i++) mvaddch(sepRow, i, ' ');
    {
      int cnt = g_alertCount.load();
      char sepBuf[128];
      snprintf(sepBuf, sizeof(sepBuf),
               " ▼ LIVE ALERTS (%d total) ", cnt);
      mvaddstr(sepRow, 1, sepBuf);

      // Show TIME / HOST / DETAIL column headers on right
      const char *colHdr = "TIME    HOST            DETAIL";
      int hdrLen = (int)strlen(colHdr);
      if (cols - hdrLen - 2 > 30)
        mvaddstr(sepRow, cols - hdrLen - 1, colHdr);
    }
    attroff(COLOR_PAIR(8) | A_BOLD);

    // ── Alert panel (bottom) ────────────────────────────────────────────────
    {
      std::lock_guard<std::mutex> lk(g_alertMtx);
      int totalAlerts = (int)g_alertLines.size();
      int displayRows = alertPanelH;
      int alertStart = sepRow + 1;

      if (totalAlerts == 0) {
        attron(COLOR_PAIR(5) | A_DIM);
        mvaddstr(alertStart, 2, "(no alerts yet — waiting for server broadcast...)");
        attroff(COLOR_PAIR(5) | A_DIM);
      } else {
        // Scroll: alertScroll=0 means show latest at bottom
        int firstIdx = totalAlerts - displayRows - alertScroll;
        if (firstIdx < 0) firstIdx = 0;

        for (int i = 0; i < displayRows; i++) {
          int idx = firstIdx + i;
          if (idx >= totalAlerts) break;
          attron(COLOR_PAIR(3)); // red text
          mvaddstr(alertStart + i, 0, "  !");
          attroff(COLOR_PAIR(3));
          attron(COLOR_PAIR(3) | A_BOLD);
          mvaddnstr(alertStart + i, 3, g_alertLines[idx].c_str(), cols - 4);
          attroff(COLOR_PAIR(3) | A_BOLD);
        }
      }
    }

    // ── Status bar ──────────────────────────────────────────────────────────
    attron(COLOR_PAIR(5));
    mvhline(rows - 2, 0, ' ', cols);
    if (!lastCmd.empty()) {
      char sb[256]; snprintf(sb, sizeof(sb), " Last: /%s ", lastCmd.c_str());
      mvaddstr(rows - 2, 0, sb);
    }
    {
      std::string bstat = g_bcastConnected.load() ? "[LIVE]" : "[OFFLINE]";
      mvaddstr(rows - 2, cols - (int)bstat.size() - 1, bstat.c_str());
    }
    attroff(COLOR_PAIR(5));

    // ── Command input line ──────────────────────────────────────────────────
    attron(A_BOLD);
    mvhline(rows - 1, 0, ' ', cols);
    if (cmdMode) {
      attron(COLOR_PAIR(5) | A_BOLD);
      mvaddstr(rows - 1, 0, " > /");
      addstr(cmd.c_str()); addstr("_");
      attroff(COLOR_PAIR(5) | A_BOLD);
    } else {
      attron(COLOR_PAIR(1));
      mvaddstr(rows - 1, 0, " [/] command  [q] quit  [↑↓] history  [PgUp/Dn] scroll alerts");
      attroff(COLOR_PAIR(1));
    }
    attroff(A_BOLD);

    refresh();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    int ch = getch();
    if (ch == 'q' || ch == 'Q') break;

    // ── Alert scroll (when not in command mode) ─────────────────────────────
    if (!cmdMode) {
      if (ch == KEY_PPAGE) { // PgUp — scroll alerts up
        alertScroll += 3;
        std::lock_guard<std::mutex> lk(g_alertMtx);
        int maxScroll = std::max(0, (int)g_alertLines.size() - alertPanelH);
        if (alertScroll > maxScroll) alertScroll = maxScroll;
        continue;
      }
      if (ch == KEY_NPAGE) { // PgDn — scroll alerts down
        alertScroll -= 3;
        if (alertScroll < 0) alertScroll = 0;
        continue;
      }
      if (ch == '/') { cmdMode = true; cmd.clear(); histIdx = -1; }
      continue;
    }

    // ── Command mode input ──────────────────────────────────────────────────
    if (ch == 27) { cmdMode = false; cmd.clear(); histIdx = -1; continue; }
    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
      if (!cmd.empty()) cmd.pop_back();
      continue;
    }

    // Command history navigation
    if (ch == KEY_UP) {
      if (!cmdHistory.empty()) {
        if (histIdx < 0) histIdx = (int)cmdHistory.size() - 1;
        else if (histIdx > 0) histIdx--;
        cmd = cmdHistory[histIdx];
      }
      continue;
    }
    if (ch == KEY_DOWN) {
      if (!cmdHistory.empty() && histIdx >= 0) {
        histIdx++;
        if (histIdx >= (int)cmdHistory.size()) {
          histIdx = -1;
          cmd.clear();
        } else {
          cmd = cmdHistory[histIdx];
        }
      }
      continue;
    }

    if (ch != '\n' && ch != KEY_ENTER) {
      if (ch >= 32 && ch < 127) cmd.push_back((char)ch);
      continue;
    }

    // ── Execute command ─────────────────────────────────────────────────────
    std::string c = trim(cmd);
    cmdMode = false; cmd.clear(); histIdx = -1;
    if (c.empty()) continue;

    // Save to command history
    cmdHistory.push_back(c);

    std::istringstream ss(c);
    std::string verb, arg1; int nArg = 30;
    ss >> verb;
    if (verb == "log") {
      std::string logArg;
      if (ss >> logArg) {
        try { nArg = std::stoi(logArg); } catch(...) { nArg = 50; }
      } else { nArg = 50; }
    } else {
      ss >> arg1;
      std::string nStr;
      if (ss >> nStr) {
        try { nArg = std::stoi(nStr); } catch(...) { nArg = 30; }
      }
    }
    lastCmd = c;

    if (verb == "clear") {
      outputLines.clear();
      {
        std::lock_guard<std::mutex> lk(g_alertMtx);
        g_alertLines.clear();
      }
      g_alertCount = 0;
      alertScroll = 0;
      continue;
    }
    if (verb == "help") {
      outputLines = {
        "Commands:",
        "  /hosts              - All hosts with status, CPU/RAM/DISK/LOAD",
        "  /history <host> [n] - Last n minutes metric samples for a host",
        "  /log [n]            - Last n minutes log events",
        "  /clear              - Clear command output & alerts",
        "",
        "Keys: Up/Down = history, PgUp/PgDn = scroll alerts",
      };
      continue;
    }

    outputLines.clear();
    outputLines.push_back("Querying server " + host + ":" + std::to_string(port) + " ...");
    refresh();

    std::string resp;
    if (verb == "hosts") {
      resp = queryServer(host, port, "hosts");
    } else if (verb == "history") {
      if (arg1.empty()) { outputLines = {"Usage: /history <host> [n]"}; continue; }
      resp = queryServer(host, port, "history " + arg1 + " " + std::to_string(nArg));
    } else if (verb == "log") {
      resp = queryServer(host, port, "log " + std::to_string(nArg));
    } else {
      outputLines = {"Unknown command: /" + verb, "Try /help"};
      continue;
    }

    outputLines.clear();
    if (resp.empty() || resp == "[]") { outputLines = {"(no data)"}; continue; }
    if (resp.front() == '{') { outputLines = {resp}; continue; }

    auto objs = splitJsonObjects(resp);
    if (objs.empty()) { outputLines = {"(empty response)"}; continue; }

    if (verb == "hosts") {
      char hdr[128];
      snprintf(hdr, sizeof(hdr), "%-16s %-8s %6s %6s %6s %6s  %-12s",
               "HOST","STATUS","CPU%","RAM%","DISK%","LOAD","LAST SEEN");
      outputLines.push_back(hdr);
      outputLines.push_back(std::string(cols-1, '-'));
      for (auto &obj : objs) {
        std::string st = jstr(obj, "status");
        int co = statusColor(st);
        std::string ts = fmtTs(jnum(obj, "lastSeen"));
        char row[256];
        snprintf(row, sizeof(row), "%-16s %-8s %5s%% %5s%% %5s%% %6s  %s",
                 jstr(obj,"host").substr(0,16).c_str(),
                 st.substr(0,8).c_str(),
                 jnum(obj,"cpu").c_str(), jnum(obj,"ram").c_str(),
                 jnum(obj,"disk").c_str(), jnum(obj,"load").c_str(),
                 ts.c_str());
        std::string marker = (co==3?"[!] ":co==4?"[~] ":co==6?"[?] ":"[.] ");
        outputLines.push_back(marker + row);
      }
    } else if (verb == "history") {
      char hdr[128];
      snprintf(hdr, sizeof(hdr), "Host: %s  (%d samples, last %d min)",
               arg1.c_str(), (int)objs.size(), nArg);
      outputLines.push_back(hdr);
      snprintf(hdr, sizeof(hdr), "%-10s %6s %6s %6s %7s  %8s %8s",
               "TIME","CPU%","RAM%","DISK%","LOAD","RX KB/s","TX KB/s");
      outputLines.push_back(hdr);
      outputLines.push_back(std::string(cols-1, '-'));
      for (int i = (int)objs.size()-1; i >= 0; i--) {
        auto &obj = objs[i];
        char row[256];
        snprintf(row, sizeof(row), "%-10s %5s%% %5s%% %5s%% %7s  %8s %8s",
                 fmtTs(jnum(obj,"ts")).c_str(),
                 jnum(obj,"cpu").c_str(), jnum(obj,"ram").c_str(),
                 jnum(obj,"disk").c_str(), jnum(obj,"load").c_str(),
                 jnum(obj,"rx").c_str(), jnum(obj,"tx").c_str());
        outputLines.push_back(row);
        if ((int)outputLines.size() > 500) break;
      }
    } else if (verb == "log") {
      char hdr[128];
      snprintf(hdr, sizeof(hdr), "Event Log (last %d min, %d entries)",
               nArg, (int)objs.size());
      outputLines.push_back(hdr);
      outputLines.push_back(std::string(cols-1, '-'));
      for (int i = (int)objs.size()-1; i >= 0; i--) {
        auto &obj = objs[i];
        std::string type = jstr(obj,"type");
        char row[256];
        snprintf(row, sizeof(row), "%-10s %-12s %-12s %-10s  %s",
                 fmtTs(jnum(obj,"ts")).c_str(),
                 jstr(obj,"host").substr(0,12).c_str(),
                 jstr(obj,"ip").substr(0,12).c_str(),
                 type.c_str(),
                 jstr(obj,"detail").c_str());
        outputLines.push_back(row);
        if ((int)outputLines.size() > 500) break;
      }
    }

    // Reset alert scroll to show latest when user runs a command
    alertScroll = 0;
  }

  g_running = false;
  endwin();
  return 0;
}
