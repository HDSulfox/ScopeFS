#include "scopefs/coordinator.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#include "scopefs/config.hpp"
#include "scopefs/util.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace scopefs {

namespace {

std::uint64_t currentPid() {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(getpid());
#endif
}

std::string makeSessionId(std::uint64_t pid) {
  const auto seed = nowIso() + ":" + std::to_string(pid) + ":" + std::to_string(reinterpret_cast<std::uintptr_t>(&pid));
  return std::to_string(pid) + "-" + std::to_string(checksum32(seed));
}

std::string field(const std::vector<std::string>& f, std::size_t index) {
  return index < f.size() ? decode(f[index]) : "";
}

std::uint64_t numberField(const std::vector<std::string>& f, std::size_t index) {
  if (index >= f.size() || f[index].empty()) return 0;
  return std::strtoull(f[index].c_str(), nullptr, 10);
}

bool removeAllRetry(const std::filesystem::path& path) {
  for (int i = 0; i < 8; ++i) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    if (!ec || !std::filesystem::exists(path)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return !std::filesystem::exists(path);
}

bool tryCreateDir(const std::filesystem::path& path) {
  std::error_code ec;
  const bool created = std::filesystem::create_directory(path, ec);
  if (created) return true;
  return false;
}

} // namespace

VolumeCoordinator::ScopedLock::ScopedLock(VolumeCoordinator* owner, std::filesystem::path path, std::string kind)
    : owner_(owner), path_(std::move(path)), kind_(std::move(kind)), active_(owner_ != nullptr) {}

VolumeCoordinator::ScopedLock::ScopedLock(ScopedLock&& other) noexcept
    : owner_(other.owner_), path_(std::move(other.path_)), kind_(std::move(other.kind_)), active_(other.active_) {
  other.owner_ = nullptr;
  other.active_ = false;
}

VolumeCoordinator::ScopedLock& VolumeCoordinator::ScopedLock::operator=(ScopedLock&& other) noexcept {
  if (this == &other) return *this;
  release();
  owner_ = other.owner_;
  path_ = std::move(other.path_);
  kind_ = std::move(other.kind_);
  active_ = other.active_;
  other.owner_ = nullptr;
  other.active_ = false;
  return *this;
}

VolumeCoordinator::ScopedLock::~ScopedLock() { release(); }

bool VolumeCoordinator::ScopedLock::active() const { return active_; }

void VolumeCoordinator::ScopedLock::release() {
  if (!active_ || !owner_) return;
  owner_->releasePath(path_, kind_);
  active_ = false;
}

VolumeCoordinator::VolumeCoordinator(TraceSink& trace) : trace_(trace), pid_(currentPid()) {}

void VolumeCoordinator::startSession() {
  ensureLayout();
  sessionId_ = makeSessionId(pid_);
  observedEpoch_ = currentEpoch();
  std::string ignored;
  hasPendingCrashSignal(&ignored);
  observedSignalEpoch_ = numberField(split(trim([&] {
    std::ifstream in(signalPath(), std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }()), '|'), 1);
  heartbeat("-", "boot");
  trace_.emit(0, "coord.session.start", sessionId_, "-", std::to_string(pid_), "coordinator session started", "ok");
}

void VolumeCoordinator::heartbeat(const std::string& user, const std::string& command) {
  if (sessionId_.empty()) startSession();
  ensureLayout();
  LockRecord record;
  record.kind = "session";
  record.sessionId = sessionId_;
  record.pid = pid_;
  record.user = user;
  record.reason = command;
  writeRecord(sessionPath(), record);
}

void VolumeCoordinator::shutdownClean() {
  releaseAllInodeLocks();
  if (!sessionId_.empty()) removeAllRetry(sessionPath());
  trace_.emit(0, "coord.session.stop", sessionId_, "-", std::to_string(pid_), "clean session shutdown", "ok");
}

void VolumeCoordinator::markAbnormalExit() { abnormalExit_ = true; }

bool VolumeCoordinator::hasOtherActiveSessions() const {
  if (!std::filesystem::exists(sessionsDir())) return false;
  for (const auto& entry : std::filesystem::directory_iterator(sessionsDir())) {
    if (!entry.is_regular_file()) continue;
    const auto record = readRecord(entry.path());
    if (record.kind == "session" && record.sessionId != sessionId_ && !isRecordStale(record)) return true;
  }
  return false;
}

const std::string& VolumeCoordinator::sessionId() const { return sessionId_; }

std::uint64_t VolumeCoordinator::pid() const { return pid_; }

VolumeCoordinator::ScopedLock VolumeCoordinator::acquireTxLock(const std::string& reason) {
  if (sessionId_.empty()) const_cast<VolumeCoordinator*>(this)->startSession();
  const auto timeoutMs = lockTimeoutMs();
  const auto start = std::chrono::steady_clock::now();
  while (true) {
    {
      auto mutex = acquireMutex();
      std::size_t removed = 0;
      cleanupStaleLocksUnlocked(&removed);
      bool readersActive = false;
      for (const auto& readFile : readLockFiles()) {
        const auto record = readRecord(readFile);
        if (record.kind == "read" && record.sessionId != sessionId_ && !isRecordStale(record)) {
          readersActive = true;
          break;
        }
      }
      if (!readersActive && tryCreateDir(txDir())) {
        LockRecord record;
        record.kind = "tx";
        record.mode = "exclusive";
        record.sessionId = sessionId_;
        record.pid = pid_;
        record.reason = reason;
        writeRecord(txDir() / "holder", record);
        ++txDepth_;
        trace_.emit(0, "coord.lock.acquire", "tx", "-", sessionId_, reason, "ok");
        return ScopedLock(this, txDir(), "tx");
      }
    }
    trace_.emit(0, "coord.lock.wait", "tx", "-", sessionId_, reason, "wait");
    if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() >= timeoutMs) {
      trace_.emit(0, "coord.lock.deny", "tx", "-", sessionId_, reason, "busy");
      throw std::runtime_error("E_LOCK_BUSY: transaction lock is busy");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

VolumeCoordinator::ScopedLock VolumeCoordinator::acquireReadLock(const std::string& reason) {
  if (sessionId_.empty()) startSession();
  const auto timeoutMs = lockTimeoutMs();
  const auto start = std::chrono::steady_clock::now();
  while (true) {
    {
      auto mutex = acquireMutex();
      std::size_t removed = 0;
      cleanupStaleLocksUnlocked(&removed);
      bool writerActive = false;
      if (std::filesystem::exists(txDir() / "holder")) {
        const auto record = readRecord(txDir() / "holder");
        writerActive = !isRecordStale(record);
      }
      if (!writerActive) {
        LockRecord record;
        record.kind = "read";
        record.mode = "shared";
        record.sessionId = sessionId_;
        record.pid = pid_;
        record.user = "-";
        record.reason = reason;
        const auto file = readLocksDir() / ("read_" + sessionId_ + "_" + std::to_string(++readLockSerial_) + ".lock");
        writeRecord(file, record);
        trace_.emit(0, "coord.lock.acquire", "read", "-", sessionId_, reason, "ok");
        return ScopedLock(this, file, "read");
      }
    }
    trace_.emit(0, "coord.lock.wait", "read", "-", sessionId_, reason, "wait");
    if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() >= timeoutMs) {
      trace_.emit(0, "coord.lock.deny", "read", "-", sessionId_, reason, "busy");
      throw std::runtime_error("E_LOCK_BUSY: volume writer is busy");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

bool VolumeCoordinator::hasTxLock() const { return txDepth_ > 0; }

bool VolumeCoordinator::acquireInodeLock(std::uint32_t inode,
                                         const std::string& path,
                                         const std::string& mode,
                                         const std::string& user,
                                         int fd,
                                         const std::string& reason,
                                         std::string* error) {
  auto mutex = acquireMutex();
  std::size_t removed = 0;
  cleanupStaleLocksUnlocked(&removed);
  const bool wantWrite = mode == "write";
  for (const auto& lockFile : inodeLockFiles()) {
    const auto record = readRecord(lockFile);
    if (record.kind != "inode" || record.inode != inode || record.sessionId == sessionId_) continue;
    const bool existingWrite = record.mode == "write";
    if (wantWrite || existingWrite) {
      if (error) *error = "inode " + std::to_string(inode) + " locked by session " + record.sessionId + " mode=" + record.mode;
      trace_.emit(0, "inode.lock.conflict", std::to_string(inode), mode, record.sessionId, reason, "busy");
      return false;
    }
  }
  LockRecord record;
  record.kind = "inode";
  record.mode = mode;
  record.sessionId = sessionId_;
  record.pid = pid_;
  record.user = user;
  record.inode = inode;
  record.fd = fd;
  record.path = path;
  record.reason = reason;
  const auto file = inodeLocksDir() / ("inode_" + std::to_string(inode) + "_" + sessionId_ + "_" + std::to_string(fd) + ".lock");
  writeRecord(file, record);
  trace_.emit(0, "inode.lock.acquire", std::to_string(inode), "-", mode, path, "ok");
  return true;
}

void VolumeCoordinator::releaseInodeLock(std::uint32_t inode, int fd) {
  const auto prefix = "inode_" + std::to_string(inode) + "_" + sessionId_ + "_" + std::to_string(fd);
  for (const auto& lockFile : inodeLockFiles()) {
    if (lockFile.filename().string().rfind(prefix, 0) == 0) {
      removeAllRetry(lockFile);
      trace_.emit(0, "inode.lock.release", std::to_string(inode), std::to_string(fd), sessionId_, "fd close", "ok");
    }
  }
}

void VolumeCoordinator::releaseAllInodeLocks() {
  for (const auto& lockFile : inodeLockFiles()) {
    const auto record = readRecord(lockFile);
    if (record.kind == "inode" && record.sessionId == sessionId_) {
      removeAllRetry(lockFile);
      trace_.emit(0, "inode.lock.release", std::to_string(record.inode), std::to_string(record.fd), sessionId_, "session cleanup", "ok");
    }
  }
}

std::size_t VolumeCoordinator::inodeHolderCount(std::uint32_t inode, bool includeSelf) const {
  std::size_t count = 0;
  for (const auto& lockFile : inodeLockFiles()) {
    const auto record = readRecord(lockFile);
    if (record.kind != "inode" || record.inode != inode) continue;
    if (!includeSelf && record.sessionId == sessionId_) continue;
    if (!isRecordStale(record)) ++count;
  }
  return count;
}

bool VolumeCoordinator::hasAnyWriteHolder(bool includeSelf) const {
  for (const auto& lockFile : inodeLockFiles()) {
    const auto record = readRecord(lockFile);
    if (record.kind != "inode" || record.mode != "write") continue;
    if (!includeSelf && record.sessionId == sessionId_) continue;
    if (!isRecordStale(record)) return true;
  }
  return false;
}

std::vector<LockRecord> VolumeCoordinator::listLocks(bool includeStale) const {
  std::vector<LockRecord> out;
  if (std::filesystem::exists(txDir() / "holder")) {
    auto tx = readRecord(txDir() / "holder");
    tx.stale = isRecordStale(tx);
    if (includeStale || !tx.stale) out.push_back(tx);
  }
  for (const auto& readFile : readLockFiles()) {
    auto record = readRecord(readFile);
    record.stale = isRecordStale(record);
    if (includeStale || !record.stale) out.push_back(record);
  }
  for (const auto& lockFile : inodeLockFiles()) {
    auto record = readRecord(lockFile);
    record.stale = isRecordStale(record);
    if (includeStale || !record.stale) out.push_back(record);
  }
  return out;
}

std::size_t VolumeCoordinator::clearStaleLocks() {
  auto mutex = acquireMutex();
  std::size_t removed = 0;
  cleanupStaleLocksUnlocked(&removed);
  trace_.emit(0, "coord.lock.clear_stale", "locks", "-", std::to_string(removed), "manual stale cleanup", "ok");
  return removed;
}

std::uint64_t VolumeCoordinator::currentEpoch() const {
  std::ifstream in(epochPath(), std::ios::binary);
  std::uint64_t epoch = 0;
  in >> epoch;
  return epoch;
}

bool VolumeCoordinator::observeEpochChange(std::uint64_t* before, std::uint64_t* after) {
  const auto current = currentEpoch();
  if (before) *before = observedEpoch_;
  if (after) *after = current;
  if (current == observedEpoch_) return false;
  observedEpoch_ = current;
  return true;
}

void VolumeCoordinator::bumpEpoch() {
  ensureLayout();
  const auto next = currentEpoch() + 1;
  std::ofstream out(epochPath(), std::ios::binary | std::ios::trunc);
  out << next << "\n";
  observedEpoch_ = next;
  trace_.emit(0, "coord.epoch.bump", "epoch", "-", std::to_string(next), "volume changed", "ok");
}

void VolumeCoordinator::broadcastCrash(const std::string& reason) {
  ensureLayout();
  const auto epoch = [&] {
    std::ifstream in(signalPath(), std::ios::binary);
    std::string line;
    std::getline(in, line);
    const auto f = split(line, '|');
    return numberField(f, 1) + 1;
  }();
  std::ofstream out(signalPath(), std::ios::binary | std::ios::trunc);
  out << "SIGNAL|" << epoch << "|" << encode(sessionId_) << "|" << pid_ << "|" << encode(reason) << "|" << encode(nowIso()) << "\n";
  observedSignalEpoch_ = epoch;
  trace_.emit(0, "coord.signal.crash", "signal", "-", std::to_string(epoch), reason, "crash");
}

bool VolumeCoordinator::hasPendingCrashSignal(std::string* reason) {
  std::ifstream in(signalPath(), std::ios::binary);
  std::string line;
  std::getline(in, line);
  const auto f = split(line, '|');
  if (f.size() < 6 || f[0] != "SIGNAL") return false;
  const auto epoch = numberField(f, 1);
  const auto sender = field(f, 2);
  const auto why = field(f, 4);
  if (epoch <= observedSignalEpoch_ || sender == sessionId_) return false;
  observedSignalEpoch_ = epoch;
  if (reason) *reason = why.empty() ? "broadcast crash signal" : why;
  trace_.emit(0, "coord.signal.receive", "signal", sender, std::to_string(epoch), why, "crash");
  return true;
}

std::filesystem::path VolumeCoordinator::workspace() const { return config::workspaceDir(); }
std::filesystem::path VolumeCoordinator::sessionsDir() const { return workspace() / "sessions"; }
std::filesystem::path VolumeCoordinator::inodeLocksDir() const { return workspace() / "inode_locks"; }
std::filesystem::path VolumeCoordinator::readLocksDir() const { return workspace() / "read_locks"; }
std::filesystem::path VolumeCoordinator::mutexDir() const { return workspace() / "scopefs.lock"; }
std::filesystem::path VolumeCoordinator::txDir() const { return workspace() / "tx.lock"; }
std::filesystem::path VolumeCoordinator::epochPath() const { return workspace() / "scopefs.epoch"; }
std::filesystem::path VolumeCoordinator::signalPath() const { return workspace() / "scopefs.signal"; }
std::filesystem::path VolumeCoordinator::sessionPath() const { return sessionsDir() / (sessionId_ + ".session"); }

void VolumeCoordinator::ensureLayout() const {
  std::filesystem::create_directories(workspace());
  std::filesystem::create_directories(sessionsDir());
  std::filesystem::create_directories(inodeLocksDir());
  std::filesystem::create_directories(readLocksDir());
  if (!std::filesystem::exists(epochPath())) {
    std::ofstream out(epochPath(), std::ios::binary | std::ios::trunc);
    out << "0\n";
  }
}

VolumeCoordinator::ScopedLock VolumeCoordinator::acquireMutex() const {
  const auto timeoutMs = 5000;
  const auto start = std::chrono::steady_clock::now();
  while (true) {
    if (tryCreateDir(mutexDir())) {
      return ScopedLock(const_cast<VolumeCoordinator*>(this), mutexDir(), "mutex");
    }
    if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() >= timeoutMs) {
      std::filesystem::remove_all(mutexDir());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

bool VolumeCoordinator::isPidAlive(std::uint64_t pid) const {
  if (pid == 0) return false;
#if defined(_WIN32)
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
  if (!process) return false;
  const DWORD result = WaitForSingleObject(process, 0);
  CloseHandle(process);
  return result == WAIT_TIMEOUT;
#else
  return kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

bool VolumeCoordinator::isRecordStale(const LockRecord& record) const {
  if (record.sessionId == sessionId_) return false;
  if (!isPidAlive(record.pid)) return true;
  const auto sessionFile = sessionsDir() / (record.sessionId + ".session");
  if (!std::filesystem::exists(sessionFile)) return true;
  return false;
}

LockRecord VolumeCoordinator::readRecord(const std::filesystem::path& path) const {
  std::ifstream in(path, std::ios::binary);
  std::string line;
  std::getline(in, line);
  const auto f = split(line, '|');
  LockRecord record;
  if (f.empty()) return record;
  record.kind = lower(f[0]);
  if (record.kind == "session") {
    record.sessionId = field(f, 1);
    record.pid = numberField(f, 2);
    record.user = field(f, 3);
    record.reason = field(f, 4);
  } else if (record.kind == "tx") {
    record.sessionId = field(f, 1);
    record.pid = numberField(f, 2);
    record.reason = field(f, 3);
    record.mode = "exclusive";
  } else if (record.kind == "read") {
    record.sessionId = field(f, 1);
    record.pid = numberField(f, 2);
    record.user = field(f, 3);
    record.reason = field(f, 4);
    record.mode = "shared";
  } else if (record.kind == "inode") {
    record.sessionId = field(f, 1);
    record.pid = numberField(f, 2);
    record.user = field(f, 3);
    record.inode = static_cast<std::uint32_t>(numberField(f, 4));
    record.fd = static_cast<int>(numberField(f, 5));
    record.mode = field(f, 6);
    record.path = field(f, 7);
    record.reason = field(f, 8);
  }
  return record;
}

void VolumeCoordinator::writeRecord(const std::filesystem::path& path, const LockRecord& record) const {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (record.kind == "session") {
    out << "SESSION|" << encode(record.sessionId) << "|" << record.pid << "|" << encode(record.user) << "|" << encode(record.reason) << "\n";
  } else if (record.kind == "tx") {
    out << "TX|" << encode(record.sessionId) << "|" << record.pid << "|" << encode(record.reason) << "\n";
  } else if (record.kind == "read") {
    out << "READ|" << encode(record.sessionId) << "|" << record.pid << "|" << encode(record.user) << "|"
        << encode(record.reason) << "\n";
  } else if (record.kind == "inode") {
    out << "INODE|" << encode(record.sessionId) << "|" << record.pid << "|" << encode(record.user) << "|"
        << record.inode << "|" << record.fd << "|" << encode(record.mode) << "|" << encode(record.path) << "|"
        << encode(record.reason) << "\n";
  }
}

std::vector<std::filesystem::path> VolumeCoordinator::inodeLockFiles() const {
  std::vector<std::filesystem::path> out;
  if (!std::filesystem::exists(inodeLocksDir())) return out;
  for (const auto& entry : std::filesystem::directory_iterator(inodeLocksDir())) {
    if (entry.is_regular_file()) out.push_back(entry.path());
  }
  return out;
}

std::vector<std::filesystem::path> VolumeCoordinator::readLockFiles() const {
  std::vector<std::filesystem::path> out;
  if (!std::filesystem::exists(readLocksDir())) return out;
  for (const auto& entry : std::filesystem::directory_iterator(readLocksDir())) {
    if (entry.is_regular_file()) out.push_back(entry.path());
  }
  return out;
}

void VolumeCoordinator::cleanupStaleLocksUnlocked(std::size_t* removed) const {
  auto drop = [&](const std::filesystem::path& path) {
    if (removeAllRetry(path) && removed) ++*removed;
  };
  if (std::filesystem::exists(txDir() / "holder")) {
    const auto record = readRecord(txDir() / "holder");
    if (isRecordStale(record)) drop(txDir());
  }
  for (const auto& readFile : readLockFiles()) {
    const auto record = readRecord(readFile);
    if (isRecordStale(record)) drop(readFile);
  }
  for (const auto& lockFile : inodeLockFiles()) {
    const auto record = readRecord(lockFile);
    if (isRecordStale(record)) drop(lockFile);
  }
  if (std::filesystem::exists(sessionsDir())) {
    for (const auto& entry : std::filesystem::directory_iterator(sessionsDir())) {
      if (!entry.is_regular_file()) continue;
      const auto record = readRecord(entry.path());
      if (record.kind == "session" && isRecordStale(record)) drop(entry.path());
    }
  }
}

void VolumeCoordinator::releasePath(const std::filesystem::path& path, const std::string& kind) {
  if (abnormalExit_) return;
  removeAllRetry(path);
  if (kind == "tx" && txDepth_ > 0) --txDepth_;
  trace_.emit(0, "coord.lock.release", kind, sessionId_, path.string(), "lock released", "ok");
}

int VolumeCoordinator::lockTimeoutMs() const {
  if (const char* env = std::getenv("SCOPEFS_LOCK_TIMEOUT_MS")) {
    const int value = std::atoi(env);
    if (value > 0) return value;
  }
  return 5000;
}

} // namespace scopefs
