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

// -- Utility-- 
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

static int statusColor(const std::string &st) {
  if (st == "ALERT")   return 3;
  if (st == "WARN")    return 4;
  if (st == "STALE")   return 6;
  if (st == "OK")      return 2;
  return 1;
}

// -- Color pairs-- 
enum ColorPairs {
  CP_DEFAULT = 1, CP_GREEN, CP_RED, CP_YELLOW, CP_CYAN, CP_MAGENTA,
  CP_HEADER,   // white on blue
  CP_ALERT_HDR, // red on dark
  CP_SEP        // cyan on dark
};

static void initViewerColors() {
  start_color();
  use_default_colors();
  init_pair(CP_DEFAULT,   COLOR_WHITE,   -1);
  init_pair(CP_GREEN,     COLOR_GREEN,   -1);
  init_pair(CP_RED,       COLOR_RED,     -1);
  init_pair(CP_YELLOW,    COLOR_YELLOW,  -1);
  init_pair(CP_CYAN,      COLOR_CYAN,    -1);
  init_pair(CP_MAGENTA,   COLOR_MAGENTA, -1);
  init_pair(CP_HEADER,    COLOR_WHITE,   COLOR_BLUE);
  init_pair(CP_ALERT_HDR, COLOR_RED,     COLOR_BLACK);
  init_pair(CP_SEP,       COLOR_CYAN,    COLOR_BLACK);
  if (COLORS >= 256) {
    init_pair(CP_GREEN,     46,  -1);
    init_pair(CP_RED,      196,  -1);
    init_pair(CP_YELLOW,   202,  -1);
    init_pair(CP_CYAN,     153,  -1);
    init_pair(CP_MAGENTA,  141,  -1);
    init_pair(CP_HEADER,    15,  17);
    init_pair(CP_ALERT_HDR,196, 234);
    init_pair(CP_SEP,      153, 234);
  }
}

// -- Global state for broadcast receiver-- 
static std::mutex g_alertMtx;
static std::vector<std::string> g_alertLines;
static const int MAX_ALERT_LINES = 200;

static std::atomic<bool> g_bcastConnected{false};
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_needsRedraw{true};
static std::atomic<int>  g_alertCount{0};

static std::string formatAlertEvt(const std::string &json) {
  std::string host = jstr(json, "host");
  std::string ts   = fmtTs(jnum(json, "timestamp"));
  std::string detail;

  std::string cpuVal = jnum(json, "cpu"), cpuTh = jnum(json, "cpu_threshold");
  if (cpuVal != "-" && cpuTh != "-")
    detail += "CPU=" + cpuVal + "%(>" + cpuTh + "%) ";

  std::string ramVal = jnum(json, "ram"), ramTh = jnum(json, "ram_threshold");
  if (ramVal != "-" && ramTh != "-")
    detail += "RAM=" + ramVal + "%(>" + ramTh + "%) ";

  std::string diskVal = jnum(json, "disk"), diskTh = jnum(json, "disk_threshold");
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
      g_needsRedraw = true;
      for (int i = 0; i < 20 && g_running; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    g_bcastConnected = true;
    g_needsRedraw = true;

    struct timeval tv{}; tv.tv_sec = 5;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string buffer;
    char buf[8192];
    while (g_running) {
      int n = recv(fd, buf, sizeof(buf), 0);
      if (n <= 0) {
        if (n == 0) break;
        continue;
      }
      buffer.append(buf, n);

      size_t pos;
      while ((pos = buffer.find('\n')) != std::string::npos) {
        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);

        if (line.rfind("ALERT_EVT ", 0) == 0) {
          std::string json = line.substr(10);
          std::string formatted = formatAlertEvt(json);
          {
            std::lock_guard<std::mutex> lk(g_alertMtx);
            g_alertLines.push_back(formatted);
            if ((int)g_alertLines.size() > MAX_ALERT_LINES)
              g_alertLines.erase(g_alertLines.begin());
          }
          g_alertCount++;
          g_needsRedraw = true;
        }
      }
      if (buffer.size() > 65536) buffer.clear();
    }
    close(fd);
    g_bcastConnected = false;
    g_needsRedraw = true;
  }
}

// -- Safe window draw helpers-- 
static void wDrawLine(WINDOW *w, int y, int x, const std::string &s, int maxCols) {
  wmove(w, y, x);
  waddnstr(w, s.c_str(), maxCols - x);
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
  keypad(stdscr, TRUE); curs_set(0);
  initViewerColors();

  // Start broadcast receiver
  std::thread bcastThread(broadcastReceiver, host, port);
  bcastThread.detach();

  std::vector<std::string> outputLines;
  std::string lastCmd;
  bool cmdMode = false;
  std::string cmd;

  // Command history
  std::vector<std::string> cmdHistory;
  int histIdx = -1;

  // Scroll offsets
  int cmdScroll   = 0; // 0 = show latest at bottom
  int alertScroll = 0;

  // Previous terminal dimensions (detect resize)
  int prevRows = 0, prevCols = 0;

  // Sub-windows (created/resized dynamically)
  WINDOW *hdrWin   = nullptr; // header bar
  WINDOW *cmdWin   = nullptr; // command output
  WINDOW *sepWin   = nullptr; // separator
  WINDOW *alertWin = nullptr; // alert panel
  WINDOW *statWin  = nullptr; // status bar
  WINDOW *inputWin = nullptr; // command input

  outputLines = {
    "viewer_cli -- Monitor Query Client",
    "",
    "Commands:",
    "  /hosts              - List all hosts with current status",
    "  /history <host> [n] - Metric history for last n mins (default 30)",
    "  /log [n]            - Event log for last n mins (default 50)",
    "  /help               - Show this help",
    "  /clear              - Clear output / clear alerts",
    "",
    "Press '/' to enter a command, Esc to cancel, q to quit.",
    "[Up/Down] Scroll command output   [PgUp/Dn] Scroll alerts",
  };

  while (true) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    // -- Detect resize => recreate windows-- 
    if (rows != prevRows || cols != prevCols) {
      prevRows = rows; prevCols = cols;
      g_needsRedraw = true;

      // Destroy old windows
      if (hdrWin)   delwin(hdrWin);
      if (cmdWin)   delwin(cmdWin);
      if (sepWin)   delwin(sepWin);
      if (alertWin) delwin(alertWin);
      if (statWin)  delwin(statWin);
      if (inputWin) delwin(inputWin);

      // Layout:
      //   Row 0:            hdrWin   (1 row)
      //   Row 1..sepR-1:    cmdWin   (command output)
      //   Row sepR:         sepWin   (1 row, alert header)
      //   Row sepR+1..R-3:  alertWin (alert panel)
      //   Row R-2:          statWin  (1 row)
      //   Row R-1:          inputWin (1 row)
      int usable = rows - 4; // hdr + sep + stat + input
      int alertH = std::max(4, usable / 3);
      int cmdH   = usable - alertH;
      if (cmdH < 3) { cmdH = 3; alertH = usable - cmdH; }
      int sepR = 1 + cmdH;

      hdrWin   = newwin(1, cols, 0, 0);
      cmdWin   = newwin(cmdH, cols, 1, 0);
      sepWin   = newwin(1, cols, sepR, 0);
      alertWin = newwin(alertH, cols, sepR + 1, 0);
      statWin  = newwin(1, cols, rows - 2, 0);
      inputWin = newwin(1, cols, rows - 1, 0);

      // Enable keypad on stdscr (already done), and set colors
      for (auto *w : {hdrWin, cmdWin, sepWin, alertWin, statWin, inputWin})
        keypad(w, TRUE);
    }

    // -- Input handling (timeout-based, non-blocking)-- 
    timeout(50); // 50ms wait, returns ERR if no key
    int ch = getch();

    if (ch != ERR)
      g_needsRedraw = true;

    if (ch == 'q' || ch == 'Q') break;

    if (!cmdMode) {
      // -- Normal mode key handling-- 
      if (ch == '/') {
        cmdMode = true; cmd.clear(); histIdx = -1;
      } else if (ch == KEY_UP) {
        cmdScroll++;
        int maxScr = std::max(0, (int)outputLines.size() - getmaxy(cmdWin));
        if (cmdScroll > maxScr) cmdScroll = maxScr;
      } else if (ch == KEY_DOWN) {
        cmdScroll--;
        if (cmdScroll < 0) cmdScroll = 0;
      } else if (ch == KEY_PPAGE) {
        alertScroll += 3;
        std::lock_guard<std::mutex> lk(g_alertMtx);
        int maxScr = std::max(0, (int)g_alertLines.size() - getmaxy(alertWin));
        if (alertScroll > maxScr) alertScroll = maxScr;
      } else if (ch == KEY_NPAGE) {
        alertScroll -= 3;
        if (alertScroll < 0) alertScroll = 0;
      }
    } else {
      // -- Command mode key handling-- 
      if (ch == 27) {
        cmdMode = false; cmd.clear(); histIdx = -1;
      } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        if (!cmd.empty()) cmd.pop_back();
      } else if (ch == KEY_UP) {
        // Command history up
        if (!cmdHistory.empty()) {
          if (histIdx < 0) histIdx = (int)cmdHistory.size() - 1;
          else if (histIdx > 0) histIdx--;
          cmd = cmdHistory[histIdx];
        }
      } else if (ch == KEY_DOWN) {
        // Command history down
        if (!cmdHistory.empty() && histIdx >= 0) {
          histIdx++;
          if (histIdx >= (int)cmdHistory.size()) {
            histIdx = -1; cmd.clear();
          } else {
            cmd = cmdHistory[histIdx];
          }
        }
      } else if (ch == '\n' || ch == KEY_ENTER) {
        // -- Execute command-- 
        std::string c = trim(cmd);
        cmdMode = false; cmd.clear(); histIdx = -1;

        if (!c.empty()) {
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
            { std::lock_guard<std::mutex> lk(g_alertMtx); g_alertLines.clear(); }
            g_alertCount = 0;
            alertScroll = 0;
          } else if (verb == "help") {
            outputLines = {
              "Commands:",
              "  /hosts              - All hosts with status, CPU/RAM/DISK/LOAD",
              "  /history <host> [n] - Last n minutes metric samples",
              "  /log [n]            - Last n minutes log events",
              "  /clear              - Clear output & alerts",
              "",
              "Normal mode:  Up/Down scroll output  PgUp/Dn scroll alerts",
              "Command mode: Up/Down browse history",
            };
          } else {
            outputLines.clear();
            outputLines.push_back("Querying server " + host + ":" + std::to_string(port) + "...");
            g_needsRedraw = true;

            // Force immediate UI update before blocking query
            if (cmdWin) {
              werase(cmdWin);
              wDrawLine(cmdWin, 0, 0, outputLines[0], cols);
              wnoutrefresh(cmdWin);
              doupdate();
            }

            std::string resp;
            if (verb == "hosts") {
              resp = queryServer(host, port, "hosts");
            } else if (verb == "history") {
              if (arg1.empty()) { outputLines = {"Usage: /history <host> [n]"}; goto done; }
              resp = queryServer(host, port, "history " + arg1 + " " + std::to_string(nArg));
            } else if (verb == "log") {
              resp = queryServer(host, port, "log " + std::to_string(nArg));
            } else {
              outputLines = {"Unknown command: /" + verb, "Try /help"};
              goto done;
            }

            outputLines.clear();
            if (resp.empty() || resp == "[]") { outputLines = {"(no data)"}; goto done; }
            if (resp.front() == '{') { outputLines = {resp}; goto done; }

            {
              auto objs = splitJsonObjects(resp);
              if (objs.empty()) { outputLines = {"(empty response)"}; goto done; }

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
                  char row[256];
                  snprintf(row, sizeof(row), "%-10s %-12s %-12s %-10s  %s",
                           fmtTs(jnum(obj,"ts")).c_str(),
                           jstr(obj,"host").substr(0,12).c_str(),
                           jstr(obj,"ip").substr(0,12).c_str(),
                           jstr(obj,"type").c_str(),
                           jstr(obj,"detail").c_str());
                  outputLines.push_back(row);
                  if ((int)outputLines.size() > 500) break;
                }
              }
            }
          }
done:
          cmdScroll = 0; // reset scroll on new command
          g_needsRedraw = true;
        }
      } else if (ch >= 32 && ch < 127) {
        cmd.push_back((char)ch);
      }
    }

    // -- Periodic redraw (time-based, for clock update)-- 
    {
      static auto lastClock = std::chrono::steady_clock::now();
      auto now = std::chrono::steady_clock::now();
      if (std::chrono::duration_cast<std::chrono::seconds>(now - lastClock).count() >= 1) {
        lastClock = now;
        g_needsRedraw = true;
      }
    }

    // -- Only redraw when needed-- 
    if (!g_needsRedraw.exchange(false))
      continue;

    if (!hdrWin || !cmdWin || !sepWin || !alertWin || !statWin || !inputWin)
      continue;

    int cmdH   = getmaxy(cmdWin);
    int alertH = getmaxy(alertWin);

    // -- Draw header bar-- 
    werase(hdrWin);
    wbkgd(hdrWin, COLOR_PAIR(CP_HEADER));
    wattron(hdrWin, A_BOLD);
    {
      char hb[256];
      snprintf(hb, sizeof(hb), " [*] MONITOR VIEWER  server=%s:%d  group1_H@ckErUSTH",
               host.c_str(), port);
      wDrawLine(hdrWin, 0, 0, hb, cols);
      std::string ts = nowStr(time(nullptr));
      mvwaddstr(hdrWin, 0, cols - (int)ts.size() - 1, ts.c_str());
    }
    wattroff(hdrWin, A_BOLD);
    wnoutrefresh(hdrWin);

    // -- Draw command output panel-- 
    werase(cmdWin);
    {
      int total = (int)outputLines.size();
      int firstIdx = total - cmdH - cmdScroll;
      if (firstIdx < 0) firstIdx = 0;
      for (int i = 0; i < cmdH; i++) {
        int idx = firstIdx + i;
        if (idx >= total) break;
        wDrawLine(cmdWin, i, 0, outputLines[idx], cols);
      }
      // Scroll indicator
      if (cmdScroll > 0) {
        wattron(cmdWin, COLOR_PAIR(CP_CYAN) | A_DIM);
        char si[32];
        snprintf(si, sizeof(si), " [^%d more] ", cmdScroll);
        mvwaddstr(cmdWin, cmdH - 1, cols - (int)strlen(si) - 1, si);
        wattroff(cmdWin, COLOR_PAIR(CP_CYAN) | A_DIM);
      }
    }
    wnoutrefresh(cmdWin);

    // -- Draw separator-- 
    werase(sepWin);
    wbkgd(sepWin, COLOR_PAIR(CP_ALERT_HDR));
    wattron(sepWin, A_BOLD);
    {
      int cnt = g_alertCount.load();
      char sb[128];
      snprintf(sb, sizeof(sb), " >> LIVE ALERTS (%d total) ", cnt);
      wDrawLine(sepWin, 0, 0, sb, cols);
      const char *colHdr = "TIME    HOST            DETAIL";
      int hLen = (int)strlen(colHdr);
      if (cols - hLen - 2 > 30)
        mvwaddstr(sepWin, 0, cols - hLen - 1, colHdr);
    }
    wattroff(sepWin, A_BOLD);
    wnoutrefresh(sepWin);

    // -- Draw alert panel-- 
    werase(alertWin);
    {
      std::lock_guard<std::mutex> lk(g_alertMtx);
      int total = (int)g_alertLines.size();
      if (total == 0) {
        wattron(alertWin, COLOR_PAIR(CP_CYAN) | A_DIM);
        wDrawLine(alertWin, 0, 2, "(no alerts yet -- waiting for broadcast...)", cols);
        wattroff(alertWin, COLOR_PAIR(CP_CYAN) | A_DIM);
      } else {
        int firstIdx = total - alertH - alertScroll;
        if (firstIdx < 0) firstIdx = 0;
        for (int i = 0; i < alertH; i++) {
          int idx = firstIdx + i;
          if (idx >= total) break;
          wattron(alertWin, COLOR_PAIR(CP_RED));
          mvwaddstr(alertWin, i, 0, "  !");
          wattroff(alertWin, COLOR_PAIR(CP_RED));
          wattron(alertWin, COLOR_PAIR(CP_RED) | A_BOLD);
          waddnstr(alertWin, g_alertLines[idx].c_str(), cols - 4);
          wattroff(alertWin, COLOR_PAIR(CP_RED) | A_BOLD);
        }
        if (alertScroll > 0) {
          wattron(alertWin, COLOR_PAIR(CP_CYAN) | A_DIM);
          char si[32];
          snprintf(si, sizeof(si), " [v%d more] ", alertScroll);
          mvwaddstr(alertWin, alertH - 1, cols - (int)strlen(si) - 1, si);
          wattroff(alertWin, COLOR_PAIR(CP_CYAN) | A_DIM);
        }
      }
    }
    wnoutrefresh(alertWin);

    // -- Draw status bar-- 
    werase(statWin);
    wbkgd(statWin, COLOR_PAIR(CP_CYAN));
    wattron(statWin, COLOR_PAIR(CP_CYAN));
    if (!lastCmd.empty()) {
      char sb[256]; snprintf(sb, sizeof(sb), " Last: /%s ", lastCmd.c_str());
      wDrawLine(statWin, 0, 0, sb, cols);
    }
    {
      std::string bs = g_bcastConnected.load() ? "[LIVE]" : "[OFFLINE]";
      mvwaddstr(statWin, 0, cols - (int)bs.size() - 1, bs.c_str());
    }
    wattroff(statWin, COLOR_PAIR(CP_CYAN));
    wnoutrefresh(statWin);

    // -- Draw command input line-- 
    werase(inputWin);
    wattron(inputWin, A_BOLD);
    if (cmdMode) {
      wattron(inputWin, COLOR_PAIR(CP_CYAN));
      wDrawLine(inputWin, 0, 0, " > /" + cmd + "_", cols);
      wattroff(inputWin, COLOR_PAIR(CP_CYAN));
    } else {
      wattron(inputWin, COLOR_PAIR(CP_DEFAULT));
      wDrawLine(inputWin, 0, 0,
        " [/] command  [q] quit  [Up/Dn] scroll output  [PgUp/Dn] scroll alerts", cols);
      wattroff(inputWin, COLOR_PAIR(CP_DEFAULT));
    }
    wattroff(inputWin, A_BOLD);
    wnoutrefresh(inputWin);

    // -- Single screen update-- 
    doupdate();
  }

  g_running = false;
  // Clean up windows
  if (hdrWin)   delwin(hdrWin);
  if (cmdWin)   delwin(cmdWin);
  if (sepWin)   delwin(sepWin);
  if (alertWin) delwin(alertWin);
  if (statWin)  delwin(statWin);
  if (inputWin) delwin(inputWin);
  endwin();
  return 0;
}
