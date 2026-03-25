#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
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
  init_pair(1, COLOR_WHITE,   -1);
  init_pair(2, COLOR_GREEN,   -1);
  init_pair(3, COLOR_RED,     -1);
  init_pair(4, COLOR_YELLOW,  -1);
  init_pair(5, COLOR_CYAN,    -1);
  init_pair(6, COLOR_MAGENTA, -1);
  init_pair(7, COLOR_WHITE,   COLOR_BLUE);
  init_pair(8, COLOR_WHITE,   COLOR_BLACK); // dashboard border
  if (COLORS >= 256) {
    init_pair(2,  46,  -1);
    init_pair(3, 196,  -1);
    init_pair(4, 202,  -1);
    init_pair(5, 153,  -1);
    init_pair(6, 141,  -1);
    init_pair(7,  15,  17);
    init_pair(8, 153,  236);
  }
}

static int statusColor(const std::string &st) {
  if (st == "ALERT")   return 3;
  if (st == "WARN")    return 4;
  if (st == "STALE")   return 6;
  if (st == "OK")      return 2;
  return 1;
}

// ── Broadcast receiver ──────────────────────────────────────────────────────
// Strips ANSI escape sequences and splits into lines for ncurses rendering.
static std::string stripAnsi(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  bool inEsc = false;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\033') { inEsc = true; continue; }
    if (inEsc) {
      // ESC [ ... <letter> ends the sequence
      if ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z'))
        inEsc = false;
      continue;
    }
    out += s[i];
  }
  return out;
}

static std::vector<std::string> splitLines(const std::string &s) {
  std::vector<std::string> lines;
  std::istringstream iss(s);
  std::string line;
  while (std::getline(iss, line)) {
    // remove trailing CR
    while (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
  }
  return lines;
}

// Global broadcast state
static std::mutex g_bcastMtx;
static std::vector<std::string> g_bcastLines;
static std::atomic<bool> g_bcastConnected{false};
static std::atomic<bool> g_running{true};

static void broadcastReceiver(const std::string &host, uint16_t port) {
  while (g_running) {
    int fd = connectTo(host, port);
    if (fd < 0) {
      g_bcastConnected = false;
      // Retry after 2s
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
        // timeout — just retry recv
        continue;
      }
      buffer.append(buf, n);

      // If buffer contains screen clear sequence, take the latest full frame
      // The frame is delimited by CLR = \033[2J\033[H
      std::string clr = "\033[2J\033[H";
      auto lastClr = buffer.rfind(clr);
      if (lastClr != std::string::npos) {
        std::string frame = buffer.substr(lastClr + clr.size());
        // Strip ANSI for ncurses rendering
        std::string clean = stripAnsi(frame);
        auto lines = splitLines(clean);
        {
          std::lock_guard<std::mutex> lk(g_bcastMtx);
          g_bcastLines = std::move(lines);
        }
        buffer.clear();
      }
      // Prevent buffer from growing unbounded
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
  int histIdx = -1; // -1 means not browsing history

  outputLines = {
    "viewer_cli — Monitor Query Client",
    "",
    "Commands:",
    "  /hosts              - List all hosts with current status",
    "  /history <host> [n] - Metric history for last n mins (default 30)",
    "  /log [n]            - Event log for last n mins (default 50)",
    "  /help               - Show this help",
    "  /clear              - Clear output",
    "",
    "Press '/' to enter a command, Esc to cancel, q to quit.",
    "Up/Down arrows recall command history.",
    "",
    "Live dashboard from server broadcast shown at bottom.",
  };

  while (true) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    erase();

    // ── Header bar ──────────────────────────────────────────────────────────
    attron(COLOR_PAIR(7) | A_BOLD);
    char hdrBuf[256];
    snprintf(hdrBuf, sizeof(hdrBuf),
             " ◈ MONITOR VIEWER  server=%s:%d  Developed by group1_H@ckErUSTH ",
             host.c_str(), port);
    for (int i = 0; i < cols; i++) mvaddch(0, i, ' ');
    mvaddstr(0, 1, hdrBuf);
    int tsLen = 8; mvaddstr(0, cols - tsLen - 1, nowStr(time(nullptr)).c_str());
    attroff(COLOR_PAIR(7) | A_BOLD);

    // ── Determine layout: split screen ──────────────────────────────────────
    // Dashboard gets bottom 1/3, command output gets top 2/3
    // Reserve: 1 row header, 2 rows footer (status + cmd), 1 row separator
    int usableRows = rows - 4; // header(1) + status(1) + cmdline(1) + separator(1)
    int dashRows = 0;
    std::vector<std::string> dashLines;
    {
      std::lock_guard<std::mutex> lk(g_bcastMtx);
      dashLines = g_bcastLines;
    }

    if (!dashLines.empty()) {
      dashRows = std::min((int)dashLines.size(), usableRows / 3);
      if (dashRows < 3) dashRows = std::min(3, usableRows / 2);
    }
    int outputRows = usableRows - dashRows;
    if (outputRows < 3) outputRows = 3;

    // ── Top section: command output ─────────────────────────────────────────
    int startLine = 0;
    if ((int)outputLines.size() > outputRows)
      startLine = (int)outputLines.size() - outputRows;
    for (int i = 0; i < outputRows && startLine + i < (int)outputLines.size(); i++) {
      const std::string &ln = outputLines[startLine + i];
      mvaddnstr(1 + i, 0, ln.c_str(), cols - 1);
    }

    // ── Dashboard separator + live broadcast ────────────────────────────────
    if (dashRows > 0 && !dashLines.empty()) {
      int sepRow = 1 + outputRows;
      attron(COLOR_PAIR(8) | A_BOLD);
      mvhline(sepRow, 0, ' ', cols);
      std::string dashTitle = g_bcastConnected.load()
        ? " ▼ LIVE DASHBOARD (broadcast) "
        : " ▼ LIVE DASHBOARD (reconnecting...) ";
      mvaddstr(sepRow, 1, dashTitle.c_str());
      attroff(COLOR_PAIR(8) | A_BOLD);

      int dashStart = sepRow + 1;
      // Show the last dashRows lines of the broadcast
      int bcastStart = 0;
      if ((int)dashLines.size() > dashRows)
        bcastStart = (int)dashLines.size() - dashRows;
      for (int i = 0; i < dashRows && bcastStart + i < (int)dashLines.size(); i++) {
        const std::string &dl = dashLines[bcastStart + i];
        mvaddnstr(dashStart + i, 0, dl.c_str(), cols - 1);
      }
    }

    // ── Status bar ──────────────────────────────────────────────────────────
    attron(COLOR_PAIR(5));
    mvhline(rows - 2, 0, ' ', cols);
    if (!lastCmd.empty()) {
      char sb[256]; snprintf(sb, sizeof(sb), " Last: /%s ", lastCmd.c_str());
      mvaddstr(rows - 2, 0, sb);
    }
    // Show broadcast status on right side
    {
      std::string bstat = g_bcastConnected.load() ? "[LIVE]" : "[OFFLINE]";
      int blen = (int)bstat.size();
      mvaddstr(rows - 2, cols - blen - 1, bstat.c_str());
    }
    attroff(COLOR_PAIR(5));

    // ── Command line ────────────────────────────────────────────────────────
    attron(A_BOLD);
    mvhline(rows - 1, 0, ' ', cols);
    if (cmdMode) {
      attron(COLOR_PAIR(5) | A_BOLD);
      mvaddstr(rows - 1, 0, " > /");
      addstr(cmd.c_str()); addstr("_");
      attroff(COLOR_PAIR(5) | A_BOLD);
    } else {
      attron(COLOR_PAIR(1));
      mvaddstr(rows - 1, 0, " [/] command  [q] quit  [↑↓] history");
      attroff(COLOR_PAIR(1));
    }
    attroff(A_BOLD);

    refresh();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    int ch = getch();
    if (ch == 'q' || ch == 'Q') break;

    if (!cmdMode) {
      if (ch == '/') { cmdMode = true; cmd.clear(); histIdx = -1; }
      continue;
    }

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

    if (verb == "clear") { outputLines.clear(); continue; }
    if (verb == "help") {
      outputLines = {
        "Commands:",
        "  /hosts              - All hosts with status, CPU/RAM/DISK/LOAD",
        "  /history <host> [n] - Last n minutes metric samples for a host",
        "  /log [n]            - Last n minutes log events",
        "  /clear              - Clear output",
        "",
        "Use Up/Down arrows to browse command history.",
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
  }

  g_running = false;
  endwin();
  return 0;
}
