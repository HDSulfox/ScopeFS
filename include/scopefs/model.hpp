#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace scopefs {

enum class NodeType {
  Regular,
  Directory,
  SnapshotRoot,
  ClassObject
};

enum class MountState {
  Clean,
  Dirty,
  Recovering
};

struct SuperBlock {
  std::uint32_t version = 1;
  std::uint32_t blockSize = 4096;
  std::uint32_t totalBlocks = 16384;
  std::uint32_t inodeCount = 2048;
  std::uint32_t freeBlocks = 0;
  std::uint32_t freeInodes = 0;
  std::uint32_t activeRoot = 1;
  std::uint32_t nextInode = 1;
  std::uint32_t nextBlock = 0;
  std::uint64_t nextTxid = 1;
  MountState mountState = MountState::Clean;
  std::uint32_t checksum = 0;
};

struct DirEntry {
  std::string name;
  NodeType type = NodeType::Regular;
  std::uint32_t inode = 0;
  std::uint32_t generation = 0;
};

struct AclEntry {
  std::string subject;
  std::string rights;
  std::string constraints;
  std::uint32_t generation = 1;
};

struct Inode {
  std::uint32_t id = 0;
  std::uint32_t generation = 1;
  NodeType type = NodeType::Regular;
  std::string owner = "root";
  std::string klass = "system";
  int mode = 0644;
  std::uint64_t size = 0;
  std::uint32_t nlink = 1;
  std::uint32_t openCount = 0;
  std::uint32_t refcount = 1;
  bool deletePending = false;
  std::vector<std::uint32_t> blocks;
  std::map<std::string, DirEntry> entries;
  std::vector<AclEntry> acl;
};

struct BlockInfo {
  std::uint32_t id = 0;
  std::uint32_t refcount = 0;
  std::uint32_t checksum = 0;
  std::uint64_t lastWriterTxid = 0;
  std::string flags = "data";
  std::uint32_t ownerInode = 0;
  std::string data;
};

struct UserRecord {
  std::string name;
  std::string passwordHash;
  std::string home;
  std::set<std::string> classes;
};

struct ClassDef {
  std::string name;
  std::string owner = "root";
  std::set<std::string> parents;
  std::set<std::string> members;
  bool grantOption = false;
  std::string constraints;
  std::uint32_t generation = 1;
};

struct Snapshot {
  std::string name;
  std::uint32_t rootInode = 0;
  std::uint32_t generation = 1;
  std::uint64_t txid = 0;
  std::string createdAt;
};

struct OpenFile {
  int fd = 0;
  std::string user;
  std::uint32_t inode = 0;
  std::string path;
  std::string flags;
  std::uint64_t offset = 0;
};

struct UserSession {
  std::string user;
  std::string cwd = "/";
  std::map<int, OpenFile> openFiles;
  int nextFd = 3;
};

struct FsState {
  SuperBlock super;
  std::map<std::uint32_t, Inode> inodes;
  std::map<std::uint32_t, BlockInfo> blocks;
  std::map<std::string, UserRecord> users;
  std::map<std::string, ClassDef> classes;
  std::map<std::string, Snapshot> snapshots;
  std::set<std::uint32_t> freeBlocks;
  std::set<std::uint32_t> freeInodes;
};

std::string toString(NodeType type);
NodeType nodeTypeFromString(const std::string& value);
std::string toString(MountState state);
MountState mountStateFromString(const std::string& value);

} // namespace scopefs
