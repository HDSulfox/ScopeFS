#include "scopefs/ui.hpp"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace scopefs::ui {

namespace {

std::string esc(const std::string& code) { return "\x1b[" + code + "m"; }

std::string zh(const std::string& key) {
  static const std::map<std::string, std::string> dict = {
      {"command_focus", "命令焦点"},
      {"command_surface", "命令工作区"},
      {"observable_kernel", "可观测 inode/block/journal/COW 内核"},
      {"terminal_first", "终端优先"},
      {"volume", "卷状态"},
      {"session", "会话"},
      {"observability", "观测"},
      {"mount", "挂载"},
      {"blocks", "块"},
      {"user", "用户"},
      {"open", "打开"},
      {"trace", "trace"},
      {"snap", "快照"},
      {"tips", "提示"},
      {"tab_commands", "Tab 补全"},
      {"palette", "命令面板"},
      {"directory", "目录"},
      {"entries", "项"},
      {"gen", "gen"},
      {"ref", "ref"},
      {"size", "大小"},
      {"kernel", "内核"},
      {"capacity", "容量"},
      {"hot_inodes", "热点 inode"},
      {"no_inode_activity", "暂无 inode 活动"},
      {"tree_shared", "目录树 / 共享引用"},
      {"disk_map", "磁盘图"},
      {"trace_timeline", "Trace 时间线"},
      {"trace_replay", "Trace 回放 / 只读"},
      {"trace_step", "Trace 单步"},
      {"snapshot_diff", "快照 diff"},
      {"class_graph", "身份类图"},
      {"acl_graph", "ACL 图"},
      {"read_result", "读取结果"},
      {"offset", "偏移"},
      {"read_advance", "本次读取"},
      {"fd_offset", "fd 偏移"}};
  const auto it = dict.find(key);
  return it == dict.end() ? key : it->second;
}

std::string en(const std::string& key) {
  static const std::map<std::string, std::string> dict = {
      {"command_focus", "command focus"},
      {"command_surface", "command surface"},
      {"observable_kernel", "observable inode/block/journal/COW kernel"},
      {"terminal_first", "terminal-first"},
      {"volume", "Volume"},
      {"session", "Session"},
      {"observability", "Observability"},
      {"mount", "mount"},
      {"blocks", "blocks"},
      {"user", "user"},
      {"open", "open"},
      {"trace", "trace"},
      {"snap", "snap"},
      {"tips", "tips"},
      {"tab_commands", "tab commands"},
      {"palette", "ctrl+p palette"},
      {"directory", "Directory"},
      {"entries", "entries"},
      {"gen", "gen"},
      {"ref", "ref"},
      {"size", "size"},
      {"kernel", "Kernel"},
      {"capacity", "Capacity"},
      {"hot_inodes", "Hot inodes"},
      {"no_inode_activity", "no inode activity"},
      {"tree_shared", "Tree / shared references"},
      {"disk_map", "Disk map"},
      {"trace_timeline", "Trace timeline"},
      {"trace_replay", "Trace replay / read-only"},
      {"trace_step", "Trace step"},
      {"snapshot_diff", "Snapshot diff"},
      {"class_graph", "Class graph"},
      {"acl_graph", "ACL graph"},
      {"read_result", "Read result"},
      {"offset", "offset"},
      {"read_advance", "read"},
      {"fd_offset", "fd offset"}};
  const auto it = dict.find(key);
  return it == dict.end() ? key : it->second;
}

std::string toneCode(const Theme& th, const std::string& tone) {
  if (tone == "amber") return th.amber;
  if (tone == "blue") return th.blue;
  if (tone == "green") return th.green;
  if (tone == "red") return th.red;
  if (tone == "magenta") return th.magenta;
  if (tone == "gray") return th.gray;
  if (tone == "dim") return th.dim;
  if (tone == "white") return th.white;
  if (tone == "panel") return th.panel;
  return th.border;
}

std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
  }
  if (lines.empty()) lines.emplace_back();
  return lines;
}

std::string iconForType(const std::string& type) {
  if (type == "dir") return "◆";
  if (type == "snap") return "◈";
  if (type == "class") return "◇";
  return "▪";
}

std::string typeTone(const std::string& type) {
  if (type == "dir") return "amber";
  if (type == "snap") return "magenta";
  if (type == "class") return "blue";
  return "white";
}

std::string eventTone(const TraceEvent& event) {
  if (event.status == "deny" || event.status == "error" || event.status == "crash") return "red";
  if (event.type.find("commit") != std::string::npos || event.type.find("fsck") != std::string::npos) return "green";
  if (event.type.find("snapshot") != std::string::npos || event.type.find("cow") != std::string::npos) return "magenta";
  if (event.type.find("path") != std::string::npos || event.type.find("trace") != std::string::npos || event.type.find("read") != std::string::npos) return "blue";
  if (event.type.find("journal") != std::string::npos || event.type.find("block") != std::string::npos) return "amber";
  return "gray";
}

std::string blockGlyph(std::uint32_t refcount) {
  if (refcount == 0) return "·";
  if (refcount == 1) return "░";
  if (refcount == 2) return "▒";
  return "█";
}

std::string blockTone(const MapCell& cell) {
  if (cell.flags == "journal") return "green";
  if (cell.flags == "inode") return "blue";
  if (cell.flags == "refcount") return "amber";
  if (cell.refcount > 1) return "magenta";
  if (cell.refcount == 1) return "gray";
  return "dim";
}

std::string layoutGlyph(const MapCell& cell) {
  if (cell.flags == "super") return "S";
  if (cell.flags == "refcount") return "R";
  if (cell.flags == "inode") return "I";
  if (cell.flags == "journal") return "J";
  if (cell.flags == "snapshot") return "P";
  if (cell.flags == "user") return "U";
  if (cell.refcount) return "D";
  return ".";
}

std::string ownerGlyph(std::uint32_t ownerInode) {
  if (ownerInode == 0) return ".";
  constexpr char digits[] = "0123456789abcdef";
  return std::string(1, digits[ownerInode % 16]);
}

std::string txGlyph(std::uint64_t txid) {
  if (txid == 0) return ".";
  constexpr char digits[] = "0123456789abcdef";
  return std::string(1, digits[txid % 16]);
}

std::string mapGlyph(const std::string& what, const MapCell& cell) {
  if (what == "blocks") return layoutGlyph(cell);
  if (what == "inode") {
    if (cell.flags == "inode") return "I";
    if (cell.flags == "data" && cell.ownerInode != 0) return ownerGlyph(cell.ownerInode);
    if (cell.refcount > 1) return "*";
    return ".";
  }
  if (what == "journal") {
    if (cell.flags == "journal") return "J";
    if (cell.lastWriterTxid != 0) return txGlyph(cell.lastWriterTxid);
    if (cell.flags == "super") return "S";
    return ".";
  }
  if (what == "owner") return ownerGlyph(cell.ownerInode);
  return blockGlyph(cell.refcount);
}

std::string mapTone(const Theme& th, const std::string& what, const MapCell& cell) {
  (void)th;
  if (what == "blocks") return blockTone(cell);
  if (what == "inode") {
    if (cell.flags == "inode") return "blue";
    if (cell.refcount > 1) return "magenta";
    if (cell.ownerInode != 0) return "amber";
    return "dim";
  }
  if (what == "journal") {
    if (cell.flags == "journal") return "green";
    if (cell.lastWriterTxid != 0) return "amber";
    if (cell.flags == "super") return "white";
    return "dim";
  }
  if (what == "owner") {
    if (cell.refcount > 1) return "magenta";
    return cell.ownerInode ? "amber" : "dim";
  }
  return blockTone(cell);
}

std::string mapLegend(const Theme& th, const std::string& what) {
  if (what == "blocks") {
    return color(th, th.dim, "legend ") + color(th, th.white, "S super ") +
           color(th, th.amber, "R refmeta ") + color(th, th.blue, "I inode ") +
           color(th, th.green, "J journal ") + color(th, th.magenta, "P snapshot ") +
           color(th, th.gray, "U users D data . free");
  }
  if (what == "inode") {
    return color(th, th.dim, "legend ") + color(th, th.blue, "I inode-table ") +
           color(th, th.amber, "0-f owner-inode data ") + color(th, th.magenta, "* shared ") +
           color(th, th.gray, ". unrelated/free");
  }
  if (what == "journal") {
    return color(th, th.dim, "legend ") + color(th, th.green, "J journal-reserved ") +
           color(th, th.amber, "0-f last-writer tx ") + color(th, th.white, "S super ") +
           color(th, th.gray, ". no journal signal");
  }
  if (what == "owner") {
    return color(th, th.dim, "legend ") + color(th, th.amber, "0-f owner inode modulo 16 ") +
           color(th, th.magenta, "shared highlighted ") + color(th, th.gray, ". no owner");
  }
  return color(th, th.dim, "legend ") + color(th, th.gray, ". free ") +
         color(th, th.white, "░ allocated ") + color(th, th.magenta, "▒/█ shared");
}

std::string repeatText(const std::string& text, int count) {
  std::string out;
  for (int i = 0; i < count; ++i) out += text;
  return out;
}

std::string indentBlock(const std::string& text, int spaces) {
  const std::string indent(std::max(0, spaces), ' ');
  std::ostringstream out;
  std::istringstream in(text);
  std::string line;
  bool first = true;
  while (std::getline(in, line)) {
    if (!first) out << "\n";
    out << indent << line;
    first = false;
  }
  return out.str();
}

std::string rawFg(const std::string& code, const std::string& text) {
  return code + text;
}

} // namespace

TerminalMetrics detectMetrics() {
  TerminalMetrics metrics;
  if (const char* forced = std::getenv("SCOPEFS_WIDTH")) {
    const int w = std::atoi(forced);
    if (w > 0) metrics.columns = w;
  }
#if defined(_WIN32)
  CONSOLE_SCREEN_BUFFER_INFO info{};
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (out != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(out, &info)) {
    metrics.columns = info.srWindow.Right - info.srWindow.Left + 1;
    metrics.rows = info.srWindow.Bottom - info.srWindow.Top + 1;
  }
#else
  winsize size{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
    metrics.columns = size.ws_col;
    metrics.rows = size.ws_row > 0 ? size.ws_row : metrics.rows;
  }
#endif
  if (const char* forced = std::getenv("SCOPEFS_WIDTH")) {
    const int w = std::atoi(forced);
    if (w > 0) metrics.columns = w;
  }
  metrics.columns = std::max(72, metrics.columns);
  metrics.compact = metrics.columns < 88;
  metrics.wide = metrics.columns >= 140;
  return metrics;
}

Theme theme(bool ansi, const std::string& name, const std::string& lang) {
  Theme th;
  th.ansi = ansi;
  th.lang = lang == "en" ? "en" : "zh";
  th.mono = name == "mono" || !ansi;
  if (!ansi) return th;
  th.reset = esc("0");
  th.bg = esc("48;2;11;12;14");
  th.panel = esc("48;2;28;29;33");
  th.panel2 = esc("48;2;36;36;40");
  th.bold = esc("1");
  th.normalWeight = esc("22");
  th.amber = esc("38;2;255;181;118");
  th.blue = esc("38;2;112;170;255");
  th.green = esc("38;2;110;220;153");
  th.red = esc("38;2;255;110;110");
  th.magenta = esc("38;2;218;140;255");
  th.gray = esc("38;2;165;166;173");
  th.dim = esc("38;2;104;106;114");
  th.white = esc("38;2;242;243;245");
  th.border = esc("38;2;125;96;88");
  if (name == "blue") {
    th.amber = esc("38;2;112;170;255");
    th.blue = esc("38;2;255;181;118");
    th.border = esc("38;2;80;116;150");
  }
  return th;
}

std::string text(const Theme& th, const std::string& key) {
  return th.lang == "en" ? en(key) : zh(key);
}

int displayWidth(const std::string& text) {
  int width = 0;
  for (std::size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c == 0x1b) {
      while (i < text.size() && text[i] != 'm') ++i;
      if (i < text.size()) ++i;
      continue;
    }
    if (c < 0x80) {
      ++width;
      ++i;
    } else {
      std::uint32_t cp = 0;
      std::size_t len = 1;
      if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
        cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3F);
        len = 2;
      } else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
        cp = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6) |
             (static_cast<unsigned char>(text[i + 2]) & 0x3F);
        len = 3;
      } else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) {
        cp = ((c & 0x07) << 18) | ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 12) |
             ((static_cast<unsigned char>(text[i + 2]) & 0x3F) << 6) |
             (static_cast<unsigned char>(text[i + 3]) & 0x3F);
        len = 4;
      }
      const bool wide = (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
                        (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x1F300 && cp <= 0x1FAFF);
      width += wide ? 2 : 1;
      i += len;
    }
  }
  return width;
}

std::string stripAnsi(const std::string& text) {
  std::string out;
  for (std::size_t i = 0; i < text.size();) {
    if (text[i] == '\x1b') {
      while (i < text.size() && text[i] != 'm') ++i;
      if (i < text.size()) ++i;
    } else {
      out.push_back(text[i++]);
    }
  }
  return out;
}

std::string truncate(const std::string& text, int width) {
  if (width <= 0) return "";
  if (displayWidth(text) <= width) return text;
  const std::string plain = stripAnsi(text);
  std::string out;
  int used = 0;
  for (std::size_t i = 0; i < plain.size() && used < width - 1;) {
    unsigned char c = static_cast<unsigned char>(plain[i]);
    std::size_t len = 1;
    if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    const auto chunk = plain.substr(i, std::min(len, plain.size() - i));
    const int w = displayWidth(chunk);
    if (used + w > width - 1) break;
    out += chunk;
    used += w;
    i += len;
  }
  return out + "…";
}

std::string padRight(const std::string& text, int width) {
  const auto clipped = truncate(text, width);
  return clipped + std::string(std::max(0, width - displayWidth(clipped)), ' ');
}

std::string padLeft(const std::string& text, int width) {
  const auto clipped = truncate(text, width);
  return std::string(std::max(0, width - displayWidth(clipped)), ' ') + clipped;
}

std::string center(const std::string& text, int width) {
  const auto clipped = truncate(text, width);
  const int remain = std::max(0, width - displayWidth(clipped));
  return std::string(remain / 2, ' ') + clipped + std::string(remain - remain / 2, ' ');
}

std::string color(const Theme& th, const std::string& code, const std::string& text) {
  if (!th.ansi || th.mono) return text;
  return code + text + th.reset;
}

std::string accent(const Theme& th, const std::string& code, const std::string& text) {
  if (!th.ansi || th.mono) return text;
  return code + th.bold + text + th.reset;
}

std::string panelAccent(const Theme& th, const std::string& code, const std::string& text) {
  if (!th.ansi || th.mono) return text;
  return code + th.bold + text + th.normalWeight;
}

std::string badge(const Theme& th, const std::string& label, const std::string& tone) {
  const auto text = " " + label + " ";
  if (!th.ansi || th.mono) return "[" + label + "]";
  return toneCode(th, tone) + th.bold + text + th.reset;
}

std::string progress(const Theme& th, std::size_t used, std::size_t total, int width, const std::string& tone) {
  if (width <= 0) return "";
  const double ratio = total == 0 ? 0.0 : static_cast<double>(used) / static_cast<double>(total);
  const int fill = std::max(0, std::min(width, static_cast<int>(ratio * width + 0.5)));
  std::string bar = repeatText("█", fill) + repeatText("░", width - fill);
  return color(th, toneCode(th, tone), bar);
}

std::string box(const Theme& th, const std::string& title, const std::vector<std::string>& lines, int width, const std::string& tone) {
  width = std::max(24, width);
  const int inner = width - 4;
  const auto border = toneCode(th, tone);
  std::ostringstream out;
  const std::string titleText = title.empty() ? "" : " " + title + " ";
  const int titleWidth = displayWidth(titleText);
  out << color(th, border, "╭" + titleText + repeatText("─", std::max(0, width - 2 - titleWidth)) + "╮") << "\n";
  for (const auto& line : lines) {
    out << color(th, border, "│ ") << padRight(line, inner) << color(th, border, " │") << "\n";
  }
  out << color(th, border, "╰" + repeatText("─", width - 2) + "╯");
  return out.str();
}

std::string columns(const std::vector<std::string>& boxed, int gap) {
  std::vector<std::vector<std::string>> cols;
  std::size_t rows = 0;
  for (const auto& b : boxed) {
    cols.push_back(splitLines(b));
    rows = std::max(rows, cols.back().size());
  }
  std::vector<int> widths(cols.size(), 0);
  for (std::size_t c = 0; c < cols.size(); ++c) {
    for (const auto& line : cols[c]) widths[c] = std::max(widths[c], displayWidth(line));
  }
  std::ostringstream out;
  for (std::size_t r = 0; r < rows; ++r) {
    for (std::size_t c = 0; c < cols.size(); ++c) {
      if (c) out << std::string(gap, ' ');
      out << padRight(r < cols[c].size() ? cols[c][r] : "", widths[c]);
    }
    out << "\n";
  }
  return out.str();
}

std::string renderDashboard(const Theme& th, const TerminalMetrics& metrics, const KernelStatus& status) {
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  const int left = std::max(0, (metrics.columns - width) / 2);
  const std::string indent(left, ' ');
  const std::vector<std::pair<std::string, std::string>> logoLarge = {
      {"██████  ██████  ██████  ██████  ██████", "██████ ██████"},
      {"██      ██      ██  ██  ██  ██  ██    ", "██     ██    "},
      {"██████  ██      ██  ██  ██████  █████ ", "████   ██████"},
      {"    ██  ██      ██  ██  ██      ██    ", "██         ██"},
      {"██████  ██████  ██████  ██      ██████", "██     ██████"},
      {"  ░░░░    ░░░░    ░░░░  ░░        ░░░░", "░░       ░░░░"}};
  std::ostringstream out;
  if (th.ansi) out << th.bg;
  out << "\n";
  if (metrics.compact) {
    const auto scope = color(th, th.gray, "Scope");
    const auto fs = color(th, th.white, "FS");
    out << indent << center(scope + fs, width) << "\n";
  } else {
    for (const auto& [scope, fs] : logoLarge) {
      const auto plain = scope + "  " + fs;
      const int logoLeft = std::max(0, (width - displayWidth(plain)) / 2);
      out << indent << std::string(logoLeft, ' ')
          << color(th, scope.find("░") != std::string::npos ? th.dim : th.gray, scope)
          << "  " << color(th, fs.find("░") != std::string::npos ? th.dim : th.white, fs) << "\n";
    }
  }
  out << "\n";
  const int panelWidth = metrics.compact ? width : std::min(width - 8, 96);
  const int panelLeft = std::max(0, (metrics.columns - panelWidth) / 2);
  std::vector<std::string> command = {
      rawFg(th.amber, "/scope") + rawFg(th.gray, "  " + text(th, "command_surface")) + th.reset,
      rawFg(th.white, "› ") + rawFg(th.gray, "format  ·  login root root  ·  scope tree  ·  map refcount") + th.reset,
      rawFg(th.blue, "Build") + rawFg(th.white, "  " + text(th, "observable_kernel") + " ") +
          rawFg(th.dim, text(th, "terminal_first")) + th.reset};
  out << indentBlock(box(th, text(th, "command_focus"), command, panelWidth, "amber"), panelLeft) << "\n\n";
  const int cardGap = 2;
  const int cardWidth = metrics.compact ? width : (width - 2 * cardGap) / 3;
  auto volume = box(th, text(th, "volume"), {
      text(th, "mount") + "  " + color(th, status.mountState == "clean" ? th.green : th.amber, status.mountState),
      "tx     " + color(th, th.white, "#" + std::to_string(status.txid)),
      text(th, "blocks") + " " + progress(th, status.blocks, status.blockTotal, std::max(10, cardWidth - 18), "blue")}, cardWidth, "border");
  auto session = box(th, text(th, "session"), {
      text(th, "user") + "   " + color(th, th.white, status.user),
      "cwd    " + color(th, th.gray, truncate(status.cwd, cardWidth - 12)),
      text(th, "open") + "   " + color(th, th.white, std::to_string(status.openFiles))}, cardWidth, "blue");
  auto obs = box(th, text(th, "observability"), {
      text(th, "trace") + "  " + color(th, status.trace ? th.green : th.red, status.trace ? "on" : "off"),
      "inode  " + progress(th, status.inodes, status.inodeTotal, std::max(10, cardWidth - 18), "amber"),
      text(th, "snap") + "   " + color(th, th.magenta, std::to_string(status.snapshots))}, cardWidth, "magenta");
  if (metrics.compact) {
    out << indentBlock(volume, left) << "\n" << indentBlock(session, left) << "\n" << indentBlock(obs, left) << "\n";
  } else {
    out << indentBlock(columns({volume, session, obs}, cardGap), left) << "\n";
  }
  out << indent << color(th, th.dim, text(th, "tips") + "  " + text(th, "tab_commands") + "   " + text(th, "palette") + "   trace show   snapshot diff   fsck") << "\n\n";
  if (th.ansi) out << th.reset;
  return out.str();
}

PromptRender renderPromptLine(const Theme& th, const TerminalMetrics& metrics, const KernelStatus& status, const std::string& line, std::size_t cursor) {
  PromptRender rendered;
  const int width = std::min(metrics.columns, metrics.compact ? metrics.columns : 112);
  const int left = std::max(0, (metrics.columns - width) / 2);
  int cwdSlot = metrics.compact ? 16 : (metrics.wide ? 32 : 24);
  const int userSlot = metrics.compact ? 8 : 10;
  const int txSlot = metrics.compact ? 7 : 8;
  cwdSlot = std::max(10, std::min(cwdSlot, width - userSlot - txSlot - 10));

  const auto cwd = truncate(status.cwd, cwdSlot);
  const auto user = truncate(status.user, userSlot);
  const auto tx = truncate("tx#" + std::to_string(status.txid), txSlot);
  const auto cwdCell = padRight(cwd, cwdSlot);
  const auto userCell = padRight(user, userSlot);
  const auto txCell = padRight(tx, txSlot);
  const std::string plainPrefix = "▌ " + cwdCell + " " + userCell + " " + txCell + " › ";
  const int prefixWidth = displayWidth(plainPrefix);
  const int available = std::max(4, width - prefixWidth);

  std::size_t visibleStart = 0;
  if (cursor > static_cast<std::size_t>(available)) visibleStart = cursor - static_cast<std::size_t>(available);
  if (visibleStart > line.size()) visibleStart = line.size();
  const auto visible = line.substr(visibleStart, static_cast<std::size_t>(available));
  const auto beforeCursor = line.substr(visibleStart, cursor - visibleStart);
  const int commandWidth = displayWidth(visible);

  rendered.cursorColumn = left + prefixWidth + displayWidth(beforeCursor);
  rendered.visibleStart = visibleStart;
  rendered.commandWidth = commandWidth;

  if (!th.ansi || th.mono) {
    rendered.row = std::string(left, ' ') + plainPrefix + visible + std::string(std::max(0, available - commandWidth), ' ');
    return rendered;
  }

  const auto prefix = th.panel2 + th.blue + "▌ " +
                      panelAccent(th, th.amber, cwdCell) + " " +
                      panelAccent(th, th.blue, userCell) + " " +
                      th.dim + txCell + th.white + th.bold + " › " + th.normalWeight;
  rendered.row = std::string(left, ' ') + prefix + th.white + visible +
                 std::string(std::max(0, available - commandWidth), ' ') + th.reset;
  return rendered;
}

std::string renderPrompt(const Theme& th, const TerminalMetrics& metrics, const KernelStatus& status) {
  const auto prompt = renderPromptLine(th, metrics, status, "", 0);
  if (!th.ansi || th.mono) return prompt.row;
  std::ostringstream out;
  out << prompt.row << "\r";
  if (prompt.cursorColumn > 0) out << "\x1b[" << prompt.cursorColumn << "C";
  return out.str();
}

std::string renderResult(const Theme& th, const TerminalMetrics& metrics, const std::string& command, bool ok, const std::string& code, const std::string& message, const std::string& output) {
  std::ostringstream out;
  const auto tone = ok ? "green" : "red";
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  const int left = std::max(0, (metrics.columns - width) / 2);
  const std::string indent(left, ' ');
  if (th.ansi && !th.mono) out << th.reset << "\x1b[2K";
  out << indent << badge(th, ok ? "OK" : code, tone) << " "
      << color(th, th.dim, truncate(command, std::max(20, width - 28)));
  if (!ok) out << " " << color(th, th.red, message);
  if (th.ansi && !th.mono) out << th.reset << "\x1b[0K";
  out << "\n";
  if (!output.empty()) {
    for (const auto& line : splitLines(output)) {
      if (th.ansi && !th.mono) out << th.reset << "\x1b[2K";
      out << indent << line;
      if (th.ansi && !th.mono) out << th.reset << "\x1b[0K";
      out << "\n";
    }
  }
  return out.str();
}

std::string renderDir(const Theme& th, const TerminalMetrics& metrics, const std::string& path, const std::vector<DirRow>& rows) {
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  const int nameWidth = metrics.compact ? 16 : (metrics.wide ? 32 : 24);
  const int typeWidth = 6;
  const std::string detailIndent(metrics.compact ? 3 : nameWidth + 3, ' ');
  std::vector<std::string> list;
  list.push_back(accent(th, th.amber, truncate(path, width - 16)) + "  " + badge(th, std::to_string(rows.size()) + " " + text(th, "entries"), "blue"));
  for (const auto& row : rows) {
    const auto tone = typeTone(row.type);
    const auto nameTone = row.type == "dir" ? th.amber : (row.shared ? th.magenta : th.white);
    const auto nameCell = accent(th, nameTone, padRight(row.name, nameWidth));
    const auto typeCell = color(th, th.white, padRight(row.type, typeWidth));
    const auto meta = color(th, th.dim, "inode ") + accent(th, th.amber, std::to_string(row.inode)) +
                      color(th, th.dim, " " + text(th, "gen") + " ") + color(th, th.white, std::to_string(row.generation)) +
                      color(th, th.dim, " " + text(th, "ref") + " ") + color(th, row.shared ? th.magenta : th.white, std::to_string(row.refcount));
    const auto ownerClass = row.owner + ":" + row.klass;
    const auto blocks = progress(th, row.blockCount, 12, metrics.compact ? 8 : 12, row.shared ? "magenta" : "blue");
    list.push_back(color(th, toneCode(th, tone), iconForType(row.type)) + " " + nameCell + " " + typeCell + " " + meta);
    list.push_back(detailIndent + color(th, th.white, row.mode) + "  " + color(th, th.white, ownerClass) +
                   "  " + color(th, th.dim, text(th, "size") + " ") + accent(th, th.white, std::to_string(row.size)) +
                   "  " + color(th, th.dim, text(th, "blocks") + " ") + blocks);
  }
  return box(th, text(th, "directory"), list, width, "amber") + "\n";
}

std::string renderScope(const Theme& th, const TerminalMetrics& metrics, const KernelStatus& status, const std::vector<InodeRow>& inodeHot, const std::vector<std::pair<std::string, std::string>>& extra) {
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  const int gap = 2;
  const int card = metrics.compact ? width : (width - gap) / 2;
  auto a = box(th, text(th, "kernel"), {
      text(th, "mount") + "  " + color(th, status.mountState == "clean" ? th.green : th.amber, status.mountState),
      "tx     " + color(th, th.white, "#" + std::to_string(status.txid)),
      "root   " + color(th, th.gray, status.mounted ? "visible" : "unmounted")}, card, "amber");
  auto b = box(th, text(th, "capacity"), {
      "inode  " + progress(th, status.inodes, status.inodeTotal, std::max(12, card - 18), "amber") + " " + std::to_string(status.inodes),
      text(th, "blocks") + "  " + progress(th, status.blocks, status.blockTotal, std::max(12, card - 18), "blue") + " " + std::to_string(status.blocks),
      text(th, "snap") + "   " + color(th, th.magenta, std::to_string(status.snapshots))}, card, "blue");
  std::vector<std::string> hot;
  for (const auto& row : inodeHot) {
    hot.push_back(color(th, th.amber, "#" + std::to_string(row.inode)) + " " + padRight(row.type, 5) +
                  " ref=" + color(th, row.refcount > 1 ? th.magenta : th.gray, std::to_string(row.refcount)) +
                  " open=" + std::to_string(row.openCount) + " " + truncate(row.owner + ":" + row.klass, card - 34));
  }
  if (hot.empty()) hot.push_back(color(th, th.dim, text(th, "no_inode_activity")));
  auto c = box(th, text(th, "hot_inodes"), hot, card, "magenta");
  std::vector<std::string> misc;
  for (const auto& item : extra) misc.push_back(color(th, th.gray, item.first) + "  " + color(th, th.white, item.second));
  if (misc.empty()) misc.push_back(text(th, "trace") + "  " + color(th, status.trace ? th.green : th.red, status.trace ? "on" : "off"));
  auto d = box(th, text(th, "session"), misc, card, "border");
  if (metrics.compact) return a + "\n" + b + "\n" + c + "\n" + d + "\n";
  return columns({a, b}, gap) + columns({c, d}, gap);
}

std::string renderTree(const Theme& th, const TerminalMetrics& metrics, const std::vector<std::string>& lines) {
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  std::vector<std::string> out;
  for (auto line : lines) {
    std::string tone = line.find("ref=2") != std::string::npos || line.find("ref=3") != std::string::npos ? "magenta" : "gray";
    out.push_back(color(th, toneCode(th, tone), truncate(line, width - 4)));
  }
  return box(th, text(th, "tree_shared"), out, width, "blue") + "\n";
}

std::string renderMap(const Theme& th, const TerminalMetrics& metrics, const std::string& what, const std::vector<MapCell>& cells, std::uint32_t totalBlocks) {
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  const int mapWidth = std::max(48, width - 8);
  const int rows = metrics.compact ? 12 : 18;
  const int totalCells = mapWidth * rows;
  const int stride = std::max<int>(1, static_cast<int>(totalBlocks) / totalCells);
  std::map<std::uint32_t, MapCell> byBlock;
  for (const auto& c : cells) byBlock[c.block] = c;
  {
    std::vector<std::string> rendered;
    rendered.push_back(color(th, th.dim, "0") + std::string(std::max(0, mapWidth - 12), ' ') + color(th, th.dim, std::to_string(totalBlocks)));
    for (int r = 0; r < rows; ++r) {
      std::string row;
      for (int c = 0; c < mapWidth; ++c) {
        const auto block = static_cast<std::uint32_t>((r * mapWidth + c) * stride);
        const auto it = byBlock.find(block);
        MapCell cell;
        if (it != byBlock.end()) cell = it->second;
        else cell.block = block;
        row += color(th, toneCode(th, mapTone(th, what, cell)), mapGlyph(what, cell));
      }
      rendered.push_back(row);
    }
    rendered.push_back(mapLegend(th, what));

    std::vector<std::string> highlights;
    if (what == "journal") {
      for (const auto& c : cells) {
        if (c.lastWriterTxid == 0) continue;
        highlights.push_back("block " + color(th, th.amber, std::to_string(c.block)) +
                             " tx=" + std::to_string(c.lastWriterTxid) +
                             " owner=#" + std::to_string(c.ownerInode));
        if (highlights.size() >= 4) break;
      }
      if (!highlights.empty()) rendered.push_back(color(th, th.amber, "tx-written blocks"));
    } else {
      for (const auto& c : cells) {
        if (c.refcount <= 1) continue;
        highlights.push_back("block " + color(th, th.magenta, std::to_string(c.block)) +
                             " ref=" + std::to_string(c.refcount) +
                             " owner=#" + std::to_string(c.ownerInode));
        if (highlights.size() >= 4) break;
      }
      if (!highlights.empty()) rendered.push_back(color(th, th.magenta, "shared hotspots"));
    }
    rendered.insert(rendered.end(), highlights.begin(), highlights.end());
    return box(th, text(th, "disk_map") + " / " + what, rendered, width, what == "journal" ? "green" : (what == "inode" ? "blue" : "amber")) + "\n";
  }
}

std::string renderTraceTimeline(const Theme& th, const TerminalMetrics& metrics, const std::vector<TraceEvent>& events, const std::string& title) {
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  std::vector<std::string> lines;
  for (const auto& e : events) {
    const auto tone = eventTone(e);
    std::string branch = e.type.find("path.lookup") != std::string::npos ? "  └─" : "●";
    if (e.type.find("journal") != std::string::npos) branch = "◆";
    if (e.status == "deny" || e.status == "crash") branch = "×";
    std::ostringstream line;
    line << color(th, toneCode(th, tone), branch) << " "
         << color(th, th.dim, "#" + std::to_string(e.seq)) << " "
         << color(th, th.amber, "tx" + std::to_string(e.txid)) << " "
         << color(th, th.white, truncate(e.type, 24)) << " "
         << color(th, th.gray, truncate(e.object, width - 58)) << " "
         << badge(th, e.status, tone);
    lines.push_back(line.str());
  }
  if (lines.empty()) lines.push_back(color(th, th.dim, th.lang == "zh" ? "暂无 trace 事件" : "no trace events"));
  std::string translatedTitle = title;
  if (title == "Trace timeline") translatedTitle = text(th, "trace_timeline");
  else if (title == "Trace replay / read-only") translatedTitle = text(th, "trace_replay");
  else if (title == "Trace step") translatedTitle = text(th, "trace_step");
  return box(th, translatedTitle, lines, width, "blue") + "\n";
}

std::string renderReadData(const Theme& th, const TerminalMetrics& metrics, const std::string& data, std::uint64_t oldOffset, std::uint64_t newOffset) {
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  const int dataWidth = std::max(24, width - 8);
  std::string shown = truncate(data, dataWidth);
  const auto advanced = newOffset >= oldOffset ? newOffset - oldOffset : 0;
  std::string ruler(dataWidth, ' ');
  for (int i = 0; i < dataWidth; i += 8) {
    const auto tick = std::to_string(oldOffset + static_cast<std::uint64_t>(i));
    for (std::size_t j = 0; j < tick.size() && i + static_cast<int>(j) < dataWidth; ++j) ruler[i + j] = tick[j];
  }
  std::string pointer(dataWidth, ' ');
  const auto pos = static_cast<int>(std::min<std::uint64_t>(advanced, dataWidth ? dataWidth - 1 : 0));
  if (pos >= 0 && pos < dataWidth) pointer[pos] = '^';
  return box(th, text(th, "read_result"), {
      color(th, th.dim, ruler),
      color(th, th.white, padRight(shown, dataWidth)),
      color(th, th.blue, pointer),
      color(th, th.gray, text(th, "read_advance") + " +" + std::to_string(advanced) +
          "  " + text(th, "fd_offset") + " " + std::to_string(oldOffset) + " -> " + std::to_string(newOffset))}, width, "blue") + "\n";
}

std::string renderSnapshotDiff(const Theme& th, const TerminalMetrics& metrics, const std::vector<std::string>& diffLines) {
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  std::vector<std::string> lines;
  for (const auto& raw : diffLines) {
    std::string tone = "gray";
    if (!raw.empty() && raw[0] == '+') tone = "green";
    else if (!raw.empty() && raw[0] == '-') tone = "red";
    else if (!raw.empty() && raw[0] == '~') tone = "amber";
    lines.push_back(color(th, toneCode(th, tone), truncate(raw, width - 4)) + (tone == "amber" ? color(th, th.magenta, "  COW") : ""));
  }
  if (lines.empty()) lines.push_back(color(th, th.green, th.lang == "zh" ? "无差异" : "no differences"));
  return box(th, text(th, "snapshot_diff"), lines, width, "magenta") + "\n";
}

std::string renderClassGraph(const Theme& th, const TerminalMetrics& metrics, const std::vector<std::string>& linesIn) {
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  std::vector<std::string> lines;
  for (auto line : linesIn) {
    std::string tone = line.find("inherits") != std::string::npos ? "magenta" : "amber";
    lines.push_back(color(th, toneCode(th, tone), "◇ ") + truncate(line, width - 6));
  }
  return box(th, text(th, "class_graph"), lines, width, "magenta") + "\n";
}

std::string renderAclGraph(const Theme& th, const TerminalMetrics& metrics, const std::string& title, const std::vector<std::string>& linesIn) {
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  std::vector<std::string> lines;
  for (auto line : linesIn) {
    std::string tone = line.find("rights=") != std::string::npos ? "blue" : "gray";
    lines.push_back(color(th, toneCode(th, tone), "● ") + truncate(line, width - 6));
  }
  if (lines.empty()) lines.push_back(color(th, th.dim, th.lang == "zh" ? "暂无 ACL grant" : "no ACL grants"));
  return box(th, title == "ACL graph" ? text(th, "acl_graph") : title, lines, width, "blue") + "\n";
}

} // namespace scopefs::ui
