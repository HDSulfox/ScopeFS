#include "scopefs/block_device.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "scopefs/config.hpp"

namespace scopefs {

BlockDevice::BlockDevice(TraceSink& trace)
    : trace_(trace), volumePath_(config::volumePath()), journalPath_(config::journalPath()) {}

void BlockDevice::ensureWorkspace() const {
  std::filesystem::create_directories(config::workspaceDir());
}

bool BlockDevice::volumeExists() const {
  return std::filesystem::exists(volumePath_);
}

std::string BlockDevice::readAll() {
  ensureWorkspace();
  std::ifstream in(volumePath_, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  trace_.emit(0, "block.read", "volume", "-", std::to_string(ss.str().size()), "load serialized volume", "ok");
  return ss.str();
}

void BlockDevice::writeAll(const std::string& bytes) {
  ensureWorkspace();
  std::ofstream out(volumePath_, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  trace_.emit(0, "block.write", "volume", "-", std::to_string(bytes.size()), "checkpoint serialized volume", "ok");
}

void BlockDevice::appendJournal(const std::string& line) {
  ensureWorkspace();
  std::ofstream out(journalPath_, std::ios::binary | std::ios::app);
  out << line << '\n';
}

std::vector<std::string> BlockDevice::readJournal() const {
  std::vector<std::string> out;
  std::ifstream in(journalPath_, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty()) out.push_back(line);
  }
  return out;
}

void BlockDevice::clearJournal() {
  ensureWorkspace();
  std::ofstream out(journalPath_, std::ios::binary | std::ios::trunc);
  (void)out;
  trace_.emit(0, "journal.clear", "journal", "-", "-", "checkpoint complete", "ok");
}

const std::string& BlockDevice::volumePath() const { return volumePath_; }
const std::string& BlockDevice::journalPath() const { return journalPath_; }

} // namespace scopefs
