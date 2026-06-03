#include "scopefs/trace.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "scopefs/util.hpp"

namespace scopefs {

namespace {

std::string jsonEscape(const std::string& value) {
  std::ostringstream out;
  for (char ch : value) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<int>(static_cast<unsigned char>(ch));
        } else {
          out << ch;
        }
    }
  }
  return out.str();
}

std::string jsonStringValue(const std::string& line, const std::string& key) {
  const std::string marker = "\"" + key + "\":\"";
  const auto pos = line.find(marker);
  if (pos == std::string::npos) return "";
  std::string out;
  for (std::size_t i = pos + marker.size(); i < line.size(); ++i) {
    const char ch = line[i];
    if (ch == '"') break;
    if (ch == '\\' && i + 1 < line.size()) {
      const char next = line[++i];
      if (next == 'n') out.push_back('\n');
      else if (next == 'r') out.push_back('\r');
      else if (next == 't') out.push_back('\t');
      else out.push_back(next);
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

std::uint64_t jsonNumberValue(const std::string& line, const std::string& key) {
  const std::string marker = "\"" + key + "\":";
  const auto pos = line.find(marker);
  if (pos == std::string::npos) return 0;
  std::size_t end = pos + marker.size();
  while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end]))) ++end;
  return std::strtoull(line.substr(pos + marker.size(), end - (pos + marker.size())).c_str(), nullptr, 10);
}

} // namespace

void TraceSink::setEnabled(bool enabled) { enabled_ = enabled; }
bool TraceSink::enabled() const { return enabled_; }

void TraceSink::setContext(std::string user, std::string command) {
  currentUser_ = std::move(user);
  currentCommand_ = std::move(command);
}

void TraceSink::emit(std::uint64_t txid,
                     const std::string& type,
                     const std::string& object,
                     const std::string& before,
                     const std::string& after,
                     const std::string& reason,
                     const std::string& status) {
  if (type.rfind("crash.", 0) == 0 || type.rfind("coord.signal.", 0) == 0) return;
  if (!enabled_ && type.rfind("trace.", 0) != 0) return;
  TraceEvent event;
  event.seq = seq_++;
  event.txid = txid;
  event.ts = nowIso();
  event.session = currentUser_.empty() ? "-" : currentUser_;
  event.user = currentUser_.empty() ? "-" : currentUser_;
  event.command = currentCommand_.empty() ? "-" : currentCommand_;
  event.type = type;
  event.object = object;
  event.before = before;
  event.after = after;
  event.reason = reason;
  event.status = status;
  ring_.push_back(event);
  while (ring_.size() > config::kTraceRing) ring_.pop_front();
}

std::vector<TraceEvent> TraceSink::recent(std::size_t n) const {
  std::vector<TraceEvent> out;
  if (n == 0 || n > ring_.size()) n = ring_.size();
  out.reserve(n);
  auto begin = ring_.end() - static_cast<std::ptrdiff_t>(n);
  for (auto it = begin; it != ring_.end(); ++it) out.push_back(*it);
  return out;
}

void TraceSink::clear() {
  ring_.clear();
  emit(0, "trace.clear", "ring", "-", "-", "manual", "ok");
}

bool TraceSink::save(const std::string& path, std::string* err) const {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (err) *err = "cannot open trace output: " + path;
    return false;
  }
  for (const auto& event : ring_) out << toJsonLine(event) << '\n';
  return true;
}

std::vector<TraceEvent> TraceSink::load(const std::string& path) {
  std::vector<TraceEvent> out;
  std::ifstream in(path, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    if (!trim(line).empty()) out.push_back(fromJsonLine(line));
  }
  return out;
}

std::string TraceSink::toJsonLine(const TraceEvent& event) {
  std::ostringstream out;
  out << "{\"seq\":" << event.seq
      << ",\"txid\":" << event.txid
      << ",\"ts\":\"" << jsonEscape(event.ts)
      << "\",\"session\":\"" << jsonEscape(event.session)
      << "\",\"user\":\"" << jsonEscape(event.user)
      << "\",\"command\":\"" << jsonEscape(event.command)
      << "\",\"type\":\"" << jsonEscape(event.type)
      << "\",\"object\":\"" << jsonEscape(event.object)
      << "\",\"before\":\"" << jsonEscape(event.before)
      << "\",\"after\":\"" << jsonEscape(event.after)
      << "\",\"reason\":\"" << jsonEscape(event.reason)
      << "\",\"status\":\"" << jsonEscape(event.status)
      << "\"}";
  return out.str();
}

TraceEvent TraceSink::fromJsonLine(const std::string& line) {
  TraceEvent event;
  event.seq = jsonNumberValue(line, "seq");
  event.txid = jsonNumberValue(line, "txid");
  event.ts = jsonStringValue(line, "ts");
  event.session = jsonStringValue(line, "session");
  event.user = jsonStringValue(line, "user");
  event.command = jsonStringValue(line, "command");
  event.type = jsonStringValue(line, "type");
  event.object = jsonStringValue(line, "object");
  event.before = jsonStringValue(line, "before");
  event.after = jsonStringValue(line, "after");
  event.reason = jsonStringValue(line, "reason");
  event.status = jsonStringValue(line, "status");
  return event;
}

std::uint64_t TraceSink::nextSeq() const { return seq_; }

void renderTraceEvents(std::ostream& out, const std::vector<TraceEvent>& events) {
  out << "┌──────┬──────┬────────────────────────┬────────────────────┬────────────┐\n";
  out << "│ seq  │ txid │ type                   │ object             │ status     │\n";
  out << "├──────┼──────┼────────────────────────┼────────────────────┼────────────┤\n";
  for (const auto& e : events) {
    auto clip = [](std::string s, std::size_t n) {
      if (s.size() <= n) return s;
      return s.substr(0, n > 1 ? n - 1 : n) + "…";
    };
    out << "│ " << std::setw(4) << e.seq
        << " │ " << std::setw(4) << e.txid
        << " │ " << std::left << std::setw(22) << clip(e.type, 22)
        << " │ " << std::setw(18) << clip(e.object, 18)
        << " │ " << std::setw(10) << clip(e.status, 10)
        << std::right << " │\n";
  }
  out << "└──────┴──────┴────────────────────────┴────────────────────┴────────────┘\n";
}

} // namespace scopefs
