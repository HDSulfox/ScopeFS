#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "scopefs/trace.hpp"

namespace scopefs {

struct LockRecord {
  std::string kind;
  std::string mode;
  std::string sessionId;
  std::uint64_t pid = 0;
  std::string user;
  std::uint32_t inode = 0;
  int fd = -1;
  std::string path;
  std::string reason;
  bool stale = false;
};

class VolumeCoordinator {
 public:
  class ScopedLock {
   public:
    ScopedLock() = default;
    ScopedLock(VolumeCoordinator* owner, std::filesystem::path path, std::string kind);
    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;
    ScopedLock(ScopedLock&& other) noexcept;
    ScopedLock& operator=(ScopedLock&& other) noexcept;
    ~ScopedLock();
    bool active() const;
    void release();

   private:
    VolumeCoordinator* owner_ = nullptr;
    std::filesystem::path path_;
    std::string kind_;
    bool active_ = false;
  };

  explicit VolumeCoordinator(TraceSink& trace);

  void startSession();
  void heartbeat(const std::string& user, const std::string& command);
  void shutdownClean();
  void markAbnormalExit();
  bool hasOtherActiveSessions() const;

  const std::string& sessionId() const;
  std::uint64_t pid() const;

  ScopedLock acquireTxLock(const std::string& reason);
  ScopedLock acquireReadLock(const std::string& reason);
  bool hasTxLock() const;

  bool acquireInodeLock(std::uint32_t inode,
                        const std::string& path,
                        const std::string& mode,
                        const std::string& user,
                        int fd,
                        const std::string& reason,
                        std::string* error);
  void releaseInodeLock(std::uint32_t inode, int fd);
  void releaseAllInodeLocks();
  std::size_t inodeHolderCount(std::uint32_t inode, bool includeSelf) const;
  bool hasAnyWriteHolder(bool includeSelf) const;

  std::vector<LockRecord> listLocks(bool includeStale) const;
  std::size_t clearStaleLocks();

  std::uint64_t currentEpoch() const;
  bool observeEpochChange(std::uint64_t* before, std::uint64_t* after);
  void bumpEpoch();

  void broadcastCrash(const std::string& reason);
  bool hasPendingCrashSignal(std::string* reason);

 private:
  TraceSink& trace_;
  std::string sessionId_;
  std::uint64_t pid_ = 0;
  std::uint64_t observedEpoch_ = 0;
  std::uint64_t observedSignalEpoch_ = 0;
  bool abnormalExit_ = false;
  int txDepth_ = 0;
  int readLockSerial_ = 0;

  std::filesystem::path workspace() const;
  std::filesystem::path sessionsDir() const;
  std::filesystem::path inodeLocksDir() const;
  std::filesystem::path readLocksDir() const;
  std::filesystem::path mutexDir() const;
  std::filesystem::path txDir() const;
  std::filesystem::path epochPath() const;
  std::filesystem::path signalPath() const;
  std::filesystem::path sessionPath() const;

  void ensureLayout() const;
  ScopedLock acquireMutex() const;
  bool isPidAlive(std::uint64_t pid) const;
  bool isRecordStale(const LockRecord& record) const;
  LockRecord readRecord(const std::filesystem::path& path) const;
  void writeRecord(const std::filesystem::path& path, const LockRecord& record) const;
  std::vector<std::filesystem::path> inodeLockFiles() const;
  std::vector<std::filesystem::path> readLockFiles() const;
  void cleanupStaleLocksUnlocked(std::size_t* removed) const;
  void releasePath(const std::filesystem::path& path, const std::string& kind);
  int lockTimeoutMs() const;
};

} // namespace scopefs
