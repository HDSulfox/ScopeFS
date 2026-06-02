#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "scopefs/model.hpp"
#include "scopefs/trace.hpp"

namespace scopefs::ui {

struct TerminalMetrics {
  int columns = 118;
  int rows = 32;
  bool compact = false;
  bool wide = false;
};

struct Theme {
  bool ansi = true;
  bool mono = false;
  std::string reset;
  std::string bg;
  std::string panel;
  std::string panel2;
  std::string amber;
  std::string blue;
  std::string green;
  std::string red;
  std::string magenta;
  std::string gray;
  std::string dim;
  std::string white;
  std::string border;
};

struct KernelStatus {
  bool mounted = false;
  std::string user = "-";
  std::string cwd = "/";
  std::string mountState = "unmounted";
  std::uint64_t txid = 0;
  std::size_t inodes = 0;
  std::size_t inodeTotal = 0;
  std::size_t blocks = 0;
  std::size_t blockTotal = 0;
  std::size_t snapshots = 0;
  std::size_t openFiles = 0;
  bool trace = true;
};

struct DirRow {
  std::string name;
  std::string type;
  std::string owner;
  std::string klass;
  std::string mode;
  std::uint32_t inode = 0;
  std::uint32_t generation = 0;
  std::uint64_t size = 0;
  std::size_t blockCount = 0;
  std::uint32_t refcount = 0;
  bool shared = false;
};

struct InodeRow {
  std::uint32_t inode = 0;
  std::string type;
  std::uint32_t generation = 0;
  std::uint32_t refcount = 0;
  std::uint32_t openCount = 0;
  std::string owner;
  std::string klass;
  std::uint64_t size = 0;
  std::size_t blockCount = 0;
  bool pending = false;
};

struct MapCell {
  std::uint32_t block = 0;
  std::uint32_t refcount = 0;
  std::uint32_t ownerInode = 0;
  std::uint64_t lastWriterTxid = 0;
  std::string flags;
};

TerminalMetrics detectMetrics();
Theme theme(bool ansi, const std::string& name = "scope-dark");

int displayWidth(const std::string& text);
std::string stripAnsi(const std::string& text);
std::string truncate(const std::string& text, int width);
std::string padRight(const std::string& text, int width);
std::string padLeft(const std::string& text, int width);
std::string center(const std::string& text, int width);
std::string color(const Theme& th, const std::string& code, const std::string& text);
std::string badge(const Theme& th, const std::string& label, const std::string& tone);
std::string progress(const Theme& th, std::size_t used, std::size_t total, int width, const std::string& tone);
std::string box(const Theme& th, const std::string& title, const std::vector<std::string>& lines, int width, const std::string& tone = "border");
std::string columns(const std::vector<std::string>& boxed, int gap = 2);

std::string renderDashboard(const Theme& th, const TerminalMetrics& metrics, const KernelStatus& status);
std::string renderPrompt(const Theme& th, const TerminalMetrics& metrics, const KernelStatus& status);
std::string renderResult(const Theme& th, const TerminalMetrics& metrics, const std::string& command, bool ok, const std::string& code, const std::string& message, const std::string& output);
std::string renderDir(const Theme& th, const TerminalMetrics& metrics, const std::string& path, const std::vector<DirRow>& rows);
std::string renderScope(const Theme& th, const TerminalMetrics& metrics, const KernelStatus& status, const std::vector<InodeRow>& inodeHot, const std::vector<std::pair<std::string, std::string>>& extra);
std::string renderTree(const Theme& th, const TerminalMetrics& metrics, const std::vector<std::string>& lines);
std::string renderMap(const Theme& th, const TerminalMetrics& metrics, const std::string& what, const std::vector<MapCell>& cells, std::uint32_t totalBlocks);
std::string renderTraceTimeline(const Theme& th, const TerminalMetrics& metrics, const std::vector<TraceEvent>& events, const std::string& title = "Trace timeline");
std::string renderReadData(const Theme& th, const TerminalMetrics& metrics, const std::string& data, std::uint64_t oldOffset, std::uint64_t newOffset);
std::string renderSnapshotDiff(const Theme& th, const TerminalMetrics& metrics, const std::vector<std::string>& diffLines);
std::string renderClassGraph(const Theme& th, const TerminalMetrics& metrics, const std::vector<std::string>& lines);
std::string renderAclGraph(const Theme& th, const TerminalMetrics& metrics, const std::string& title, const std::vector<std::string>& lines);

} // namespace scopefs::ui
