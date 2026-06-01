#pragma once

#include <cstdint>
#include <deque>
#include <ostream>
#include <string>
#include <vector>

#include "scopefs/config.hpp"

namespace scopefs {

struct TraceEvent {
  std::uint64_t seq = 0;
  std::uint64_t txid = 0;
  std::string ts;
  std::string session;
  std::string user;
  std::string command;
  std::string type;
  std::string object;
  std::string before;
  std::string after;
  std::string reason;
  std::string status;
};

class TraceSink {
 public:
  void setEnabled(bool enabled);
  bool enabled() const;
  void setContext(std::string user, std::string command);
  void emit(std::uint64_t txid,
            const std::string& type,
            const std::string& object,
            const std::string& before,
            const std::string& after,
            const std::string& reason,
            const std::string& status);
  std::vector<TraceEvent> recent(std::size_t n = 0) const;
  void clear();
  bool save(const std::string& path, std::string* err) const;
  static std::vector<TraceEvent> load(const std::string& path);
  static std::string toJsonLine(const TraceEvent& event);
  static TraceEvent fromJsonLine(const std::string& line);
  std::uint64_t nextSeq() const;

 private:
  bool enabled_ = true;
  std::uint64_t seq_ = 1;
  std::string currentUser_ = "-";
  std::string currentCommand_ = "-";
  std::deque<TraceEvent> ring_;
};

void renderTraceEvents(std::ostream& out, const std::vector<TraceEvent>& events);

} // namespace scopefs
