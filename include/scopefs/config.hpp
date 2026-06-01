#pragma once

#include <cstdint>
#include <string>

namespace scopefs::config {

constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kBlockSize = 4096;
constexpr std::uint32_t kTotalBlocks = 16384;
constexpr std::uint32_t kInodeCount = 2048;
constexpr std::uint32_t kJournalBlocks = 1024;
constexpr std::uint32_t kDirectPtrs = 12;
constexpr std::uint32_t kMaxOpenPerUser = 64;
constexpr std::size_t kTraceRing = 10000;

constexpr std::uint32_t kReservedBlock = 0;
constexpr std::uint32_t kSuperBlock = 1;
constexpr std::uint32_t kBlockMetaStart = 2;
constexpr std::uint32_t kBlockMetaBlocks = 128;
constexpr std::uint32_t kInodeStart = kBlockMetaStart + kBlockMetaBlocks;
constexpr std::uint32_t kInodeBlocks = 512;
constexpr std::uint32_t kJournalStart = kInodeStart + kInodeBlocks;
constexpr std::uint32_t kSnapshotStart = kJournalStart + kJournalBlocks;
constexpr std::uint32_t kSnapshotBlocks = 64;
constexpr std::uint32_t kUserTableStart = kSnapshotStart + kSnapshotBlocks;
constexpr std::uint32_t kUserTableBlocks = 64;
constexpr std::uint32_t kDataStart = kUserTableStart + kUserTableBlocks;

inline std::string workspaceDir() { return ".scopefs"; }
inline std::string volumePath() { return ".scopefs/scopefs.volume"; }
inline std::string journalPath() { return ".scopefs/scopefs.journal"; }
inline std::string tracePath() { return ".scopefs/scopefs.trace.jsonl"; }

} // namespace scopefs::config
