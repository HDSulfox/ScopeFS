#pragma once

#include <string>
#include <vector>

#include "scopefs/trace.hpp"

namespace scopefs {

class BlockDevice {
 public:
  explicit BlockDevice(TraceSink& trace);
  void ensureWorkspace() const;
  bool volumeExists() const;
  std::string readAll();
  void writeAll(const std::string& bytes);
  void appendJournal(const std::string& line);
  std::vector<std::string> readJournal() const;
  void clearJournal();
  const std::string& volumePath() const;
  const std::string& journalPath() const;

 private:
  TraceSink& trace_;
  std::string volumePath_;
  std::string journalPath_;
};

} // namespace scopefs
