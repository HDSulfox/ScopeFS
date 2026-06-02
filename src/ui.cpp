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

Theme theme(bool ansi, const std::string& name) {
  Theme th;
  th.ansi = ansi;
  th.mono = name == "mono" || !ansi;
  if (!ansi) return th;
  th.reset = esc("0");
  th.bg = esc("48;2;11;12;14");
  th.panel = esc("48;2;28;29;33");
  th.panel2 = esc("48;2;36;36;40");
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

std::string badge(const Theme& th, const std::string& label, const std::string& tone) {
  const auto text = " " + label + " ";
  if (!th.ansi || th.mono) return "[" + label + "]";
  return toneCode(th, tone) + text + th.reset;
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
  const std::vector<std::string> logoLarge = {
      "███████╗ ██████╗ ██████╗ ██████╗ ███████╗███████╗███████╗",
      "██╔════╝██╔════╝██╔═══██╗██╔══██╗██╔════╝██╔════╝██╔════╝",
      "███████╗██║     ██║   ██║██████╔╝█████╗  █████╗  ███████╗",
      "╚════██║██║     ██║   ██║██╔═══╝ ██╔══╝  ██╔══╝  ╚════██║",
      "███████║╚██████╗╚██████╔╝██║     ███████╗██║     ███████║",
      "╚══════╝ ╚═════╝ ╚═════╝ ╚═╝     ╚══════╝╚═╝     ╚══════╝"};
  std::ostringstream out;
  if (th.ansi) out << th.bg;
  out << "\n";
  if (metrics.compact) {
    out << indent << color(th, th.amber, center("SCOPEFS", width)) << "\n";
  } else {
    for (const auto& line : logoLarge) out << indent << color(th, th.white, center(line, width)) << "\n";
  }
  out << "\n";
  const int panelWidth = metrics.compact ? width : std::min(width - 8, 96);
  const int panelLeft = std::max(0, (metrics.columns - panelWidth) / 2);
  std::vector<std::string> command = {
      rawFg(th.amber, "/scope") + rawFg(th.gray, "  command surface") + th.reset,
      rawFg(th.white, "› ") + rawFg(th.gray, "format  ·  login root root  ·  scope tree  ·  map refcount") + th.reset,
      rawFg(th.blue, "Build") + rawFg(th.white, "  observable inode/block/journal/COW kernel ") +
          rawFg(th.dim, "terminal-first") + th.reset};
  out << indentBlock(box(th, "command focus", command, panelWidth, "amber"), panelLeft) << "\n\n";
  const int cardGap = 2;
  const int cardWidth = metrics.compact ? width : (width - 2 * cardGap) / 3;
  auto volume = box(th, "Volume", {
      "mount  " + color(th, status.mountState == "clean" ? th.green : th.amber, status.mountState),
      "tx     " + color(th, th.white, "#" + std::to_string(status.txid)),
      "blocks " + progress(th, status.blocks, status.blockTotal, std::max(10, cardWidth - 18), "blue")}, cardWidth, "border");
  auto session = box(th, "Session", {
      "user   " + color(th, th.white, status.user),
      "cwd    " + color(th, th.gray, truncate(status.cwd, cardWidth - 12)),
      "open   " + color(th, th.white, std::to_string(status.openFiles))}, cardWidth, "blue");
  auto obs = box(th, "Observability", {
      "trace  " + color(th, status.trace ? th.green : th.red, status.trace ? "on" : "off"),
      "inode  " + progress(th, status.inodes, status.inodeTotal, std::max(10, cardWidth - 18), "amber"),
      "snap   " + color(th, th.magenta, std::to_string(status.snapshots))}, cardWidth, "magenta");
  if (metrics.compact) {
    out << indentBlock(volume, left) << "\n" << indentBlock(session, left) << "\n" << indentBlock(obs, left) << "\n";
  } else {
    out << indentBlock(columns({volume, session, obs}, cardGap), left);
  }
  out << indent << color(th, th.dim, "tips  tab commands   ctrl+p palette   trace show   snapshot diff   fsck") << "\n\n";
  if (th.ansi) out << th.reset;
  return out.str();
}

std::string renderPrompt(const Theme& th, const TerminalMetrics& metrics, const KernelStatus& status) {
  const int width = std::min(metrics.columns, metrics.compact ? metrics.columns : 112);
  const int left = std::max(0, (metrics.columns - width) / 2);
  const auto leftInfo = " " + truncate(status.cwd, std::max(12, width / 3)) + " ";
  const auto mid = status.user + "  tx#" + std::to_string(status.txid);
  std::ostringstream out;
  if (!th.ansi || th.mono) {
    out << std::string(left, ' ') << "▌" << leftInfo << padRight(mid, std::max(16, width / 4)) << " › ";
    return out.str();
  }
  const auto cursor = th.panel2 + th.blue + "▌" + th.amber + leftInfo +
                      th.dim + padRight(mid, std::max(16, width / 4)) + th.white + " › ";
  const int cursorWidth = displayWidth(cursor);
  out << std::string(left, ' ') << cursor << std::string(std::max(0, width - cursorWidth), ' ')
      << th.reset << "\r" << std::string(left, ' ') << cursor;
  return out.str();
}

std::string renderResult(const Theme& th, const TerminalMetrics& metrics, const std::string& command, bool ok, const std::string& code, const std::string& message, const std::string& output) {
  std::ostringstream out;
  const auto tone = ok ? "green" : "red";
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  const int left = std::max(0, (metrics.columns - width) / 2);
  const std::string indent(left, ' ');
  out << indent << badge(th, ok ? "OK" : code, tone) << " "
      << color(th, th.dim, truncate(command, std::max(20, width - 28)));
  if (!ok) out << " " << color(th, th.red, message);
  out << "\n";
  if (!output.empty()) {
    for (const auto& line : splitLines(output)) out << indent << line << "\n";
  }
  return out.str();
}

std::string renderDir(const Theme& th, const TerminalMetrics& metrics, const std::string& path, const std::vector<DirRow>& rows) {
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  std::vector<std::string> list;
  list.push_back(color(th, th.dim, truncate(path, width - 8)) + "  " + badge(th, std::to_string(rows.size()) + " entries", "blue"));
  for (const auto& row : rows) {
    const auto tone = typeTone(row.type);
    const auto title = color(th, toneCode(th, tone), iconForType(row.type)) + " " + color(th, th.white, truncate(row.name, metrics.compact ? 22 : 32));
    const auto meta = color(th, th.dim, "inode ") + color(th, th.amber, std::to_string(row.inode)) +
                      color(th, th.dim, " gen ") + std::to_string(row.generation) +
                      color(th, th.dim, " ref ") + color(th, row.shared ? th.magenta : th.gray, std::to_string(row.refcount));
    const auto mode = color(th, th.dim, row.mode + "  " + row.owner + ":" + row.klass);
    const auto blocks = progress(th, row.blockCount, 12, metrics.compact ? 8 : 12, row.shared ? "magenta" : "blue");
    list.push_back(title + "  " + badge(th, row.type, tone) + "  " + meta);
    list.push_back("   " + mode + "  size " + color(th, th.white, std::to_string(row.size)) + "  blocks " + blocks);
  }
  return box(th, "Directory", list, width, "amber") + "\n";
}

std::string renderScope(const Theme& th, const TerminalMetrics& metrics, const KernelStatus& status, const std::vector<InodeRow>& inodeHot, const std::vector<std::pair<std::string, std::string>>& extra) {
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  const int gap = 2;
  const int card = metrics.compact ? width : (width - gap) / 2;
  auto a = box(th, "Kernel", {
      "mount  " + color(th, status.mountState == "clean" ? th.green : th.amber, status.mountState),
      "tx     " + color(th, th.white, "#" + std::to_string(status.txid)),
      "root   " + color(th, th.gray, status.mounted ? "visible" : "unmounted")}, card, "amber");
  auto b = box(th, "Capacity", {
      "inode  " + progress(th, status.inodes, status.inodeTotal, std::max(12, card - 18), "amber") + " " + std::to_string(status.inodes),
      "block  " + progress(th, status.blocks, status.blockTotal, std::max(12, card - 18), "blue") + " " + std::to_string(status.blocks),
      "snap   " + color(th, th.magenta, std::to_string(status.snapshots))}, card, "blue");
  std::vector<std::string> hot;
  for (const auto& row : inodeHot) {
    hot.push_back(color(th, th.amber, "#" + std::to_string(row.inode)) + " " + padRight(row.type, 5) +
                  " ref=" + color(th, row.refcount > 1 ? th.magenta : th.gray, std::to_string(row.refcount)) +
                  " open=" + std::to_string(row.openCount) + " " + truncate(row.owner + ":" + row.klass, card - 34));
  }
  if (hot.empty()) hot.push_back(color(th, th.dim, "no inode activity"));
  auto c = box(th, "Hot inodes", hot, card, "magenta");
  std::vector<std::string> misc;
  for (const auto& item : extra) misc.push_back(color(th, th.gray, item.first) + "  " + color(th, th.white, item.second));
  if (misc.empty()) misc.push_back("trace  " + color(th, status.trace ? th.green : th.red, status.trace ? "on" : "off"));
  auto d = box(th, "Session", misc, card, "border");
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
  return box(th, "Tree / shared references", out, width, "blue") + "\n";
}

std::string renderMap(const Theme& th, const TerminalMetrics& metrics, const std::string& what, const std::vector<MapCell>& cells, std::uint32_t totalBlocks) {
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  const int mapWidth = std::max(48, width - 8);
  const int rows = metrics.compact ? 12 : 18;
  const int totalCells = mapWidth * rows;
  const int stride = std::max<int>(1, static_cast<int>(totalBlocks) / totalCells);
  std::map<std::uint32_t, MapCell> byBlock;
  for (const auto& c : cells) byBlock[c.block] = c;
  std::vector<std::string> lines;
  lines.push_back(color(th, th.dim, "0") + std::string(std::max(0, mapWidth - 12), ' ') + color(th, th.dim, std::to_string(totalBlocks)));
  for (int r = 0; r < rows; ++r) {
    std::string row;
    for (int c = 0; c < mapWidth; ++c) {
      const auto block = static_cast<std::uint32_t>((r * mapWidth + c) * stride);
      const auto it = byBlock.find(block);
      MapCell cell;
      if (it != byBlock.end()) cell = it->second;
      else cell.block = block;
      if (what == "blocks" || what == "inode" || what == "journal") {
        std::string glyph = "·";
        if (cell.flags == "super") glyph = "S";
        else if (cell.flags == "refcount") glyph = "R";
        else if (cell.flags == "inode") glyph = "I";
        else if (cell.flags == "journal") glyph = "J";
        else if (cell.flags == "snapshot") glyph = "P";
        else if (cell.flags == "user") glyph = "U";
        else if (cell.refcount) glyph = "D";
        row += color(th, toneCode(th, blockTone(cell)), glyph);
      } else if (what == "owner") {
        const std::string glyph = cell.ownerInode ? std::string(1, static_cast<char>('A' + (cell.ownerInode % 26))) : "·";
        row += color(th, toneCode(th, blockTone(cell)), glyph);
      } else {
        row += color(th, toneCode(th, blockTone(cell)), blockGlyph(cell.refcount));
      }
    }
    lines.push_back(row);
  }
  lines.push_back(color(th, th.dim, "legend ") + color(th, th.amber, "R refcount ") + color(th, th.blue, "I inode ") +
                  color(th, th.green, "J journal ") + color(th, th.magenta, "▒█ shared ") + color(th, th.gray, "░ allocated · free"));
  std::vector<std::string> hot;
  for (const auto& c : cells) {
    if (c.refcount > 1) hot.push_back("block " + color(th, th.magenta, std::to_string(c.block)) + " ref=" + std::to_string(c.refcount) + " owner=#" + std::to_string(c.ownerInode));
    if (hot.size() >= 4) break;
  }
  if (!hot.empty()) {
    lines.push_back(color(th, th.magenta, "shared hotspots"));
    lines.insert(lines.end(), hot.begin(), hot.end());
  }
  return box(th, "Disk map / " + what, lines, width, "amber") + "\n";
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
  if (lines.empty()) lines.push_back(color(th, th.dim, "no trace events"));
  return box(th, title, lines, width, "blue") + "\n";
}

std::string renderReadData(const Theme& th, const TerminalMetrics& metrics, const std::string& data, std::uint64_t oldOffset, std::uint64_t newOffset) {
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  const int dataWidth = std::max(24, width - 8);
  std::string shown = truncate(data, dataWidth);
  std::string ruler(dataWidth, ' ');
  for (int i = 0; i < dataWidth; i += 8) {
    const auto tick = std::to_string(i);
    for (std::size_t j = 0; j < tick.size() && i + static_cast<int>(j) < dataWidth; ++j) ruler[i + j] = tick[j];
  }
  std::string pointer(dataWidth, ' ');
  const auto pos = static_cast<int>(std::min<std::uint64_t>(newOffset, dataWidth ? dataWidth - 1 : 0));
  if (pos >= 0 && pos < dataWidth) pointer[pos] = '^';
  return box(th, "Read offset", {
      color(th, th.dim, ruler),
      color(th, th.white, padRight(shown, dataWidth)),
      color(th, th.blue, pointer) + " fd " + std::to_string(oldOffset) + " -> " + std::to_string(newOffset)}, width, "blue") + "\n";
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
  if (lines.empty()) lines.push_back(color(th, th.green, "no differences"));
  return box(th, "Snapshot diff", lines, width, "magenta") + "\n";
}

std::string renderClassGraph(const Theme& th, const TerminalMetrics& metrics, const std::vector<std::string>& linesIn) {
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  std::vector<std::string> lines;
  for (auto line : linesIn) {
    std::string tone = line.find("inherits") != std::string::npos ? "magenta" : "amber";
    lines.push_back(color(th, toneCode(th, tone), "◇ ") + truncate(line, width - 6));
  }
  return box(th, "Identity class graph", lines, width, "magenta") + "\n";
}

std::string renderAclGraph(const Theme& th, const TerminalMetrics& metrics, const std::string& title, const std::vector<std::string>& linesIn) {
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  std::vector<std::string> lines;
  for (auto line : linesIn) {
    std::string tone = line.find("rights=") != std::string::npos ? "blue" : "gray";
    lines.push_back(color(th, toneCode(th, tone), "● ") + truncate(line, width - 6));
  }
  if (lines.empty()) lines.push_back(color(th, th.dim, "no ACL grants"));
  return box(th, title, lines, width, "blue") + "\n";
}

} // namespace scopefs::ui
