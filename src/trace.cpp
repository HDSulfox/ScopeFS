#include "scopefs/trace.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
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

TraceSink::Span::Span(TraceSink* sink, std::uint64_t seq) : sink_(sink), seq_(seq), active_(sink != nullptr && seq != 0) {}

TraceSink::Span::Span(Span&& other) noexcept
    : sink_(other.sink_), seq_(other.seq_), active_(other.active_) {
  other.sink_ = nullptr;
  other.seq_ = 0;
  other.active_ = false;
}

TraceSink::Span& TraceSink::Span::operator=(Span&& other) noexcept {
  if (this == &other) return *this;
  if (active_ && sink_) sink_->finishSpan(seq_, "", "", "", "", "");
  sink_ = other.sink_;
  seq_ = other.seq_;
  active_ = other.active_;
  other.sink_ = nullptr;
  other.seq_ = 0;
  other.active_ = false;
  return *this;
}

TraceSink::Span::~Span() {
  if (active_ && sink_) sink_->finishSpan(seq_, "", "", "", "", "");
}

bool TraceSink::Span::active() const { return active_; }

std::uint64_t TraceSink::Span::seq() const { return seq_; }

void TraceSink::Span::setTxid(std::uint64_t txid) {
  if (!active_ || !sink_) return;
  sink_->setSpanTxid(seq_, txid);
}

void TraceSink::Span::update(const std::string& object,
                             const std::string& before,
                             const std::string& after,
                             const std::string& reason,
                             const std::string& status) {
  if (!active_ || !sink_) return;
  sink_->updateSpan(seq_, object, before, after, reason, status);
}

void TraceSink::Span::finish(const std::string& object,
                             const std::string& before,
                             const std::string& after,
                             const std::string& reason,
                             const std::string& status) {
  if (!active_ || !sink_) return;
  sink_->finishSpan(seq_, object, before, after, reason, status);
  active_ = false;
}

void TraceSink::setEnabled(bool enabled) { enabled_ = enabled; }
bool TraceSink::enabled() const { return enabled_; }

void TraceSink::setContext(std::string user, std::string command) {
  currentUser_ = std::move(user);
  currentCommand_ = std::move(command);
}

bool TraceSink::filtered(const std::string& type) const {
  if (type.rfind("crash.", 0) == 0 || type.rfind("coord.signal.", 0) == 0) return true;
  if (!enabled_ && type.rfind("trace.", 0) != 0) return true;
  return false;
}

std::uint64_t TraceSink::beginSpan(std::uint64_t txid,
                                   const std::string& type,
                                   const std::string& object,
                                   const std::string& before,
                                   const std::string& after,
                                   const std::string& reason,
                                   const std::string& status) {
  if (filtered(type)) return 0;
  TraceEvent event;
  event.seq = seq_++;
  event.txid = txid;
  event.parentSeq = stack_.empty() ? 0 : stack_.back();
  event.depth = static_cast<int>(stack_.size());
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
  stack_.push_back(event.seq);
  return event.seq;
}

void TraceSink::finishSpan(std::uint64_t seq,
                           const std::string& object,
                           const std::string& before,
                           const std::string& after,
                           const std::string& reason,
                           const std::string& status) {
  if (seq == 0) return;
  for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
    if (*it == seq) {
      stack_.erase(std::next(it).base());
      break;
    }
  }
  for (auto& event : ring_) {
    if (event.seq != seq) continue;
    if (!object.empty()) event.object = object;
    if (!before.empty()) event.before = before;
    if (!after.empty()) event.after = after;
    if (!reason.empty()) event.reason = reason;
    if (!status.empty()) event.status = status;
    return;
  }
}

void TraceSink::updateSpan(std::uint64_t seq,
                           const std::string& object,
                           const std::string& before,
                           const std::string& after,
                           const std::string& reason,
                           const std::string& status) {
  if (seq == 0) return;
  for (auto& event : ring_) {
    if (event.seq != seq) continue;
    if (!object.empty()) event.object = object;
    if (!before.empty()) event.before = before;
    if (!after.empty()) event.after = after;
    if (!reason.empty()) event.reason = reason;
    if (!status.empty()) event.status = status;
    return;
  }
}

void TraceSink::setSpanTxid(std::uint64_t seq, std::uint64_t txid) {
  if (seq == 0) return;
  for (auto& event : ring_) {
    if (event.seq == seq) {
      event.txid = txid;
      return;
    }
  }
}

TraceSink::Span TraceSink::span(std::uint64_t txid,
                                const std::string& type,
                                const std::string& object,
                                const std::string& before,
                                const std::string& after,
                                const std::string& reason,
                                const std::string& status) {
  return Span(this, beginSpan(txid, type, object, before, after, reason, status));
}

void TraceSink::emit(std::uint64_t txid,
                     const std::string& type,
                     const std::string& object,
                     const std::string& before,
                     const std::string& after,
                     const std::string& reason,
                     const std::string& status) {
  if (filtered(type)) return;
  for (auto stackIt = stack_.rbegin(); stackIt != stack_.rend(); ++stackIt) {
    for (auto it = ring_.rbegin(); it != ring_.rend(); ++it) {
      if (it->seq != *stackIt) continue;
      if (it->type == type && it->status == "start") {
        it->txid = txid;
        it->object = object;
        it->before = before;
        it->after = after;
        it->reason = reason;
        it->status = status;
        return;
      }
      break;
    }
  }
  TraceEvent event;
  event.seq = seq_++;
  event.txid = txid;
  event.parentSeq = stack_.empty() ? 0 : stack_.back();
  event.depth = static_cast<int>(stack_.size());
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

std::vector<TraceEvent> TraceSink::since(std::uint64_t firstSeq) const {
  std::vector<TraceEvent> out;
  for (const auto& event : ring_) {
    if (event.seq >= firstSeq) out.push_back(event);
  }
  return out;
}

std::vector<TraceEvent> TraceSink::sinceForCommand(std::uint64_t firstSeq, const std::string& command) const {
  std::vector<TraceEvent> out;
  for (const auto& event : ring_) {
    if (event.seq >= firstSeq && event.command == command) out.push_back(event);
  }
  return out;
}

void TraceSink::clear() {
  ring_.clear();
  stack_.clear();
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
      << ",\"parent\":" << event.parentSeq
      << ",\"depth\":" << event.depth
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
  event.parentSeq = jsonNumberValue(line, "parent");
  event.depth = static_cast<int>(jsonNumberValue(line, "depth"));
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
  out << "┌──────┬────────────────────────────┬────────────────────┬────────────┐\n";
  out << "│ seq  │ type                       │ object             │ status     │\n";
  out << "├──────┼────────────────────────────┼────────────────────┼────────────┤\n";
  for (const auto& e : events) {
    auto clip = [](std::string s, std::size_t n) {
      if (s.size() <= n) return s;
      return s.substr(0, n > 1 ? n - 1 : n) + "…";
    };
    const auto type = e.txid == 0 ? e.type : "tx#" + std::to_string(e.txid) + " " + e.type;
    out << "│ " << std::setw(4) << e.seq
        << " │ " << std::left << std::setw(26) << clip(type, 26)
        << " │ " << std::setw(18) << clip(e.object, 18)
        << " │ " << std::setw(10) << clip(e.status, 10)
        << std::right << " │\n";
  }
  out << "└──────┴────────────────────────────┴────────────────────┴────────────┘\n";
}

} // namespace scopefs
