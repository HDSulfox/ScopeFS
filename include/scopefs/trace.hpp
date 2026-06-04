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
  std::uint64_t parentSeq = 0;
  int depth = 0;
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
  class Span {
   public:
    Span() = default;
    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;
    Span(Span&& other) noexcept;
    Span& operator=(Span&& other) noexcept;
    ~Span();

    bool active() const;
    std::uint64_t seq() const;
    void setTxid(std::uint64_t txid);
    void update(const std::string& object,
                const std::string& before,
                const std::string& after,
                const std::string& reason,
                const std::string& status);
    void finish(const std::string& object,
                const std::string& before,
                const std::string& after,
                const std::string& reason,
                const std::string& status);

   private:
    friend class TraceSink;
    Span(TraceSink* sink, std::uint64_t seq);

    TraceSink* sink_ = nullptr;
    std::uint64_t seq_ = 0;
    bool active_ = false;
  };

  void setEnabled(bool enabled);
  bool enabled() const;
  void setContext(std::string user, std::string command);
  Span span(std::uint64_t txid,
            const std::string& type,
            const std::string& object,
            const std::string& before,
            const std::string& after,
            const std::string& reason,
            const std::string& status);
  void emit(std::uint64_t txid,
            const std::string& type,
            const std::string& object,
            const std::string& before,
            const std::string& after,
            const std::string& reason,
            const std::string& status);
  std::vector<TraceEvent> recent(std::size_t n = 0) const;
  std::vector<TraceEvent> since(std::uint64_t firstSeq) const;
  std::vector<TraceEvent> sinceForCommand(std::uint64_t firstSeq, const std::string& command) const;
  void clear();
  bool save(const std::string& path, std::string* err) const;
  static std::vector<TraceEvent> load(const std::string& path);
  static std::string toJsonLine(const TraceEvent& event);
  static TraceEvent fromJsonLine(const std::string& line);
  std::uint64_t nextSeq() const;

 private:
  std::uint64_t beginSpan(std::uint64_t txid,
                          const std::string& type,
                          const std::string& object,
                          const std::string& before,
                          const std::string& after,
                          const std::string& reason,
                          const std::string& status);
  void finishSpan(std::uint64_t seq,
                  const std::string& object,
                  const std::string& before,
                  const std::string& after,
                  const std::string& reason,
                  const std::string& status);
  void setSpanTxid(std::uint64_t seq, std::uint64_t txid);
  void updateSpan(std::uint64_t seq,
                  const std::string& object,
                  const std::string& before,
                  const std::string& after,
                  const std::string& reason,
                  const std::string& status);
  bool filtered(const std::string& type) const;

  bool enabled_ = true;
  std::uint64_t seq_ = 1;
  std::string currentUser_ = "-";
  std::string currentCommand_ = "-";
  std::deque<TraceEvent> ring_;
  std::vector<std::uint64_t> stack_;
};

void renderTraceEvents(std::ostream& out, const std::vector<TraceEvent>& events);

} // namespace scopefs
