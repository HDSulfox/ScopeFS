#include "scopefs/kernel.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <queue>
#include <sstream>

#include "scopefs/config.hpp"
#include "scopefs/util.hpp"

namespace scopefs {

namespace {

std::string yn(bool value) { return value ? "1" : "0"; }

std::string boolText(bool value) { return value ? "yes" : "no"; }

bool parseBool(const std::string& value) {
  return value == "1" || lower(value) == "true" || lower(value) == "yes";
}

std::string nodeMarker(NodeType type) {
  switch (type) {
    case NodeType::Regular: return "file";
    case NodeType::Directory: return "dir";
    case NodeType::SnapshotRoot: return "snap";
    case NodeType::ClassObject: return "class";
  }
  return "unknown";
}

std::string hashPassword(const std::string& user, const std::string& password) {
  return std::to_string(checksum32(user + ":" + password + ":ScopeFS"));
}

std::string clip(const std::string& value, std::size_t width) {
  if (value.size() <= width) return value;
  if (width < 2) return value.substr(0, width);
  return value.substr(0, width - 1) + "…";
}

std::string rightForFlag(const std::string& flags) {
  if (flags.find('w') != std::string::npos || flags.find("append") != std::string::npos ||
      flags.find("truncate") != std::string::npos) {
    return "w";
  }
  return "r";
}

std::string blockFlagsForId(std::uint32_t block) {
  if (block == config::kSuperBlock) return "super";
  if (block >= config::kBlockMetaStart && block < config::kBlockMetaStart + config::kBlockMetaBlocks) return "refcount";
  if (block >= config::kInodeStart && block < config::kInodeStart + config::kInodeBlocks) return "inode";
  if (block >= config::kJournalStart && block < config::kJournalStart + config::kJournalBlocks) return "journal";
  if (block >= config::kSnapshotStart && block < config::kSnapshotStart + config::kSnapshotBlocks) return "snapshot";
  if (block >= config::kUserTableStart && block < config::kUserTableStart + config::kUserTableBlocks) return "user";
  return "data";
}

std::string hexDigit(std::uint64_t value) {
  constexpr char digits[] = "0123456789abcdef";
  return std::string(1, digits[value % 16]);
}

std::string plainLayoutGlyph(std::uint32_t block, const BlockInfo* info) {
  const auto f = blockFlagsForId(block);
  if (f == "super") return "S";
  if (f == "refcount") return "R";
  if (f == "inode") return "I";
  if (f == "journal") return "J";
  if (f == "snapshot") return "P";
  if (f == "user") return "U";
  if (info && info->refcount > 0) return "D";
  return ".";
}

std::string plainMapGlyph(const std::string& what, std::uint32_t block, const BlockInfo* info) {
  if (what == "blocks") return plainLayoutGlyph(block, info);
  if (what == "inode") {
    const auto f = blockFlagsForId(block);
    if (f == "inode") return "I";
    if (info && info->ownerInode != 0) return hexDigit(info->ownerInode);
    if (info && info->refcount > 1) return "*";
    return ".";
  }
  if (what == "journal") {
    const auto f = blockFlagsForId(block);
    if (f == "journal") return "J";
    if (info && info->lastWriterTxid != 0) return hexDigit(info->lastWriterTxid);
    if (f == "super") return "S";
    return ".";
  }
  if (what == "owner") {
    if (info && info->ownerInode != 0) return hexDigit(info->ownerInode);
    return ".";
  }
  if (!info || info->refcount == 0) return ".";
  if (info->refcount == 1) return "1";
  if (info->refcount == 2) return "2";
  return "#";
}

std::string plainMapLegend(const std::string& what) {
  if (what == "blocks") return "legend: S super R refmeta I inode J journal P snapshot U users D data . free";
  if (what == "inode") return "legend: I inode-table 0-f owner-inode data * shared . unrelated/free";
  if (what == "journal") return "legend: J journal-reserved 0-f last-writer tx S super . no journal signal";
  if (what == "owner") return "legend: 0-f owner inode modulo 16 . no owner";
  return "legend: . free 1 ref=1 2 ref=2 # ref>2";
}

std::string plainMapLegendZh(const std::string& what) {
  if (what == "blocks") return "图例: S super R refmeta I inode J journal P snapshot U users D data . 空闲";
  if (what == "inode") return "图例: I inode-table 0-f owner-inode 数据块 * 共享 . 无关/空闲";
  if (what == "journal") return "图例: J journal 保留区 0-f last-writer tx S super . 无 journal 信号";
  if (what == "owner") return "图例: 0-f owner inode modulo 16 . 无 owner";
  return "图例: . 空闲 1 ref=1 2 ref=2 # ref>2";
}

} // namespace

std::string toString(NodeType type) {
  switch (type) {
    case NodeType::Regular: return "regular";
    case NodeType::Directory: return "directory";
    case NodeType::SnapshotRoot: return "snapshot_root";
    case NodeType::ClassObject: return "class_object";
  }
  return "regular";
}

NodeType nodeTypeFromString(const std::string& value) {
  if (value == "directory") return NodeType::Directory;
  if (value == "snapshot_root") return NodeType::SnapshotRoot;
  if (value == "class_object") return NodeType::ClassObject;
  return NodeType::Regular;
}

std::string toString(MountState state) {
  switch (state) {
    case MountState::Clean: return "clean";
    case MountState::Dirty: return "dirty";
    case MountState::Recovering: return "recovering";
  }
  return "clean";
}

MountState mountStateFromString(const std::string& value) {
  if (value == "dirty") return MountState::Dirty;
  if (value == "recovering") return MountState::Recovering;
  return MountState::Clean;
}

FileSystemKernel::FileSystemKernel() : device_(trace_) {}

TraceSink& FileSystemKernel::trace() { return trace_; }

bool FileSystemKernel::isMounted() const { return mounted_; }

std::string FileSystemKernel::currentUser() const {
  return activeUser_.empty() ? "-" : activeUser_;
}

void FileSystemKernel::setInteractiveUi(bool enabled, bool ansi) {
  interactiveUi_ = enabled;
  ansiUi_ = ansi;
}

ui::KernelStatus FileSystemKernel::status() const {
  ui::KernelStatus s;
  s.mounted = mounted_;
  s.user = activeUser_.empty() ? "-" : activeUser_;
  s.cwd = activeUser_.empty() || sessions_.find(activeUser_) == sessions_.end() ? "/" : sessions_.at(activeUser_).cwd;
  s.mountState = mounted_ ? toString(state_.super.mountState) : "unmounted";
  s.txid = mounted_ ? state_.super.nextTxid : 0;
  s.inodes = state_.inodes.size();
  s.inodeTotal = config::kInodeCount;
  s.blocks = state_.blocks.size();
  s.blockTotal = config::kTotalBlocks;
  s.snapshots = state_.snapshots.size();
  s.trace = trace_.enabled();
  std::size_t open = 0;
  for (const auto& [user, sess] : sessions_) {
    open += sess.openFiles.size();
    (void)user;
  }
  s.openFiles = open;
  return s;
}

std::string FileSystemKernel::uiThemeName() const { return themeName_; }

std::string FileSystemKernel::uiLanguageName() const { return langName_; }

bool FileSystemKernel::uiAnsiEnabled() const { return ansiUi_; }

std::string FileSystemKernel::prompt() const {
  if (!mounted_) return "scopefs[unmounted]> ";
  const auto user = activeUser_.empty() ? "-" : activeUser_;
  const auto cwd = activeUser_.empty() ? "/" : session().cwd;
  return "scopefs[" + user + " " + cwd + "]> ";
}

std::vector<std::string> FileSystemKernel::completePath(const std::string& token, bool directoriesOnly) const {
  std::vector<std::string> matches;
  if (!mounted_ || state_.inodes.empty()) return matches;

  const auto slash = token.find_last_of('/');
  const std::string dirToken = slash == std::string::npos ? "." : (slash == 0 ? "/" : token.substr(0, slash));
  const std::string namePrefix = slash == std::string::npos ? token : token.substr(slash + 1);
  const std::string replacementPrefix = slash == std::string::npos ? "" : token.substr(0, slash + 1);
  const auto base = activeUser_.empty() || sessions_.find(activeUser_) == sessions_.end() ? "/" : sessions_.at(activeUser_).cwd;
  const auto dirPath = canonicalize(base, dirToken);

  std::uint32_t current = state_.super.activeRoot;
  for (const auto& part : pathParts(dirPath)) {
    const auto inodeIt = state_.inodes.find(current);
    if (inodeIt == state_.inodes.end() || inodeIt->second.type != NodeType::Directory) return matches;
    const auto entryIt = inodeIt->second.entries.find(part);
    if (entryIt == inodeIt->second.entries.end()) return matches;
    current = entryIt->second.inode;
  }

  const auto dirIt = state_.inodes.find(current);
  if (dirIt == state_.inodes.end() || dirIt->second.type != NodeType::Directory) return matches;
  for (const auto& [name, entry] : dirIt->second.entries) {
    if (!startsWith(name, namePrefix)) continue;
    const auto childIt = state_.inodes.find(entry.inode);
    if (directoriesOnly && (childIt == state_.inodes.end() || childIt->second.type != NodeType::Directory)) continue;
    matches.push_back(replacementPrefix + name);
  }
  std::sort(matches.begin(), matches.end());
  return matches;
}

CommandResult FileSystemKernel::ok(const std::string& out) {
  return {ErrorCode::Ok, "ok", out};
}

CommandResult FileSystemKernel::err(ErrorCode code, const std::string& message, const std::string& out) {
  return {code, message, out};
}

std::string FileSystemKernel::usage(const std::string& spec) const {
  if (langName_ == "en") return "usage: " + spec;
  std::string out = spec;
  const std::vector<std::pair<std::string, std::string>> repl = {
      {"<user_or_class>", "<用户或身份类>"},
      {"<class_name>", "<身份类名>"},
      {"[password]", "[密码]"},
      {"[mode]", "[模式]"},
      {"[size]", "[大小]"},
      {"[constraints]", "[约束]"},
      {"<password>", "<密码>"},
      {"<path|fd>", "<路径|fd>"},
      {"<constraints>", "<约束>"},
      {"<rights>", "<权限>"},
      {"<event>", "<事件>"},
      {"<class>", "<身份类>"},
      {"<file>", "<文件>"},
      {"<user>", "<用户>"},
      {"<path>", "<路径>"},
      {"<mode>", "<模式>"},
      {"<data>", "<数据>"},
      {"<size>", "<大小>"},
      {"<name>", "<名称>"},
      {"<src>", "<源>"},
      {"<dst>", "<目标>"},
      {"<seq>", "<序号>"},
      {"<a>", "<a>"},
      {"<b>", "<b>"}};
  for (const auto& [from, to] : repl) {
    std::size_t pos = 0;
    while ((pos = out.find(from, pos)) != std::string::npos) {
      out.replace(pos, from.size(), to);
      pos += to.size();
    }
  }
  return "用法: " + out;
}

std::string FileSystemKernel::msg(const std::string& en, const std::string& zh) const {
  return langName_ == "zh" ? zh : en;
}

bool FileSystemKernel::requiresLogin(const std::string& command) const {
  static const std::set<std::string> allowed = {
      "help", "exit", "format", "login", "trace", "theme", "lang"};
  if (allowed.count(command)) return false;
  return true;
}

bool FileSystemKernel::ensureLogged(CommandResult* result) const {
  if (!activeUser_.empty()) return true;
  if (result) *result = {ErrorCode::NeedLogin, msg("login required before file-system operations", "执行文件系统操作前需要先 login"), ""};
  return false;
}

bool FileSystemKernel::isRoot() const {
  return activeUser_ == "root" || activeUser_ == "admin";
}

UserSession& FileSystemKernel::session() { return sessions_.at(activeUser_); }

const UserSession& FileSystemKernel::session() const { return sessions_.at(activeUser_); }

void FileSystemKernel::boot() {
  device_.ensureWorkspace();
  if (!device_.volumeExists()) {
    trace_.emit(0, "mount.missing", "volume", "-", device_.volumePath(), "volume not found", "need_format");
    mounted_ = false;
    return;
  }
  deserializeState(device_.readAll());
  mounted_ = true;
  recoverIfNeeded();
  setMountState(MountState::Dirty);
  saveState();
  trace_.emit(0, "mount.open", "volume", "clean", "dirty", "normal mount", "ok");
}

void FileSystemKernel::unmountClean() {
  if (!mounted_) return;
  for (auto& [user, sess] : sessions_) {
    for (auto& [fd, of] : sess.openFiles) {
      auto it = state_.inodes.find(of.inode);
      if (it != state_.inodes.end() && it->second.openCount > 0) --it->second.openCount;
      if (systemOpen_[of.inode] > 0) --systemOpen_[of.inode];
    }
    sess.openFiles.clear();
  }
  setMountState(MountState::Clean);
  saveState();
  device_.clearJournal();
  trace_.save(config::tracePath(), nullptr);
  trace_.emit(0, "mount.close", "volume", "dirty", "clean", "normal unmount", "ok");
}

CommandResult FileSystemKernel::execute(const std::vector<std::string>& args, const std::string& rawCommand) {
  if (args.empty()) return ok();
  const auto cmd = lower(args[0]);
  trace_.setContext(currentUser(), rawCommand);
  if (mounted_) crashPoint("command.dispatch", "before");
  if (!mounted_ && cmd != "format" && cmd != "help" && cmd != "exit" && cmd != "trace" && cmd != "lang") {
    return err(ErrorCode::InvalidCommand, msg("volume is not mounted; run format first", "卷尚未挂载；请先运行 format"));
  }
  if (requiresLogin(cmd)) {
    CommandResult result;
    if (!ensureLogged(&result)) return result;
  }
  try {
    if (cmd == "format") return cmdFormat();
    if (cmd == "login") return cmdLogin(args);
    if (cmd == "logout") return cmdLogout();
    if (cmd == "whoami") return cmdWhoami();
    if (cmd == "mkdir") return cmdMkdir(args);
    if (cmd == "rmdir") return cmdRmdir(args);
    if (cmd == "chdir" || cmd == "cd") return cmdChdir(args);
    if (cmd == "dir" || cmd == "ls") return cmdDir(args);
    if (cmd == "create") return cmdCreate(args);
    if (cmd == "open") return cmdOpen(args);
    if (cmd == "read") return cmdRead(args);
    if (cmd == "write") return cmdWrite(args, rawCommand);
    if (cmd == "close") return cmdClose(args);
    if (cmd == "delete" || cmd == "rm") return cmdDelete(args);
    if (cmd == "truncate") return cmdTruncate(args);
    if (cmd == "trace") return cmdTrace(args);
    if (cmd == "scope") return cmdScope(args);
    if (cmd == "map") return cmdMap(args);
    if (cmd == "snapshot") return cmdSnapshot(args);
    if (cmd == "clone") return cmdClone(args);
    if (cmd == "class") return cmdClass(args);
    if (cmd == "acl") return cmdAcl(args);
    if (cmd == "chmod") return cmdChmod(args);
    if (cmd == "chown") return cmdChown(args);
    if (cmd == "chclass") return cmdChclass(args);
    if (cmd == "fsck") return cmdFsck(args);
    if (cmd == "crash") return cmdCrash(args);
    if (cmd == "theme") return cmdTheme(args);
    if (cmd == "lang") return cmdLang(args);
    if (cmd == "help") return cmdHelp();
  } catch (const CrashException&) {
    throw;
  } catch (const std::exception& ex) {
    trace_.emit(0, "command.error", cmd, "-", ex.what(), "uncaught exception", "error");
    return err(ErrorCode::IoError, ex.what());
  }
  return err(ErrorCode::InvalidCommand, msg("unknown command: ", "未知命令: ") + args[0]);
}

void FileSystemKernel::initFreshState() {
  state_ = FsState{};
  state_.super.version = config::kVersion;
  state_.super.blockSize = config::kBlockSize;
  state_.super.totalBlocks = config::kTotalBlocks;
  state_.super.inodeCount = config::kInodeCount;
  state_.super.activeRoot = 1;
  state_.super.nextInode = 1;
  state_.super.nextBlock = config::kDataStart;
  state_.super.nextTxid = 1;
  state_.super.mountState = MountState::Dirty;
  for (std::uint32_t i = 1; i <= config::kInodeCount; ++i) state_.freeInodes.insert(i);
  for (std::uint32_t b = config::kDataStart; b < config::kTotalBlocks; ++b) state_.freeBlocks.insert(b);
  for (std::uint32_t b = 0; b < config::kDataStart; ++b) {
    BlockInfo meta;
    meta.id = b;
    meta.refcount = 1;
    meta.flags = blockFlagsForId(b);
    state_.blocks[b] = meta;
  }

  auto makeUser = [&](const std::string& name, const std::string& pass, const std::string& home) {
    UserRecord u;
    u.name = name;
    u.passwordHash = hashPassword(name, pass);
    u.home = home;
    u.classes.insert(name);
    state_.users[name] = u;
    ClassDef c;
    c.name = name;
    c.owner = "root";
    c.members.insert(name);
    c.grantOption = false;
    state_.classes[name] = c;
  };
  makeUser("root", "root", "/root");
  makeUser("admin", "admin", "/root");
  for (int i = 1; i <= 8; ++i) {
    const auto u = "usr" + std::to_string(i);
    makeUser(u, u, "/home/" + u);
  }
  state_.classes["system"] = ClassDef{"system", "root", {}, {"root", "admin"}, true, "", 1};
  state_.users["root"].classes.insert("system");
  state_.users["admin"].classes.insert("system");

  const auto root = allocateInode(NodeType::Directory, "root", "system", 0755);
  state_.super.activeRoot = root;
  auto& rootNode = state_.inodes[root];
  rootNode.entries["."] = {".", NodeType::Directory, root, rootNode.generation};
  rootNode.entries[".."] = {"..", NodeType::Directory, root, rootNode.generation};

  const auto home = allocateInode(NodeType::Directory, "root", "system", 0755);
  auto& homeNode = state_.inodes[home];
  homeNode.entries["."] = {".", NodeType::Directory, home, homeNode.generation};
  homeNode.entries[".."] = {"..", NodeType::Directory, root, rootNode.generation};
  rootNode.entries["home"] = {"home", NodeType::Directory, home, homeNode.generation};

  const auto rootHome = allocateInode(NodeType::Directory, "root", "system", 0700);
  auto& rh = state_.inodes[rootHome];
  rh.entries["."] = {".", NodeType::Directory, rootHome, rh.generation};
  rh.entries[".."] = {"..", NodeType::Directory, root, rootNode.generation};
  rootNode.entries["root"] = {"root", NodeType::Directory, rootHome, rh.generation};

  for (int i = 1; i <= 8; ++i) {
    const auto u = "usr" + std::to_string(i);
    const auto dir = allocateInode(NodeType::Directory, u, u, 0750);
    auto& node = state_.inodes[dir];
    node.entries["."] = {".", NodeType::Directory, dir, node.generation};
    node.entries[".."] = {"..", NodeType::Directory, home, homeNode.generation};
    homeNode.entries[u] = {u, NodeType::Directory, dir, node.generation};
    refreshDirBlock(node, 0);
  }
  refreshDirBlock(homeNode, 0);
  refreshDirBlock(rh, 0);
  refreshDirBlock(rootNode, 0);
  recomputeSuper();
}

void FileSystemKernel::saveState() {
  recomputeSuper();
  device_.writeAll(serializeState());
}

void FileSystemKernel::loadState(const std::string& text) {
  deserializeState(text);
}

std::string FileSystemKernel::serializeState() const {
  std::ostringstream out;
  const auto& s = state_.super;
  out << "S|" << s.version << '|' << s.blockSize << '|' << s.totalBlocks << '|' << s.inodeCount
      << '|' << s.freeBlocks << '|' << s.freeInodes << '|' << s.activeRoot << '|' << s.nextInode
      << '|' << s.nextBlock << '|' << s.nextTxid << '|' << toString(s.mountState) << '|' << s.checksum << '\n';
  for (const auto& [id, inode] : state_.inodes) {
    out << "I|" << id << '|' << inode.generation << '|' << toString(inode.type) << '|'
        << encode(inode.owner) << '|' << encode(inode.klass) << '|' << inode.mode << '|'
        << inode.size << '|' << inode.nlink << '|' << inode.openCount << '|' << inode.refcount << '|'
        << yn(inode.deletePending) << '|' << csvNumbers(inode.blocks) << '\n';
    for (const auto& [name, de] : inode.entries) {
      out << "D|" << id << '|' << encode(name) << '|' << toString(de.type) << '|' << de.inode << '|' << de.generation << '\n';
    }
    for (const auto& acl : inode.acl) {
      out << "A|" << id << '|' << encode(acl.subject) << '|' << encode(acl.rights) << '|'
          << encode(acl.constraints) << '|' << acl.generation << '\n';
    }
  }
  for (const auto& [id, block] : state_.blocks) {
    out << "B|" << id << '|' << block.refcount << '|' << block.checksum << '|' << block.lastWriterTxid
        << '|' << encode(block.flags) << '|' << block.ownerInode << '|' << encode(block.data) << '\n';
  }
  for (const auto& [name, user] : state_.users) {
    out << "U|" << encode(name) << '|' << encode(user.passwordHash) << '|' << encode(user.home)
        << '|' << encode(joinSet(user.classes, ',')) << '\n';
  }
  for (const auto& [name, cls] : state_.classes) {
    out << "C|" << encode(name) << '|' << encode(cls.owner) << '|' << encode(joinSet(cls.parents, ','))
        << '|' << encode(joinSet(cls.members, ',')) << '|' << yn(cls.grantOption) << '|'
        << encode(cls.constraints) << '|' << cls.generation << '\n';
  }
  for (const auto& [name, snap] : state_.snapshots) {
    out << "P|" << encode(name) << '|' << snap.rootInode << '|' << snap.generation << '|'
        << snap.txid << '|' << encode(snap.createdAt) << '\n';
  }
  return out.str();
}

void FileSystemKernel::deserializeState(const std::string& text) {
  state_ = FsState{};
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    const auto f = split(line, '|');
    if (f.empty()) continue;
    if (f[0] == "S" && f.size() >= 13) {
      state_.super.version = static_cast<std::uint32_t>(std::stoul(f[1]));
      state_.super.blockSize = static_cast<std::uint32_t>(std::stoul(f[2]));
      state_.super.totalBlocks = static_cast<std::uint32_t>(std::stoul(f[3]));
      state_.super.inodeCount = static_cast<std::uint32_t>(std::stoul(f[4]));
      state_.super.freeBlocks = static_cast<std::uint32_t>(std::stoul(f[5]));
      state_.super.freeInodes = static_cast<std::uint32_t>(std::stoul(f[6]));
      state_.super.activeRoot = static_cast<std::uint32_t>(std::stoul(f[7]));
      state_.super.nextInode = static_cast<std::uint32_t>(std::stoul(f[8]));
      state_.super.nextBlock = static_cast<std::uint32_t>(std::stoul(f[9]));
      state_.super.nextTxid = std::stoull(f[10]);
      state_.super.mountState = mountStateFromString(f[11]);
      state_.super.checksum = static_cast<std::uint32_t>(std::stoul(f[12]));
    } else if (f[0] == "I" && f.size() >= 13) {
      Inode inode;
      inode.id = static_cast<std::uint32_t>(std::stoul(f[1]));
      inode.generation = static_cast<std::uint32_t>(std::stoul(f[2]));
      inode.type = nodeTypeFromString(f[3]);
      inode.owner = decode(f[4]);
      inode.klass = decode(f[5]);
      inode.mode = std::stoi(f[6]);
      inode.size = std::stoull(f[7]);
      inode.nlink = static_cast<std::uint32_t>(std::stoul(f[8]));
      inode.openCount = static_cast<std::uint32_t>(std::stoul(f[9]));
      inode.refcount = static_cast<std::uint32_t>(std::stoul(f[10]));
      inode.deletePending = parseBool(f[11]);
      inode.blocks = parseCsvNumbers<std::uint32_t>(f[12]);
      state_.inodes[inode.id] = inode;
    } else if (f[0] == "D" && f.size() >= 6) {
      const auto parent = static_cast<std::uint32_t>(std::stoul(f[1]));
      DirEntry de;
      de.name = decode(f[2]);
      de.type = nodeTypeFromString(f[3]);
      de.inode = static_cast<std::uint32_t>(std::stoul(f[4]));
      de.generation = static_cast<std::uint32_t>(std::stoul(f[5]));
      state_.inodes[parent].entries[de.name] = de;
    } else if (f[0] == "A" && f.size() >= 6) {
      const auto inode = static_cast<std::uint32_t>(std::stoul(f[1]));
      AclEntry acl;
      acl.subject = decode(f[2]);
      acl.rights = decode(f[3]);
      acl.constraints = decode(f[4]);
      acl.generation = static_cast<std::uint32_t>(std::stoul(f[5]));
      state_.inodes[inode].acl.push_back(acl);
    } else if (f[0] == "B" && f.size() >= 8) {
      BlockInfo block;
      block.id = static_cast<std::uint32_t>(std::stoul(f[1]));
      block.refcount = static_cast<std::uint32_t>(std::stoul(f[2]));
      block.checksum = static_cast<std::uint32_t>(std::stoul(f[3]));
      block.lastWriterTxid = std::stoull(f[4]);
      block.flags = decode(f[5]);
      block.ownerInode = static_cast<std::uint32_t>(std::stoul(f[6]));
      block.data = decode(f[7]);
      state_.blocks[block.id] = block;
    } else if (f[0] == "U" && f.size() >= 5) {
      UserRecord u;
      u.name = decode(f[1]);
      u.passwordHash = decode(f[2]);
      u.home = decode(f[3]);
      u.classes = splitSet(decode(f[4]), ',');
      state_.users[u.name] = u;
    } else if (f[0] == "C" && f.size() >= 8) {
      ClassDef c;
      c.name = decode(f[1]);
      c.owner = decode(f[2]);
      c.parents = splitSet(decode(f[3]), ',');
      c.members = splitSet(decode(f[4]), ',');
      c.grantOption = parseBool(f[5]);
      c.constraints = decode(f[6]);
      c.generation = static_cast<std::uint32_t>(std::stoul(f[7]));
      state_.classes[c.name] = c;
    } else if (f[0] == "P" && f.size() >= 6) {
      Snapshot p;
      p.name = decode(f[1]);
      p.rootInode = static_cast<std::uint32_t>(std::stoul(f[2]));
      p.generation = static_cast<std::uint32_t>(std::stoul(f[3]));
      p.txid = std::stoull(f[4]);
      p.createdAt = decode(f[5]);
      state_.snapshots[p.name] = p;
    }
  }
  state_.freeInodes.clear();
  for (std::uint32_t i = 1; i <= config::kInodeCount; ++i) {
    if (!state_.inodes.count(i)) state_.freeInodes.insert(i);
  }
  state_.freeBlocks.clear();
  for (std::uint32_t b = config::kDataStart; b < config::kTotalBlocks; ++b) {
    if (!state_.blocks.count(b) || state_.blocks[b].refcount == 0) state_.freeBlocks.insert(b);
  }
}

void FileSystemKernel::recomputeSuper() {
  state_.super.freeBlocks = static_cast<std::uint32_t>(state_.freeBlocks.size());
  state_.super.freeInodes = static_cast<std::uint32_t>(state_.freeInodes.size());
  std::uint32_t nextInode = 1;
  while (state_.inodes.count(nextInode) && nextInode <= config::kInodeCount) ++nextInode;
  state_.super.nextInode = nextInode;
  std::uint32_t nextBlock = config::kDataStart;
  while (state_.blocks.count(nextBlock) && state_.blocks[nextBlock].refcount != 0 && nextBlock < config::kTotalBlocks) ++nextBlock;
  state_.super.nextBlock = nextBlock;
  state_.super.checksum = checksumFields({
      std::to_string(state_.super.version),
      std::to_string(state_.super.blockSize),
      std::to_string(state_.super.totalBlocks),
      std::to_string(state_.super.inodeCount),
      std::to_string(state_.super.freeBlocks),
      std::to_string(state_.super.freeInodes),
      std::to_string(state_.super.activeRoot),
      std::to_string(state_.super.nextInode),
      std::to_string(state_.super.nextBlock),
      std::to_string(state_.super.nextTxid),
      toString(state_.super.mountState)});
}

void FileSystemKernel::setMountState(MountState state) {
  const auto before = toString(state_.super.mountState);
  state_.super.mountState = state;
  recomputeSuper();
  trace_.emit(0, "super.mount_state", "superblock", before, toString(state), "state transition", "ok");
}

void FileSystemKernel::recoverIfNeeded() {
  const auto journal = device_.readJournal();
  if (state_.super.mountState == MountState::Clean && journal.empty()) return;
  trace_.emit(0, "recovery.begin", "journal", toString(state_.super.mountState), std::to_string(journal.size()), "dirty mount or journal present", "start");
  setMountState(MountState::Recovering);
  recoveryFromJournal(journal);
  const auto report = fsck(false, true);
  trace_.emit(0, "fsck.light", "volume", "-", report, "automatic recovery check", "ok");
  device_.clearJournal();
  setMountState(MountState::Dirty);
  saveState();
  trace_.emit(0, "recovery.end", "journal", "-", "dirty", "redo complete", "ok");
}

void FileSystemKernel::recoveryFromJournal(const std::vector<std::string>& journal) {
  std::map<std::uint64_t, std::string> after;
  std::set<std::uint64_t> committed;
  std::set<std::uint64_t> checkpointed;
  for (const auto& line : journal) {
    const auto f = split(line, '|');
    if (f.size() >= 4 && f[0] == "RECORD" && f[2] == "after") {
      after[std::stoull(f[1])] = fromHex(f[3]);
    } else if (f.size() >= 2 && f[0] == "COMMIT") {
      committed.insert(std::stoull(f[1]));
    } else if (f.size() >= 2 && f[0] == "CHECKPOINT") {
      checkpointed.insert(std::stoull(f[1]));
    }
  }
  for (const auto txid : committed) {
    if (checkpointed.count(txid)) continue;
    auto it = after.find(txid);
    if (it == after.end()) continue;
    trace_.emit(txid, "recovery.redo", "tx", "-", std::to_string(txid), "committed transaction without checkpoint", "redo");
    deserializeState(it->second);
  }
  for (const auto& [txid, image] : after) {
    if (!committed.count(txid)) {
      trace_.emit(txid, "recovery.ignore", "tx", "-", std::to_string(txid), "no commit record", "ignored");
    }
    (void)image;
  }
}

FileSystemKernel::Tx FileSystemKernel::beginTx(const std::string& name) {
  Tx tx;
  tx.id = state_.super.nextTxid++;
  tx.name = name;
  setMountState(MountState::Dirty);
  saveState();
  tx.before = serializeState();
  tx.active = true;
  crashPoint("journal.begin", "before");
  journalLine("BEGIN|" + std::to_string(tx.id) + "|" + encode(name));
  trace_.emit(tx.id, "journal.begin", name, "-", "-", "transaction begin", "ok");
  crashPoint("journal.begin", "after");
  crashPoint("journal.record", "before");
  journalLine("RECORD|" + std::to_string(tx.id) + "|before|" + toHex(tx.before));
  trace_.emit(tx.id, "journal.record", name, "-", "before_image", "record before state", "ok");
  crashPoint("journal.record", "after");
  return tx;
}

void FileSystemKernel::commitTx(Tx& tx) {
  if (!tx.active) return;
  const auto after = serializeState();
  crashPoint("journal.record", "before");
  journalLine("RECORD|" + std::to_string(tx.id) + "|after|" + toHex(after));
  trace_.emit(tx.id, "journal.record", tx.name, "-", "after_image", "record redo state", "ok");
  crashPoint("journal.record", "after");
  crashPoint("journal.commit", "before");
  journalLine("COMMIT|" + std::to_string(tx.id));
  trace_.emit(tx.id, "journal.commit", tx.name, "-", "-", "commit durable", "ok");
  crashPoint("journal.commit", "after");
  crashPoint("journal.checkpoint", "before");
  saveState();
  journalLine("CHECKPOINT|" + std::to_string(tx.id));
  trace_.emit(tx.id, "journal.checkpoint", tx.name, "-", "home_blocks", "checkpoint to volume", "ok");
  device_.clearJournal();
  crashPoint("journal.checkpoint", "after");
  tx.active = false;
}

void FileSystemKernel::journalLine(const std::string& line) {
  device_.appendJournal(line);
}

void FileSystemKernel::crashPoint(const std::string& event, const std::string& phase) {
  const auto full = phase + "." + event;
  trace_.emit(0, "crash.point", full, "-", "-", "instrumented crash point", "armed_check");
  if (crashMode_ == "before" && crashEvent_ == event && phase == "before") maybeCrashNow();
  if (crashMode_ == "after" && crashEvent_ == event && phase == "after") maybeCrashNow();
  if (crashMode_ == "at" && trace_.nextSeq() >= crashSeq_) maybeCrashNow();
}

void FileSystemKernel::maybeCrashNow() {
  trace_.emit(0, "crash.inject", crashMode_, "-", crashEvent_, "simulated abnormal exit", "crash");
  throw CrashException(crashMode_.empty() ? "now" : crashMode_ + ":" + crashEvent_);
}

std::uint32_t FileSystemKernel::allocateInode(NodeType type, const std::string& owner, const std::string& klass, int mode) {
  if (state_.freeInodes.empty()) throw std::runtime_error("no free inode");
  const auto id = *state_.freeInodes.begin();
  state_.freeInodes.erase(id);
  Inode inode;
  inode.id = id;
  inode.type = type;
  inode.owner = owner;
  inode.klass = klass;
  inode.mode = mode;
  inode.generation = 1;
  inode.refcount = 1;
  state_.inodes[id] = inode;
  trace_.emit(0, "inode.alloc", std::to_string(id), "-", toString(type), "allocate inode", "ok");
  return id;
}

std::uint32_t FileSystemKernel::allocateBlock(std::uint32_t ownerInode, const std::string& data, const std::string& flags, std::uint64_t txid) {
  if (state_.freeBlocks.empty()) throw std::runtime_error("no free block");
  const auto id = *state_.freeBlocks.begin();
  state_.freeBlocks.erase(id);
  BlockInfo b;
  b.id = id;
  b.refcount = 1;
  b.data = data;
  b.checksum = checksum32(data);
  b.lastWriterTxid = txid;
  b.flags = flags;
  b.ownerInode = ownerInode;
  state_.blocks[id] = b;
  trace_.emit(txid, "block.alloc", std::to_string(id), "free", "ref=1", "allocate data block", "ok");
  return id;
}

void FileSystemKernel::retainInode(std::uint32_t inode) {
  auto it = state_.inodes.find(inode);
  if (it == state_.inodes.end()) return;
  ++it->second.refcount;
  trace_.emit(0, "inode.retain", std::to_string(inode), "-", std::to_string(it->second.refcount), "shared reference", "ok");
}

void FileSystemKernel::releaseInode(std::uint32_t inode, bool recursive) {
  auto it = state_.inodes.find(inode);
  if (it == state_.inodes.end()) return;
  if (it->second.refcount > 0) --it->second.refcount;
  trace_.emit(0, "inode.release", std::to_string(inode), "-", std::to_string(it->second.refcount), "drop reference", "ok");
  if (it->second.refcount != 0) return;
  if (it->second.openCount > 0) {
    it->second.deletePending = true;
    return;
  }
  if (recursive && it->second.type == NodeType::Directory) {
    const auto entries = it->second.entries;
    for (const auto& [name, de] : entries) {
      if (name == "." || name == "..") continue;
      releaseInode(de.inode, true);
    }
  }
  for (auto b : it->second.blocks) releaseBlock(b);
  state_.freeInodes.insert(inode);
  state_.inodes.erase(it);
  trace_.emit(0, "inode.free", std::to_string(inode), "allocated", "free", "last reference dropped", "ok");
}

void FileSystemKernel::retainBlock(std::uint32_t block) {
  auto& b = state_.blocks[block];
  ++b.refcount;
  state_.freeBlocks.erase(block);
  trace_.emit(0, "block.retain", std::to_string(block), "-", std::to_string(b.refcount), "shared block", "ok");
}

void FileSystemKernel::releaseBlock(std::uint32_t block) {
  auto it = state_.blocks.find(block);
  if (it == state_.blocks.end()) return;
  if (it->second.refcount > 0) --it->second.refcount;
  trace_.emit(0, "block.release", std::to_string(block), "-", std::to_string(it->second.refcount), "drop block ref", "ok");
  if (it->second.refcount == 0 && block >= config::kDataStart) {
    state_.freeBlocks.insert(block);
    state_.blocks.erase(it);
  }
}

std::string FileSystemKernel::readFileData(const Inode& inode) const {
  std::string data;
  for (auto block : inode.blocks) {
    auto it = state_.blocks.find(block);
    if (it != state_.blocks.end()) data += it->second.data;
  }
  if (data.size() > inode.size) data.resize(static_cast<std::size_t>(inode.size));
  return data;
}

void FileSystemKernel::setFileData(Inode& inode, const std::string& data, std::uint64_t txid) {
  for (auto b : inode.blocks) {
    const auto it = state_.blocks.find(b);
    if (it != state_.blocks.end() && it->second.refcount > 1) {
      trace_.emit(txid, "cow.break", std::to_string(b), std::to_string(it->second.refcount), "new block", "write shared block", "ok");
    }
  }
  const auto oldBlocks = inode.blocks;
  inode.blocks.clear();
  for (auto b : oldBlocks) releaseBlock(b);
  std::size_t pos = 0;
  while (pos < data.size() || (data.empty() && pos == 0)) {
    const auto chunk = data.substr(pos, config::kBlockSize);
    if (!chunk.empty() || data.empty()) {
      inode.blocks.push_back(allocateBlock(inode.id, chunk, "data", txid));
    }
    if (data.empty()) break;
    pos += chunk.size();
  }
  inode.size = data.size();
  ++inode.generation;
  trace_.emit(txid, "inode.write_map", std::to_string(inode.id), csvNumbers(oldBlocks), csvNumbers(inode.blocks), "file block map updated", "ok");
}

void FileSystemKernel::refreshDirBlock(Inode& inode, std::uint64_t txid) {
  std::ostringstream data;
  for (const auto& [name, de] : inode.entries) {
    data << name << ':' << de.inode << ':' << toString(de.type) << '\n';
  }
  setFileData(inode, data.str(), txid);
}

FileSystemKernel::ResolvedPath FileSystemKernel::resolve(const std::string& path, bool mustExist) {
  ResolvedPath result;
  result.canonical = canonicalize(activeUser_.empty() ? "/" : session().cwd, path);
  const auto parts = pathParts(result.canonical);
  std::uint32_t current = state_.super.activeRoot;
  std::string currentPath = "/";
  trace_.emit(0, "path.lookup", "/", "-", std::to_string(current), "start path lookup", "ok");
  if (parts.empty()) {
    result.inode = current;
    result.parent = current;
    result.parentPath = "/";
    result.leaf = "/";
    return result;
  }
  for (std::size_t i = 0; i < parts.size(); ++i) {
    auto inodeIt = state_.inodes.find(current);
    if (inodeIt == state_.inodes.end()) throw std::runtime_error("path lookup reached missing inode");
    if (inodeIt->second.type != NodeType::Directory) {
      throw std::runtime_error("path component is not a directory: " + currentPath);
    }
    const auto entryIt = inodeIt->second.entries.find(parts[i]);
    const bool last = i + 1 == parts.size();
    if (entryIt == inodeIt->second.entries.end()) {
      if (last && !mustExist) {
        result.parent = current;
        result.inode = 0;
        result.leaf = parts[i];
        result.parentPath = currentPath;
        return result;
      }
      throw std::runtime_error("path not found: " + result.canonical);
    }
    result.parent = current;
    result.parentPath = currentPath;
    result.leaf = parts[i];
    current = entryIt->second.inode;
    currentPath = pathJoin(currentPath, parts[i]);
    trace_.emit(0, "path.lookup", currentPath, "-", std::to_string(current), "resolve component " + parts[i], "ok");
  }
  result.inode = current;
  return result;
}

std::string FileSystemKernel::canonicalize(const std::string& base, const std::string& path) const {
  std::vector<std::string> parts;
  if (!path.empty() && path[0] == '/') {
    parts = pathParts(path);
  } else {
    parts = pathParts(base + "/" + path);
  }
  if (parts.empty()) return "/";
  return "/" + join(parts, '/');
}

bool FileSystemKernel::ensureMutablePath(const std::string& targetPath, bool includeLeaf, std::uint64_t txid, ResolvedPath* updated) {
  const auto canonical = canonicalize(activeUser_.empty() ? "/" : session().cwd, targetPath);
  const auto parts = pathParts(canonical);
  if (state_.inodes[state_.super.activeRoot].refcount > 1) {
    const auto oldRoot = state_.super.activeRoot;
    const auto newRoot = cloneInode(oldRoot, txid);
    releaseInode(oldRoot, false);
    state_.super.activeRoot = newRoot;
    auto& nr = state_.inodes[newRoot];
    nr.entries["."] = {".", NodeType::Directory, newRoot, nr.generation};
    nr.entries[".."] = {"..", NodeType::Directory, newRoot, nr.generation};
    refreshDirBlock(nr, txid);
    trace_.emit(txid, "cow.path_root", "/", std::to_string(oldRoot), std::to_string(newRoot), "active root was shared", "ok");
  }
  std::uint32_t current = state_.super.activeRoot;
  const std::size_t limit = includeLeaf ? parts.size() : (parts.empty() ? 0 : parts.size() - 1);
  for (std::size_t i = 0; i < limit; ++i) {
    auto& parent = state_.inodes[current];
    auto entryIt = parent.entries.find(parts[i]);
    if (entryIt == parent.entries.end()) break;
    const auto child = entryIt->second.inode;
    if (state_.inodes[child].refcount > 1) {
      const auto replacement = cloneInode(child, txid);
      releaseInode(child, false);
      entryIt->second.inode = replacement;
      entryIt->second.generation = state_.inodes[replacement].generation;
      current = replacement;
      refreshDirBlock(parent, txid);
      trace_.emit(txid, "cow.path_node", parts[i], std::to_string(child), std::to_string(replacement), "path component was shared", "ok");
    } else {
      current = child;
    }
  }
  if (updated) *updated = resolve(canonical, includeLeaf);
  return true;
}

std::uint32_t FileSystemKernel::cloneInode(std::uint32_t src, std::uint64_t txid) {
  const auto old = state_.inodes.at(src);
  const auto id = allocateInode(old.type, old.owner, old.klass, old.mode);
  auto& clone = state_.inodes[id];
  clone.generation = old.generation + 1;
  clone.size = old.size;
  clone.nlink = old.nlink;
  clone.acl = old.acl;
  clone.entries = old.entries;
  clone.blocks = old.blocks;
  clone.refcount = 1;
  for (auto b : clone.blocks) retainBlock(b);
  if (clone.type == NodeType::Directory) {
    for (const auto& [name, de] : clone.entries) {
      if (name == "." || name == "..") continue;
      retainInode(de.inode);
    }
    clone.entries["."] = {".", NodeType::Directory, id, clone.generation};
  }
  trace_.emit(txid, "inode.clone", std::to_string(src), "-", std::to_string(id), "COW inode clone", "ok");
  return id;
}

bool FileSystemKernel::authCheck(std::uint32_t inodeId, const std::string& right, const std::string& path, std::string* reason) {
  if (isRoot()) {
    if (reason) *reason = "root bypass";
    trace_.emit(0, "auth.check", path, "-", right, "root bypass", "allow");
    return true;
  }
  const auto inodeIt = state_.inodes.find(inodeId);
  if (inodeIt == state_.inodes.end()) {
    if (reason) *reason = "target inode missing";
    return false;
  }
  const auto& inode = inodeIt->second;
  auto modeAllows = [&](int shift) {
    const int bits = (inode.mode >> shift) & 7;
    if (right == "r") return (bits & 4) != 0;
    if (right == "w" || right == "c" || right == "d" || right == "s" || right == "g") return (bits & 2) != 0;
    if (right == "x") return (bits & 1) != 0;
    return false;
  };
  if (inode.owner == activeUser_ && modeAllows(6)) {
    if (reason) *reason = "owner mode allows";
    trace_.emit(0, "auth.check", path, "-", right, "owner mode", "allow");
    return true;
  }
  const auto classes = effectiveClasses(activeUser_, path);
  if (classes.count(inode.klass) && modeAllows(3)) {
    if (reason) *reason = "file class mode allows via " + inode.klass;
    trace_.emit(0, "auth.check", path, "-", right, "class mode", "allow");
    return true;
  }
  if (modeAllows(0)) {
    if (reason) *reason = "other mode allows";
    trace_.emit(0, "auth.check", path, "-", right, "other mode", "allow");
    return true;
  }
  for (const auto& acl : inode.acl) {
    const bool subjectMatch = acl.subject == activeUser_ || classes.count(acl.subject) != 0;
    if (subjectMatch && rightsContain(acl.rights, right) && constraintsAllow(acl.constraints, path)) {
      if (reason) *reason = "ACL allows " + acl.subject;
      trace_.emit(0, "auth.check", path, "-", right, "ACL " + acl.subject, "allow");
      return true;
    }
  }
  if (reason) *reason = "default deny after owner/class/ACL checks";
  trace_.emit(0, "auth.check", path, "-", right, "default deny", "deny");
  return false;
}

bool FileSystemKernel::canTraverse(const std::string& canonical, bool includeTarget, std::string* reason) {
  if (isRoot()) {
    if (reason) *reason = "root bypass";
    return true;
  }
  const auto parts = pathParts(canonical);
  std::uint32_t current = state_.super.activeRoot;
  std::string currentPath = "/";
  std::size_t limit = includeTarget ? parts.size() : (parts.empty() ? 0 : parts.size() - 1);
  if (limit == 0) {
    return authCheck(current, "x", "/", reason);
  }
  for (std::size_t i = 0; i < limit; ++i) {
    auto& node = state_.inodes[current];
    auto it = node.entries.find(parts[i]);
    if (it == node.entries.end()) {
      if (reason) *reason = "path component missing during traversal";
      return false;
    }
    current = it->second.inode;
    currentPath = pathJoin(currentPath, parts[i]);
    auto inodeIt = state_.inodes.find(current);
    if (inodeIt == state_.inodes.end() || inodeIt->second.type != NodeType::Directory) {
      if (i + 1 == limit && !includeTarget) return true;
      if (reason) *reason = currentPath + " is not a traversable directory";
      return false;
    }
    if (!authCheck(current, "x", currentPath, reason)) return false;
  }
  return true;
}

std::set<std::string> FileSystemKernel::effectiveClasses(const std::string& user, const std::string& path) const {
  std::set<std::string> out;
  auto userIt = state_.users.find(user);
  if (userIt == state_.users.end()) return out;
  std::queue<std::string> q;
  for (const auto& c : userIt->second.classes) q.push(c);
  while (!q.empty()) {
    const auto c = q.front();
    q.pop();
    if (out.count(c)) continue;
    auto clsIt = state_.classes.find(c);
    if (clsIt != state_.classes.end() && !constraintsAllow(clsIt->second.constraints, path)) continue;
    out.insert(c);
    if (clsIt != state_.classes.end()) {
      for (const auto& p : clsIt->second.parents) q.push(p);
    }
  }
  return out;
}

bool FileSystemKernel::constraintsAllow(const std::string& constraints, const std::string& path) const {
  if (constraints.empty() || constraints == "-") return true;
  for (const auto& part : split(constraints, ',')) {
    const auto item = trim(part);
    if (startsWith(item, "path_prefix=")) {
      const auto prefix = item.substr(std::string("path_prefix=").size());
      if (!startsWith(path, prefix)) return false;
    }
  }
  return true;
}

CommandResult FileSystemKernel::cmdFormat() {
  initFreshState();
  mounted_ = true;
  activeUser_.clear();
  sessions_.clear();
  systemOpen_.clear();
  auto tx = beginTx("format");
  commitTx(tx);
  std::ostringstream out;
  out << msg("ScopeFS formatted at ", "ScopeFS 已格式化: ") << device_.volumePath() << "\n"
      << msg("root/admin password: root/admin, usr1..usr8 password equals username",
             "root/admin 密码: root/admin，usr1..usr8 密码等于用户名") << "\n"
      << msg("layout: ", "布局: ") << "blocks=" << config::kTotalBlocks << " block_size=" << config::kBlockSize
      << " data_start=" << config::kDataStart << "\n";
  return ok(out.str());
}

CommandResult FileSystemKernel::cmdLogin(const std::vector<std::string>& args) {
  if (args.size() < 2) return err(ErrorCode::InvalidArgument, usage("login <user> [password]"));
  const auto user = args[1];
  const auto password = args.size() >= 3 ? args[2] : "";
  auto it = state_.users.find(user);
  if (it == state_.users.end()) return err(ErrorCode::AuthFailed, msg("unknown user", "未知用户"));
  if (it->second.passwordHash != hashPassword(user, password)) {
    return err(ErrorCode::AuthFailed, msg("invalid password", "密码错误"));
  }
  if (!sessions_.count(user)) {
    UserSession s;
    s.user = user;
    s.cwd = it->second.home.empty() ? "/" : it->second.home;
    sessions_[user] = s;
  }
  activeUser_ = user;
  trace_.setContext(activeUser_, "login");
  trace_.emit(0, "session.login", user, "-", sessions_[user].cwd, "user authenticated", "ok");
  return ok(msg("logged in as ", "已登录: ") + user + "\n");
}

CommandResult FileSystemKernel::cmdLogout() {
  if (activeUser_.empty()) return err(ErrorCode::NeedLogin, "no active session");
  auto& s = session();
  for (const auto& [fd, of] : s.openFiles) {
    auto it = state_.inodes.find(of.inode);
    if (it != state_.inodes.end() && it->second.openCount > 0) --it->second.openCount;
    if (systemOpen_[of.inode] > 0) --systemOpen_[of.inode];
    trace_.emit(0, "open.close", std::to_string(fd), "-", std::to_string(of.inode), "logout closes fd", "ok");
  }
  s.openFiles.clear();
  const auto old = activeUser_;
  activeUser_.clear();
  saveState();
  return ok(msg("logged out ", "已登出: ") + old + "\n");
}

CommandResult FileSystemKernel::cmdWhoami() {
  std::ostringstream out;
  out << "user: " << activeUser_ << "\n";
  out << "cwd : " << session().cwd << "\n";
  out << "classes: " << joinSet(effectiveClasses(activeUser_, session().cwd), ',') << "\n";
  return ok(out.str());
}

CommandResult FileSystemKernel::cmdMkdir(const std::vector<std::string>& args) {
  if (args.size() < 2) return err(ErrorCode::InvalidArgument, usage("mkdir <path>"));
  ResolvedPath rp;
  try { rp = resolve(args[1], false); } catch (const std::exception& ex) { return err(ErrorCode::NotFound, ex.what()); }
  if (rp.inode != 0) return err(ErrorCode::Exists, "path already exists");
  std::string reason;
  if (!canTraverse(rp.parentPath, true, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
  if (!authCheck(rp.parent, "c", rp.parentPath, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
  auto tx = beginTx("mkdir");
  ensureMutablePath(rp.parentPath, true, tx.id, nullptr);
  rp = resolve(args[1], false);
  const auto id = allocateInode(NodeType::Directory, activeUser_, activeUser_, 0755);
  auto& node = state_.inodes[id];
  node.entries["."] = {".", NodeType::Directory, id, node.generation};
  node.entries[".."] = {"..", NodeType::Directory, rp.parent, state_.inodes[rp.parent].generation};
  refreshDirBlock(node, tx.id);
  auto& parent = state_.inodes[rp.parent];
  parent.entries[rp.leaf] = {rp.leaf, NodeType::Directory, id, node.generation};
  refreshDirBlock(parent, tx.id);
  trace_.emit(tx.id, "dir.mkdir", rp.canonical, "-", std::to_string(id), "create directory inode and entry", "ok");
  commitTx(tx);
  return ok(msg("created directory ", "已创建目录 ") + rp.canonical + " inode=" + std::to_string(id) + "\n");
}

CommandResult FileSystemKernel::cmdRmdir(const std::vector<std::string>& args) {
  if (args.size() < 2) return err(ErrorCode::InvalidArgument, usage("rmdir <path>"));
  ResolvedPath rp;
  try { rp = resolve(args[1], true); } catch (const std::exception& ex) { return err(ErrorCode::NotFound, ex.what()); }
  if (rp.canonical == "/") return err(ErrorCode::InvalidArgument, "cannot remove root");
  auto& node = state_.inodes[rp.inode];
  if (node.type != NodeType::Directory) return err(ErrorCode::NotDirectory, "target is not a directory");
  for (const auto& [name, de] : node.entries) {
    if (name != "." && name != "..") return err(ErrorCode::DirectoryNotEmpty, "directory is not empty");
    (void)de;
  }
  if (startsWith(session().cwd + "/", rp.canonical + "/")) {
    return err(ErrorCode::Busy, "cannot remove current cwd ancestor");
  }
  std::string reason;
  if (!canTraverse(rp.parentPath, true, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
  if (!authCheck(rp.parent, "d", rp.parentPath, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
  auto tx = beginTx("rmdir");
  ensureMutablePath(rp.parentPath, true, tx.id, nullptr);
  rp = resolve(args[1], true);
  auto& parent = state_.inodes[rp.parent];
  parent.entries.erase(rp.leaf);
  refreshDirBlock(parent, tx.id);
  releaseInode(rp.inode, true);
  trace_.emit(tx.id, "dir.rmdir", rp.canonical, std::to_string(rp.inode), "deleted", "remove empty directory", "ok");
  commitTx(tx);
  return ok(msg("removed directory ", "已删除目录 ") + rp.canonical + "\n");
}

CommandResult FileSystemKernel::cmdChdir(const std::vector<std::string>& args) {
  if (args.size() < 2) return err(ErrorCode::InvalidArgument, usage("chdir <path>"));
  ResolvedPath rp;
  try { rp = resolve(args[1], true); } catch (const std::exception& ex) { return err(ErrorCode::NotFound, ex.what()); }
  if (state_.inodes[rp.inode].type != NodeType::Directory) return err(ErrorCode::NotDirectory, "target is not a directory");
  std::string reason;
  if (!canTraverse(rp.canonical, true, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
  if (!authCheck(rp.inode, "x", rp.canonical, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
  session().cwd = rp.canonical;
  trace_.emit(0, "session.chdir", activeUser_, "-", rp.canonical, "cwd changed", "ok");
  return ok("cwd " + rp.canonical + "\n");
}

CommandResult FileSystemKernel::cmdDir(const std::vector<std::string>& args) {
  const auto path = args.size() >= 2 ? args[1] : ".";
  ResolvedPath rp;
  try { rp = resolve(path, true); } catch (const std::exception& ex) { return err(ErrorCode::NotFound, ex.what()); }
  if (state_.inodes[rp.inode].type != NodeType::Directory) return err(ErrorCode::NotDirectory, "target is not a directory");
  std::string reason;
  if (!canTraverse(rp.canonical, true, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
  if (!authCheck(rp.inode, "r", rp.canonical, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
  return ok(renderDir(rp.inode, rp.canonical));
}

CommandResult FileSystemKernel::cmdCreate(const std::vector<std::string>& args) {
  if (args.size() < 2) return err(ErrorCode::InvalidArgument, usage("create <path> [mode]"));
  ResolvedPath rp;
  try { rp = resolve(args[1], false); } catch (const std::exception& ex) { return err(ErrorCode::NotFound, ex.what()); }
  if (rp.inode != 0) return err(ErrorCode::Exists, "path already exists");
  std::string reason;
  if (!canTraverse(rp.parentPath, true, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
  if (!authCheck(rp.parent, "c", rp.parentPath, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
  auto tx = beginTx("create");
  ensureMutablePath(rp.parentPath, true, tx.id, nullptr);
  rp = resolve(args[1], false);
  const auto id = allocateInode(NodeType::Regular, activeUser_, activeUser_, args.size() >= 3 ? parseMode(args[2], 0644) : 0644);
  auto& parent = state_.inodes[rp.parent];
  parent.entries[rp.leaf] = {rp.leaf, NodeType::Regular, id, state_.inodes[id].generation};
  refreshDirBlock(parent, tx.id);
  trace_.emit(tx.id, "file.create", rp.canonical, "-", std::to_string(id), "create regular file", "ok");
  commitTx(tx);
  return ok(msg("created file ", "已创建文件 ") + rp.canonical + " inode=" + std::to_string(id) + "\n");
}

CommandResult FileSystemKernel::cmdOpen(const std::vector<std::string>& args) {
  if (args.size() < 3) return err(ErrorCode::InvalidArgument, usage("open <path> <r|w|rw|append|truncate>"));
  ResolvedPath rp;
  try { rp = resolve(args[1], true); } catch (const std::exception& ex) { return err(ErrorCode::NotFound, ex.what()); }
  auto& inode = state_.inodes[rp.inode];
  if (inode.type == NodeType::Directory) return err(ErrorCode::IsDirectory, "cannot open directory as file");
  std::string reason;
  if (!canTraverse(rp.canonical, false, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
  if (!authCheck(rp.inode, rightForFlag(args[2]), rp.canonical, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
  if (session().openFiles.size() >= config::kMaxOpenPerUser) return err(ErrorCode::Busy, "user open file table is full");
  if (args[2].find("truncate") != std::string::npos) {
    auto tx = beginTx("open.truncate");
    ensureMutablePath(rp.canonical, true, tx.id, &rp);
    setFileData(state_.inodes[rp.inode], "", tx.id);
    commitTx(tx);
  }
  OpenFile of;
  of.fd = session().nextFd++;
  of.user = activeUser_;
  of.inode = rp.inode;
  of.path = rp.canonical;
  of.flags = args[2];
  of.offset = args[2].find("append") != std::string::npos ? state_.inodes[rp.inode].size : 0;
  session().openFiles[of.fd] = of;
  ++state_.inodes[rp.inode].openCount;
  ++systemOpen_[rp.inode];
  trace_.emit(0, "open.fd", std::to_string(of.fd), "-", std::to_string(rp.inode), "file opened", "ok");
  return ok("fd " + std::to_string(of.fd) + " -> " + rp.canonical + "\n");
}

CommandResult FileSystemKernel::cmdRead(const std::vector<std::string>& args) {
  if (args.size() < 2) return err(ErrorCode::InvalidArgument, usage("read <fd> [size]"));
  const int fd = std::stoi(args[1]);
  auto it = session().openFiles.find(fd);
  if (it == session().openFiles.end()) return err(ErrorCode::InvalidFd, "fd not open");
  if (it->second.flags.find('r') == std::string::npos && it->second.flags.find("rw") == std::string::npos) {
    return err(ErrorCode::PermissionDenied, "fd was not opened for read");
  }
  const auto& inode = state_.inodes[it->second.inode];
  auto data = readFileData(inode);
  const std::size_t size = args.size() >= 3 ? static_cast<std::size_t>(std::stoul(args[2])) : data.size();
  const auto off = static_cast<std::size_t>(it->second.offset);
  const auto chunk = off >= data.size() ? "" : data.substr(off, size);
  it->second.offset += chunk.size();
  trace_.emit(0, "file.read", std::to_string(fd), std::to_string(off), std::to_string(it->second.offset), "read advances fd offset", "ok");
  if (interactiveUi_) {
    return ok(ui::renderReadData(ui::theme(ansiUi_, themeName_, langName_), ui::detectMetrics(), chunk, off, it->second.offset));
  }
  return ok(chunk + "\n");
}

CommandResult FileSystemKernel::cmdWrite(const std::vector<std::string>& args, const std::string& rawCommand) {
  if (args.size() < 3) return err(ErrorCode::InvalidArgument, usage("write <fd> <data>"));
  const int fd = std::stoi(args[1]);
  auto it = session().openFiles.find(fd);
  if (it == session().openFiles.end()) return err(ErrorCode::InvalidFd, "fd not open");
  if (it->second.flags.find('w') == std::string::npos && it->second.flags.find("append") == std::string::npos &&
      it->second.flags.find("truncate") == std::string::npos && it->second.flags.find("rw") == std::string::npos) {
    return err(ErrorCode::PermissionDenied, "fd was not opened for write");
  }
  std::string data;
  const auto fdPos = rawCommand.find(args[1]);
  if (fdPos != std::string::npos) {
    const auto afterFd = rawCommand.find_first_not_of(" \t", fdPos + args[1].size());
    if (afterFd != std::string::npos) data = rawCommand.substr(afterFd);
  }
  if (data.empty()) {
    for (std::size_t i = 2; i < args.size(); ++i) {
      if (i > 2) data += " ";
      data += args[i];
    }
  }
  auto tx = beginTx("write");
  ResolvedPath rp;
  ensureMutablePath(it->second.path, true, tx.id, &rp);
  it = session().openFiles.find(fd);
  it->second.inode = rp.inode;
  auto& inode = state_.inodes[rp.inode];
  auto existing = readFileData(inode);
  if (it->second.flags.find("append") != std::string::npos) it->second.offset = existing.size();
  const auto off = static_cast<std::size_t>(it->second.offset);
  if (existing.size() < off) existing.resize(off, '\0');
  if (existing.size() < off + data.size()) existing.resize(off + data.size());
  existing.replace(off, data.size(), data);
  setFileData(inode, existing, tx.id);
  it->second.offset = off + data.size();
  trace_.emit(tx.id, "file.write", std::to_string(fd), std::to_string(off), std::to_string(it->second.offset), "write advances fd offset", "ok");
  commitTx(tx);
  return ok(msg("wrote ", "已写入 ") + std::to_string(data.size()) + msg(" bytes\n", " 字节\n"));
}

CommandResult FileSystemKernel::cmdClose(const std::vector<std::string>& args) {
  if (args.size() < 2) return err(ErrorCode::InvalidArgument, usage("close <fd>"));
  const int fd = std::stoi(args[1]);
  auto it = session().openFiles.find(fd);
  if (it == session().openFiles.end()) return err(ErrorCode::InvalidFd, "fd not open");
  const auto inodeId = it->second.inode;
  auto inodeIt = state_.inodes.find(inodeId);
  if (inodeIt != state_.inodes.end() && inodeIt->second.openCount > 0) --inodeIt->second.openCount;
  if (systemOpen_[inodeId] > 0) --systemOpen_[inodeId];
  session().openFiles.erase(it);
  if (inodeIt != state_.inodes.end() && inodeIt->second.deletePending && inodeIt->second.openCount == 0) {
    releaseInode(inodeId, true);
  }
  saveState();
  return ok(msg("closed fd ", "已关闭 fd ") + std::to_string(fd) + "\n");
}

CommandResult FileSystemKernel::cmdDelete(const std::vector<std::string>& args) {
  if (args.size() < 2) return err(ErrorCode::InvalidArgument, usage("delete <path>"));
  ResolvedPath rp;
  try { rp = resolve(args[1], true); } catch (const std::exception& ex) { return err(ErrorCode::NotFound, ex.what()); }
  if (state_.inodes[rp.inode].type == NodeType::Directory) return err(ErrorCode::IsDirectory, "use rmdir for directories");
  std::string reason;
  if (!canTraverse(rp.parentPath, true, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
  if (!authCheck(rp.parent, "d", rp.parentPath, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
  auto tx = beginTx("delete");
  ensureMutablePath(rp.parentPath, true, tx.id, nullptr);
  rp = resolve(args[1], true);
  auto& parent = state_.inodes[rp.parent];
  parent.entries.erase(rp.leaf);
  refreshDirBlock(parent, tx.id);
  if (state_.inodes[rp.inode].openCount > 0) state_.inodes[rp.inode].deletePending = true;
  releaseInode(rp.inode, true);
  trace_.emit(tx.id, "file.delete", rp.canonical, std::to_string(rp.inode), "unlinked", "remove dir entry and maybe reclaim", "ok");
  commitTx(tx);
  return ok("deleted " + rp.canonical + "\n");
}

CommandResult FileSystemKernel::cmdTruncate(const std::vector<std::string>& args) {
  if (args.size() < 3) return err(ErrorCode::InvalidArgument, usage("truncate <path|fd> <size>"));
  const auto newSize = static_cast<std::size_t>(std::stoul(args[2]));
  std::uint32_t inodeId = 0;
  std::string path = args[1];
  bool byFd = false;
  int fd = -1;
  if (std::all_of(args[1].begin(), args[1].end(), ::isdigit)) {
    byFd = true;
    fd = std::stoi(args[1]);
    auto it = session().openFiles.find(fd);
    if (it == session().openFiles.end()) return err(ErrorCode::InvalidFd, "fd not open");
    path = it->second.path;
  } else {
    ResolvedPath rp;
    try { rp = resolve(args[1], true); } catch (const std::exception& ex) { return err(ErrorCode::NotFound, ex.what()); }
    std::string reason;
    if (!canTraverse(rp.canonical, false, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
    if (!authCheck(rp.inode, "w", rp.canonical, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
    path = rp.canonical;
  }
  auto tx = beginTx("truncate");
  ResolvedPath rp;
  ensureMutablePath(path, true, tx.id, &rp);
  inodeId = rp.inode;
  if (byFd) session().openFiles[fd].inode = inodeId;
  auto data = readFileData(state_.inodes[inodeId]);
  data.resize(newSize, '\0');
  setFileData(state_.inodes[inodeId], data, tx.id);
  commitTx(tx);
  return ok("truncated " + path + " to " + std::to_string(newSize) + " bytes\n");
}

CommandResult FileSystemKernel::cmdTrace(const std::vector<std::string>& args) {
  if (args.size() < 2) return err(ErrorCode::InvalidArgument, usage("trace on|off|show|save|replay|step|clear"));
  const auto sub = lower(args[1]);
  if (sub == "on") {
    trace_.setEnabled(true);
    trace_.emit(0, "trace.on", "trace", "off", "on", "manual", "ok");
    return ok(msg("trace enabled\n", "trace 已开启\n"));
  }
  if (sub == "off") {
    trace_.emit(0, "trace.off", "trace", "on", "off", "manual", "ok");
    trace_.setEnabled(false);
    return ok(msg("trace disabled\n", "trace 已关闭\n"));
  }
  if (sub == "show") {
    const std::size_t n = args.size() >= 3 ? static_cast<std::size_t>(std::stoul(args[2])) : 0;
    std::ostringstream out;
    const auto events = trace_.recent(n);
    if (interactiveUi_) return ok(ui::renderTraceTimeline(ui::theme(ansiUi_, themeName_, langName_), ui::detectMetrics(), events));
    renderTraceEvents(out, events);
    return ok(out.str());
  }
  if (sub == "save") {
    const auto path = args.size() >= 3 ? args[2] : config::tracePath();
    std::string e;
    if (!trace_.save(path, &e)) return err(ErrorCode::IoError, e);
    return ok("trace saved to " + path + "\n");
  }
  if (sub == "replay") {
    const auto path = args.size() >= 3 ? args[2] : config::tracePath();
    auto events = TraceSink::load(path);
    if (interactiveUi_) {
      return ok(ui::renderTraceTimeline(ui::theme(ansiUi_, themeName_, langName_), ui::detectMetrics(), events, "Trace replay / read-only"));
    }
    std::ostringstream out;
    out << "ScopeFS trace replay (" << events.size() << " events, read-only)\n";
    renderTraceEvents(out, events);
    return ok(out.str());
  }
  if (sub == "step") {
    std::ostringstream out;
    const auto events = trace_.recent(1);
    if (interactiveUi_) return ok(ui::renderTraceTimeline(ui::theme(ansiUi_, themeName_, langName_), ui::detectMetrics(), events, "Trace step"));
    renderTraceEvents(out, events);
    return ok(out.str());
  }
  if (sub == "clear") {
    trace_.clear();
    return ok("trace cleared\n");
  }
  return err(ErrorCode::InvalidArgument, msg("unknown trace subcommand", "未知 trace 子命令"));
}

CommandResult FileSystemKernel::cmdScope(const std::vector<std::string>& args) {
  return ok(renderScope(args.size() >= 2 ? lower(args[1]) : "summary"));
}

CommandResult FileSystemKernel::cmdMap(const std::vector<std::string>& args) {
  auto what = args.size() >= 2 ? lower(args[1]) : "blocks";
  if (what != "blocks" && what != "inode" && what != "journal" && what != "refcount" && what != "owner") {
    return err(ErrorCode::InvalidArgument, usage("map [blocks|inode|journal|refcount|owner]"));
  }
  return ok(renderMap(what));
}

CommandResult FileSystemKernel::cmdSnapshot(const std::vector<std::string>& args) {
  if (args.size() < 2) return err(ErrorCode::InvalidArgument, usage("snapshot create|list|show|diff|rollback|delete"));
  const auto sub = lower(args[1]);
  if (sub == "list") {
    if (interactiveUi_) {
      std::vector<std::string> lines;
      for (const auto& [name, c] : state_.classes) {
        std::string line = name;
        if (!c.parents.empty()) line += " inherits " + joinSet(c.parents, ',');
        line += "  members=" + joinSet(c.members, ',');
        line += c.grantOption ? "  grant-option" : "  no-grant";
        if (!c.constraints.empty()) line += "  " + c.constraints;
        lines.push_back(line);
      }
      return ok(ui::renderClassGraph(ui::theme(ansiUi_, themeName_, langName_), ui::detectMetrics(), lines));
    }
    std::ostringstream out;
    out << "name                 root   gen   txid  created\n";
    for (const auto& [name, s] : state_.snapshots) {
      out << std::left << std::setw(20) << name << " " << std::setw(6) << s.rootInode
          << std::setw(6) << s.generation << std::setw(6) << s.txid << s.createdAt << "\n";
    }
    return ok(out.str());
  }
  if (sub == "create") {
    if (args.size() < 3) return err(ErrorCode::InvalidArgument, usage("snapshot create <name>"));
    if (state_.snapshots.count(args[2])) return err(ErrorCode::Exists, "snapshot already exists");
    auto tx = beginTx("snapshot.create");
    Snapshot s;
    s.name = args[2];
    s.rootInode = state_.super.activeRoot;
    s.generation = state_.inodes[s.rootInode].generation;
    s.txid = tx.id;
    s.createdAt = nowIso();
    retainInode(s.rootInode);
    state_.snapshots[s.name] = s;
    trace_.emit(tx.id, "snapshot.create", s.name, "-", std::to_string(s.rootInode), "O(1) root reference", "ok");
    commitTx(tx);
    return ok("snapshot " + s.name + " root=" + std::to_string(s.rootInode) + "\n");
  }
  if (sub == "show") {
    if (args.size() < 3) return err(ErrorCode::InvalidArgument, usage("snapshot show <name>"));
    auto it = state_.snapshots.find(args[2]);
    if (it == state_.snapshots.end()) return err(ErrorCode::NotFound, "snapshot not found");
    std::ostringstream out;
    out << "snapshot " << it->second.name << "\nroot inode " << it->second.rootInode
        << "\ngeneration " << it->second.generation << "\ntxid " << it->second.txid
        << "\ncreated " << it->second.createdAt << "\n";
    return ok(out.str());
  }
  if (sub == "diff") {
    if (args.size() < 4) return err(ErrorCode::InvalidArgument, usage("snapshot diff <a> <b>"));
    auto a = state_.snapshots.find(args[2]);
    auto b = state_.snapshots.find(args[3]);
    if (a == state_.snapshots.end() || b == state_.snapshots.end()) return err(ErrorCode::NotFound, "snapshot not found");
    const auto fa = flattenTree(a->second.rootInode);
    const auto fb = flattenTree(b->second.rootInode);
    std::ostringstream out;
    std::vector<std::string> diffLines;
    for (const auto& [path, inode] : fb) {
      if (!fa.count(path)) {
        out << "+ " << path << "\n";
        diffLines.push_back("+ " + path);
      }
      else {
        const auto& ia = state_.inodes.at(fa.at(path));
        const auto& ib = state_.inodes.at(inode);
        if (ia.generation != ib.generation || ia.size != ib.size || csvNumbers(ia.blocks) != csvNumbers(ib.blocks)) {
          out << "~ " << path << "\n";
          diffLines.push_back("~ " + path);
        }
      }
    }
    for (const auto& [path, inode] : fa) {
      if (!fb.count(path)) {
        out << "- " << path << "\n";
        diffLines.push_back("- " + path);
      }
      (void)inode;
    }
    if (interactiveUi_) return ok(ui::renderSnapshotDiff(ui::theme(ansiUi_, themeName_, langName_), ui::detectMetrics(), diffLines));
    const auto text = out.str();
    return ok(text.empty() ? "no differences\n" : text);
  }
  if (sub == "rollback") {
    if (args.size() < 3) return err(ErrorCode::InvalidArgument, usage("snapshot rollback <name>"));
    auto it = state_.snapshots.find(args[2]);
    if (it == state_.snapshots.end()) return err(ErrorCode::NotFound, "snapshot not found");
    auto tx = beginTx("snapshot.rollback");
    const auto oldRoot = state_.super.activeRoot;
    retainInode(it->second.rootInode);
    state_.super.activeRoot = it->second.rootInode;
    releaseInode(oldRoot, true);
    trace_.emit(tx.id, "snapshot.rollback", it->first, std::to_string(oldRoot), std::to_string(it->second.rootInode), "atomic active root switch", "ok");
    commitTx(tx);
    return ok("rolled back to " + it->first + "\n");
  }
  if (sub == "delete") {
    if (args.size() < 3) return err(ErrorCode::InvalidArgument, usage("snapshot delete <name>"));
    auto it = state_.snapshots.find(args[2]);
    if (it == state_.snapshots.end()) return err(ErrorCode::NotFound, "snapshot not found");
    auto tx = beginTx("snapshot.delete");
    const auto root = it->second.rootInode;
    state_.snapshots.erase(it);
    releaseInode(root, true);
    trace_.emit(tx.id, "snapshot.delete", args[2], std::to_string(root), "deleted", "drop snapshot reference", "ok");
    commitTx(tx);
    return ok("snapshot deleted\n");
  }
  return err(ErrorCode::InvalidArgument, msg("unknown snapshot subcommand", "未知 snapshot 子命令"));
}

CommandResult FileSystemKernel::cmdClone(const std::vector<std::string>& args) {
  if (args.size() < 3) return err(ErrorCode::InvalidArgument, usage("clone <src> <dst>"));
  ResolvedPath src;
  ResolvedPath dst;
  try {
    src = resolve(args[1], true);
    dst = resolve(args[2], false);
  } catch (const std::exception& ex) {
    return err(ErrorCode::NotFound, ex.what());
  }
  if (dst.inode != 0) return err(ErrorCode::Exists, "destination exists");
  if (state_.inodes[src.inode].type != NodeType::Regular) return err(ErrorCode::InvalidArgument, "source must be regular file");
  std::string reason;
  if (!canTraverse(src.canonical, false, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
  if (!authCheck(src.inode, "r", src.canonical, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
  if (!canTraverse(dst.parentPath, true, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
  if (!authCheck(dst.parent, "c", dst.parentPath, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
  auto tx = beginTx("clone");
  ensureMutablePath(dst.parentPath, true, tx.id, nullptr);
  src = resolve(args[1], true);
  dst = resolve(args[2], false);
  const auto id = cloneInode(src.inode, tx.id);
  auto& clone = state_.inodes[id];
  clone.owner = activeUser_;
  auto& parent = state_.inodes[dst.parent];
  parent.entries[dst.leaf] = {dst.leaf, NodeType::Regular, id, clone.generation};
  refreshDirBlock(parent, tx.id);
  trace_.emit(tx.id, "file.clone", src.canonical, std::to_string(src.inode), std::to_string(id), "shared data blocks", "ok");
  commitTx(tx);
  return ok("cloned " + src.canonical + " -> " + dst.canonical + "\n");
}

CommandResult FileSystemKernel::cmdClass(const std::vector<std::string>& args) {
  if (args.size() < 2) return err(ErrorCode::InvalidArgument, usage("class create|grant|revoke|list|tree"));
  const auto sub = lower(args[1]);
  if (sub == "list") {
    std::ostringstream out;
    out << "class                owner      parents              members              grant constraints\n";
    for (const auto& [name, c] : state_.classes) {
      out << std::left << std::setw(20) << name << " " << std::setw(10) << c.owner << " "
          << std::setw(20) << joinSet(c.parents, ',') << " " << std::setw(20)
          << joinSet(c.members, ',') << " " << std::setw(5) << boolText(c.grantOption)
          << " " << c.constraints << "\n";
    }
    return ok(out.str());
  }
  if (sub == "tree") {
    std::ostringstream out;
    std::vector<std::string> lines;
    for (const auto& [name, c] : state_.classes) {
      out << name;
      if (!c.parents.empty()) out << " inherits " << joinSet(c.parents, ',');
      out << "\n";
      std::string line = name;
      if (!c.parents.empty()) line += " inherits " + joinSet(c.parents, ',');
      lines.push_back(line);
    }
    if (interactiveUi_) return ok(ui::renderClassGraph(ui::theme(ansiUi_, themeName_, langName_), ui::detectMetrics(), lines));
    return ok(out.str());
  }
  if (sub == "create") {
    if (args.size() < 3) return err(ErrorCode::InvalidArgument, usage("class create <class_name>"));
    if (state_.classes.count(args[2])) return err(ErrorCode::Exists, "class exists");
    auto tx = beginTx("class.create");
    ClassDef c;
    c.name = args[2];
    c.owner = activeUser_;
    c.members.insert(activeUser_);
    c.grantOption = true;
    state_.classes[c.name] = c;
    state_.users[activeUser_].classes.insert(c.name);
    trace_.emit(tx.id, "class.create", c.name, "-", activeUser_, "create identity class", "ok");
    commitTx(tx);
    return ok("class created " + c.name + "\n");
  }
  if (sub == "grant") {
    if (args.size() < 5 || lower(args[3]) != "to") return err(ErrorCode::InvalidArgument, usage("class grant <class> to <user_or_class> [with grant option] [constraints]"));
    const auto cls = args[2];
    const auto target = args[4];
    if (!state_.classes.count(cls)) return err(ErrorCode::NotFound, "class not found");
    if (!isRoot() && !state_.classes[cls].grantOption && state_.classes[cls].owner != activeUser_) return err(ErrorCode::PermissionDenied, "grant option missing");
    bool grantOption = false;
    std::string constraints;
    for (std::size_t i = 5; i < args.size(); ++i) {
      if (lower(args[i]) == "with" && i + 2 < args.size() && lower(args[i + 1]) == "grant" && lower(args[i + 2]) == "option") {
        grantOption = true;
        i += 2;
      } else {
        if (!constraints.empty()) constraints += ",";
        constraints += args[i];
      }
    }
    auto tx = beginTx("class.grant");
    if (state_.users.count(target)) {
      state_.users[target].classes.insert(cls);
      state_.classes[cls].members.insert(target);
    } else {
      if (!state_.classes.count(target)) {
        ClassDef c;
        c.name = target;
        c.owner = activeUser_;
        state_.classes[target] = c;
      }
      state_.classes[target].parents.insert(cls);
      state_.classes[cls].members.insert(target);
    }
    if (grantOption) state_.classes[cls].grantOption = true;
    if (!constraints.empty()) state_.classes[cls].constraints = constraints;
    ++state_.classes[cls].generation;
    trace_.emit(tx.id, "class.grant", cls, "-", target, "grant identity class", "ok");
    commitTx(tx);
    return ok("granted " + cls + " to " + target + "\n");
  }
  if (sub == "revoke") {
    if (args.size() < 5 || lower(args[3]) != "from") return err(ErrorCode::InvalidArgument, usage("class revoke <class> from <user_or_class>"));
    const auto cls = args[2];
    const auto target = args[4];
    if (!state_.classes.count(cls)) return err(ErrorCode::NotFound, "class not found");
    auto tx = beginTx("class.revoke");
    if (state_.users.count(target)) state_.users[target].classes.erase(cls);
    if (state_.classes.count(target)) state_.classes[target].parents.erase(cls);
    state_.classes[cls].members.erase(target);
    ++state_.classes[cls].generation;
    trace_.emit(tx.id, "class.revoke", cls, target, "revoked", "generation invalidates downstream grants", "ok");
    commitTx(tx);
    return ok("revoked " + cls + " from " + target + "\n");
  }
  return err(ErrorCode::InvalidArgument, msg("unknown class subcommand", "未知 class 子命令"));
}

CommandResult FileSystemKernel::cmdAcl(const std::vector<std::string>& args) {
  if (args.size() < 3) return err(ErrorCode::InvalidArgument, usage("acl show|grant|revoke ..."));
  const auto sub = lower(args[1]);
  if (sub == "show") {
    ResolvedPath rp;
    try { rp = resolve(args[2], true); } catch (const std::exception& ex) { return err(ErrorCode::NotFound, ex.what()); }
    std::string reason;
    if (!canTraverse(rp.canonical, false, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
    std::ostringstream out;
    out << "ACL for " << rp.canonical << " inode=" << rp.inode << "\n";
    std::vector<std::string> lines;
    lines.push_back(rp.canonical + " inode=#" + std::to_string(rp.inode));
    for (const auto& acl : state_.inodes[rp.inode].acl) {
      out << acl.subject << " rights=" << acl.rights << " constraints=" << acl.constraints
          << " gen=" << acl.generation << "\n";
      lines.push_back(acl.subject + " rights=" + acl.rights + " constraints=" + acl.constraints +
                      " gen=" + std::to_string(acl.generation));
    }
    if (interactiveUi_) return ok(ui::renderAclGraph(ui::theme(ansiUi_, themeName_, langName_), ui::detectMetrics(), "ACL graph", lines));
    return ok(out.str());
  }
  if (sub == "grant") {
    if (args.size() < 5) return err(ErrorCode::InvalidArgument, usage("acl grant <path> <user_or_class> <rights> [constraints]"));
    ResolvedPath rp;
    try { rp = resolve(args[2], true); } catch (const std::exception& ex) { return err(ErrorCode::NotFound, ex.what()); }
    std::string reason;
    if (!canTraverse(rp.canonical, false, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
    if (!authCheck(rp.inode, "g", rp.canonical, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
    auto tx = beginTx("acl.grant");
    ensureMutablePath(rp.canonical, true, tx.id, &rp);
    AclEntry acl;
    acl.subject = args[3];
    acl.rights = rightsNormalize(args[4]);
    for (std::size_t i = 5; i < args.size(); ++i) {
      if (!acl.constraints.empty()) acl.constraints += ",";
      acl.constraints += args[i];
    }
    state_.inodes[rp.inode].acl.push_back(acl);
    trace_.emit(tx.id, "acl.grant", rp.canonical, "-", acl.subject + ":" + acl.rights, "grant ACL rights", "ok");
    commitTx(tx);
    return ok("acl granted\n");
  }
  if (sub == "revoke") {
    if (args.size() < 5) return err(ErrorCode::InvalidArgument, usage("acl revoke <path> <user_or_class> <rights>"));
    ResolvedPath rp;
    try { rp = resolve(args[2], true); } catch (const std::exception& ex) { return err(ErrorCode::NotFound, ex.what()); }
    std::string reason;
    if (!canTraverse(rp.canonical, false, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
    auto tx = beginTx("acl.revoke");
    ensureMutablePath(rp.canonical, true, tx.id, &rp);
    auto& acl = state_.inodes[rp.inode].acl;
    acl.erase(std::remove_if(acl.begin(), acl.end(), [&](const AclEntry& e) {
      return e.subject == args[3] && rightsContain(e.rights, args[4]);
    }), acl.end());
    trace_.emit(tx.id, "acl.revoke", rp.canonical, args[3], args[4], "revoke ACL rights", "ok");
    commitTx(tx);
    return ok("acl revoked\n");
  }
  return err(ErrorCode::InvalidArgument, msg("unknown acl subcommand", "未知 acl 子命令"));
}

CommandResult FileSystemKernel::cmdChmod(const std::vector<std::string>& args) {
  if (args.size() < 3) return err(ErrorCode::InvalidArgument, usage("chmod <path> <mode>"));
  ResolvedPath rp;
  try { rp = resolve(args[1], true); } catch (const std::exception& ex) { return err(ErrorCode::NotFound, ex.what()); }
  std::string reason;
  if (!canTraverse(rp.canonical, false, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
  if (!isRoot() && state_.inodes[rp.inode].owner != activeUser_) return err(ErrorCode::PermissionDenied, "only owner/root can chmod");
  auto tx = beginTx("chmod");
  ensureMutablePath(rp.canonical, true, tx.id, &rp);
  const auto before = state_.inodes[rp.inode].mode;
  state_.inodes[rp.inode].mode = parseMode(args[2], before);
  trace_.emit(tx.id, "inode.chmod", rp.canonical, std::to_string(before), std::to_string(state_.inodes[rp.inode].mode), "mode changed", "ok");
  commitTx(tx);
  return ok("mode updated\n");
}

CommandResult FileSystemKernel::cmdChown(const std::vector<std::string>& args) {
  if (args.size() < 3) return err(ErrorCode::InvalidArgument, usage("chown <path> <user>"));
  if (!isRoot()) return err(ErrorCode::PermissionDenied, "only root can chown");
  if (!state_.users.count(args[2])) return err(ErrorCode::NotFound, "user not found");
  ResolvedPath rp;
  try { rp = resolve(args[1], true); } catch (const std::exception& ex) { return err(ErrorCode::NotFound, ex.what()); }
  std::string reason;
  if (!canTraverse(rp.canonical, false, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
  auto tx = beginTx("chown");
  ensureMutablePath(rp.canonical, true, tx.id, &rp);
  const auto before = state_.inodes[rp.inode].owner;
  state_.inodes[rp.inode].owner = args[2];
  trace_.emit(tx.id, "inode.chown", rp.canonical, before, args[2], "owner changed", "ok");
  commitTx(tx);
  return ok("owner updated\n");
}

CommandResult FileSystemKernel::cmdChclass(const std::vector<std::string>& args) {
  if (args.size() < 3) return err(ErrorCode::InvalidArgument, usage("chclass <path> <class>"));
  if (!state_.classes.count(args[2])) return err(ErrorCode::NotFound, "class not found");
  ResolvedPath rp;
  try { rp = resolve(args[1], true); } catch (const std::exception& ex) { return err(ErrorCode::NotFound, ex.what()); }
  std::string reason;
  if (!canTraverse(rp.canonical, false, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
  if (!isRoot() && state_.inodes[rp.inode].owner != activeUser_) return err(ErrorCode::PermissionDenied, "only owner/root can chclass");
  auto tx = beginTx("chclass");
  ensureMutablePath(rp.canonical, true, tx.id, &rp);
  const auto before = state_.inodes[rp.inode].klass;
  state_.inodes[rp.inode].klass = args[2];
  trace_.emit(tx.id, "inode.chclass", rp.canonical, before, args[2], "file class changed", "ok");
  commitTx(tx);
  return ok("class updated\n");
}

CommandResult FileSystemKernel::cmdFsck(const std::vector<std::string>& args) {
  const bool repair = args.size() >= 2 && args[1] == "--repair";
  return ok(fsck(repair, false));
}

CommandResult FileSystemKernel::cmdCrash(const std::vector<std::string>& args) {
  if (args.size() < 2) return err(ErrorCode::InvalidArgument, usage("crash now|after|before|at|clear"));
  const auto sub = lower(args[1]);
  if (sub == "clear") {
    crashMode_.clear();
    crashEvent_.clear();
    crashSeq_ = 0;
    return ok("crash injection cleared\n");
  }
  if (sub == "now") {
    crashMode_ = "now";
    maybeCrashNow();
  }
  if ((sub == "after" || sub == "before") && args.size() >= 3) {
    crashMode_ = sub;
    crashEvent_ = args[2];
    return ok("crash armed " + sub + " " + crashEvent_ + "\n");
  }
  if (sub == "at" && args.size() >= 3) {
    crashMode_ = "at";
    crashSeq_ = std::stoull(args[2]);
    return ok("crash armed at trace seq " + std::to_string(crashSeq_) + "\n");
  }
  return err(ErrorCode::InvalidArgument, usage("crash now|after <event>|before <event>|at <seq>|clear"));
}

CommandResult FileSystemKernel::cmdTheme(const std::vector<std::string>& args) {
  if (args.size() < 2) {
    return ok("theme " + themeName_ + "\n");
  }
  const auto name = lower(args[1]);
  if (name != "scope-dark" && name != "amber" && name != "blue" && name != "mono") {
    return err(ErrorCode::InvalidArgument, usage("theme scope-dark|amber|blue|mono"));
  }
  themeName_ = name == "amber" ? "scope-dark" : name;
  if (name == "mono") ansiUi_ = false;
  return ok("theme set to " + name + "\n");
}

CommandResult FileSystemKernel::cmdLang(const std::vector<std::string>& args) {
  if (args.size() < 2) {
    return ok("lang " + langName_ + "\n");
  }
  const auto name = lower(args[1]);
  if (name != "zh" && name != "en") {
    return err(ErrorCode::InvalidArgument, langName_ == "zh" ? "用法: lang zh|en" : "usage: lang zh|en");
  }
  langName_ = name;
  return ok(langName_ == "zh" ? "语言已切换为中文\n" : "language switched to English\n");
}

CommandResult FileSystemKernel::cmdHelp() {
  std::ostringstream out;
  if (langName_ == "zh") {
    out << "ScopeFS 命令\n"
        << "  format | login <用户> [密码] | logout | whoami | exit\n"
        << "  mkdir/rmdir/chdir/dir/create/open/read/write/close/delete/rm/truncate\n"
        << "  trace on/off/show [数量]/save <文件>/replay <文件>/step/clear\n"
        << "  scope [inode|block|journal|open|tree] | map [blocks|inode|journal|refcount|owner]\n"
        << "  snapshot create/list/show/diff/rollback/delete | clone <源> <目标>\n"
        << "  class create/grant/revoke/list/tree | chmod/chown/chclass | acl show/grant/revoke\n"
        << "  crash now/after/before/at/clear | fsck [--repair] | theme scope-dark|blue|mono | lang zh|en\n";
  } else {
    out << "ScopeFS commands\n"
        << "  format | login <user> [password] | logout | whoami | exit\n"
        << "  mkdir/rmdir/chdir/dir/create/open/read/write/close/delete/rm/truncate\n"
        << "  trace on/off/show [n]/save <file>/replay <file>/step/clear\n"
        << "  scope [inode|block|journal|open|tree] | map [blocks|inode|journal|refcount|owner]\n"
        << "  snapshot create/list/show/diff/rollback/delete | clone <src> <dst>\n"
        << "  class create/grant/revoke/list/tree | chmod/chown/chclass | acl show/grant/revoke\n"
        << "  crash now/after/before/at/clear | fsck [--repair] | theme scope-dark|blue|mono | lang zh|en\n";
  }
  return ok(out.str());
}

std::string FileSystemKernel::renderDir(std::uint32_t inodeId, const std::string& path) const {
  const auto& dir = state_.inodes.at(inodeId);
  if (interactiveUi_) {
    std::vector<ui::DirRow> rows;
    for (const auto& [name, de] : dir.entries) {
      const auto it = state_.inodes.find(de.inode);
      if (it == state_.inodes.end()) continue;
      const auto& n = it->second;
      ui::DirRow row;
      row.name = name;
      row.type = nodeMarker(n.type);
      row.owner = n.owner;
      row.klass = n.klass;
      row.mode = modeString(n.mode, n.type == NodeType::Directory);
      row.inode = n.id;
      row.generation = n.generation;
      row.size = n.size;
      row.blockCount = n.blocks.size();
      row.refcount = n.refcount;
      row.shared = n.refcount > 1;
      rows.push_back(row);
    }
    return ui::renderDir(ui::theme(ansiUi_, themeName_, langName_), ui::detectMetrics(), path, rows);
  }
  std::ostringstream out;
  out << "╭────────────────────────────────────────────────────────────────────────────────────────╮\n";
  out << "│ name                 type    size    owner      class      mode        inode blocks     │\n";
  out << "├────────────────────────────────────────────────────────────────────────────────────────┤\n";
  for (const auto& [name, de] : dir.entries) {
    const auto it = state_.inodes.find(de.inode);
    if (it == state_.inodes.end()) continue;
    const auto& n = it->second;
    out << "│ " << std::left << std::setw(20) << clip(name, 20)
        << " " << std::setw(7) << nodeMarker(n.type)
        << " " << std::right << std::setw(7) << n.size
        << " " << std::left << std::setw(10) << clip(n.owner, 10)
        << " " << std::setw(10) << clip(n.klass, 10)
        << " " << std::setw(11) << modeString(n.mode, n.type == NodeType::Directory)
        << " " << std::right << std::setw(5) << n.id
        << " " << std::setw(6) << n.blocks.size() << " │\n";
  }
  out << "╰────────────────────────────────────────────────────────────────────────────────────────╯\n";
  return out.str();
}

std::string FileSystemKernel::renderScope(const std::string& what) const {
  std::ostringstream out;
  const auto th = ui::theme(ansiUi_, themeName_, langName_);
  const auto metrics = ui::detectMetrics();
  if (what == "tree") {
    std::set<std::uint32_t> seen;
    const auto tree = renderTree(state_.super.activeRoot, "", "/", seen);
    if (interactiveUi_) {
      std::vector<std::string> lines;
      std::istringstream in(tree);
      std::string line;
      while (std::getline(in, line)) lines.push_back(line);
      return ui::renderTree(th, metrics, lines);
    }
    return tree;
  }
  if (what == "inode") {
    if (interactiveUi_) {
      std::vector<ui::InodeRow> inodeRows;
      for (const auto& [id, n] : state_.inodes) {
        ui::InodeRow row;
        row.inode = id;
        row.type = nodeMarker(n.type);
        row.generation = n.generation;
        row.refcount = n.refcount;
        row.openCount = n.openCount;
        row.owner = n.owner;
        row.klass = n.klass;
        row.size = n.size;
        row.blockCount = n.blocks.size();
        row.pending = n.deletePending;
        inodeRows.push_back(row);
      }
      std::sort(inodeRows.begin(), inodeRows.end(), [](const auto& a, const auto& b) {
        if (a.openCount != b.openCount) return a.openCount > b.openCount;
        if (a.refcount != b.refcount) return a.refcount > b.refcount;
        return a.inode < b.inode;
      });
      if (inodeRows.size() > 10) inodeRows.resize(10);
      return ui::renderScope(th, metrics, status(), inodeRows, {{"view", "inode table"}, {"mode", "hot first"}});
    }
    out << "inode type    gen ref open owner      class      size blocks pending\n";
    for (const auto& [id, n] : state_.inodes) {
      out << std::setw(5) << id << " " << std::left << std::setw(7) << nodeMarker(n.type)
          << std::right << std::setw(4) << n.generation << std::setw(4) << n.refcount
          << std::setw(5) << n.openCount << " " << std::left << std::setw(10) << n.owner
          << std::setw(11) << n.klass << std::right << std::setw(6) << n.size
          << std::setw(7) << n.blocks.size() << " " << boolText(n.deletePending) << "\n";
    }
    return out.str();
  }
  if (what == "block") return renderMap("refcount");
  if (what == "journal") {
    const auto lines = device_.readJournal();
    if (interactiveUi_) {
      std::vector<std::string> body;
      body.push_back("journal lines: " + std::to_string(lines.size()));
      for (const auto& l : lines) body.push_back(clip(l, 110));
      return ui::box(th, "Journal timeline", body, std::min(metrics.columns, metrics.wide ? 132 : 112), "green") + "\n";
    }
    out << "journal lines: " << lines.size() << "\n";
    for (const auto& l : lines) out << clip(l, 120) << "\n";
    return out.str();
  }
  if (what == "open") {
    if (interactiveUi_) {
      std::vector<std::string> body;
      for (const auto& [user, sess] : sessions_) {
        for (const auto& [fd, of] : sess.openFiles) {
          body.push_back(user + " fd=" + std::to_string(fd) + " inode=#" + std::to_string(of.inode) +
                         " offset=" + std::to_string(of.offset) + " " + of.flags + " " + of.path);
        }
      }
      if (body.empty()) body.push_back("no open file descriptors");
      return ui::box(th, "Open file tables", body, std::min(metrics.columns, metrics.wide ? 132 : 112), "blue") + "\n";
    }
    out << "user fd inode offset flags path\n";
    for (const auto& [user, sess] : sessions_) {
      for (const auto& [fd, of] : sess.openFiles) {
        out << user << " " << fd << " " << of.inode << " " << of.offset << " " << of.flags << " " << of.path << "\n";
      }
    }
    out << "system open table\n";
    for (const auto& [inode, count] : systemOpen_) {
      if (count > 0) out << "inode " << inode << " refs " << count << "\n";
    }
    return out.str();
  }
  if (interactiveUi_) {
    std::vector<ui::InodeRow> hot;
    for (const auto& [id, n] : state_.inodes) {
      if (n.refcount > 1 || n.openCount > 0 || n.deletePending) {
        ui::InodeRow row;
        row.inode = id;
        row.type = nodeMarker(n.type);
        row.generation = n.generation;
        row.refcount = n.refcount;
        row.openCount = n.openCount;
        row.owner = n.owner;
        row.klass = n.klass;
        row.size = n.size;
        row.blockCount = n.blocks.size();
        row.pending = n.deletePending;
        hot.push_back(row);
      }
    }
    if (hot.size() > 6) hot.resize(6);
    return ui::renderScope(th, metrics, status(), hot,
                           {{"cwd", activeUser_.empty() ? "/" : session().cwd},
                            {"classes", activeUser_.empty() ? "-" : joinSet(effectiveClasses(activeUser_, session().cwd), ',')},
                            {"trace", trace_.enabled() ? "on" : "off"}});
  }
  out << "╭──────── ScopeFS ────────╮\n";
  out << "│ mount     " << std::setw(12) << toString(state_.super.mountState) << " │\n";
  out << "│ tx next   " << std::setw(12) << state_.super.nextTxid << " │\n";
  out << "│ root      " << std::setw(12) << state_.super.activeRoot << " │\n";
  out << "│ inodes    " << std::setw(5) << state_.inodes.size() << "/" << std::setw(6) << config::kInodeCount << " │\n";
  out << "│ blocks    " << std::setw(5) << state_.blocks.size() << "/" << std::setw(6) << config::kTotalBlocks << " │\n";
  out << "│ snapshots " << std::setw(12) << state_.snapshots.size() << " │\n";
  out << "│ trace     " << std::setw(12) << (trace_.enabled() ? "on" : "off") << " │\n";
  out << "╰─────────────────────────╯\n";
  return out.str();
}

std::string FileSystemKernel::renderTree(std::uint32_t inode, const std::string& prefix, const std::string& name, std::set<std::uint32_t>& seen) const {
  std::ostringstream out;
  auto it = state_.inodes.find(inode);
  if (it == state_.inodes.end()) return "";
  out << prefix << name << " [" << it->second.id << ":" << nodeMarker(it->second.type)
      << " ref=" << it->second.refcount << "]\n";
  if (it->second.type != NodeType::Directory || seen.count(inode)) return out.str();
  seen.insert(inode);
  for (const auto& [entryName, de] : it->second.entries) {
    if (entryName == "." || entryName == "..") continue;
    out << renderTree(de.inode, prefix + "  ", entryName, seen);
  }
  return out.str();
}

std::string FileSystemKernel::renderMap(const std::string& what) const {
  if (interactiveUi_) {
    std::vector<ui::MapCell> cells;
    cells.reserve(state_.blocks.size());
    for (const auto& [id, block] : state_.blocks) {
      ui::MapCell cell;
      cell.block = id;
      cell.refcount = block.refcount;
      cell.ownerInode = block.ownerInode;
      cell.lastWriterTxid = block.lastWriterTxid;
      cell.flags = block.flags;
      cells.push_back(cell);
    }
    return ui::renderMap(ui::theme(ansiUi_, themeName_, langName_), ui::detectMetrics(), what, cells, config::kTotalBlocks);
  }
  std::ostringstream out;
  out << (langName_ == "zh" ? "ScopeFS 磁盘图: " : "ScopeFS disk map: ") << what << "\n";
  const std::uint32_t width = 96;
  const std::uint32_t rows = 24;
  const std::uint32_t total = width * rows;
  const std::uint32_t stride = std::max<std::uint32_t>(1, config::kTotalBlocks / total);
  {
    for (std::uint32_t r = 0; r < rows; ++r) {
      for (std::uint32_t c = 0; c < width; ++c) {
        const auto b = (r * width + c) * stride;
        auto it = state_.blocks.find(b);
        const BlockInfo* info = it == state_.blocks.end() ? nullptr : &it->second;
        out << plainMapGlyph(what, b, info);
      }
      out << "\n";
    }
    out << (langName_ == "zh" ? plainMapLegendZh(what) : plainMapLegend(what)) << "\n";
    return out.str();
  }
}

std::string FileSystemKernel::fsck(bool repair, bool light) {
  std::ostringstream out;
  int issues = 0;
  const auto expected = checksumFields({
      std::to_string(state_.super.version),
      std::to_string(state_.super.blockSize),
      std::to_string(state_.super.totalBlocks),
      std::to_string(state_.super.inodeCount),
      std::to_string(state_.super.freeBlocks),
      std::to_string(state_.super.freeInodes),
      std::to_string(state_.super.activeRoot),
      std::to_string(state_.super.nextInode),
      std::to_string(state_.super.nextBlock),
      std::to_string(state_.super.nextTxid),
      toString(state_.super.mountState)});
  if (expected != state_.super.checksum) {
    ++issues;
    out << "BAD superblock checksum expected=" << expected << " got=" << state_.super.checksum << "\n";
    if (repair) state_.super.checksum = expected;
  }
  if (!state_.inodes.count(state_.super.activeRoot)) {
    ++issues;
    out << "BAD active root missing\n";
  }
  std::map<std::uint32_t, std::uint32_t> blockRefs;
  for (const auto& [id, inode] : state_.inodes) {
    for (auto b : inode.blocks) ++blockRefs[b];
    if (inode.type == NodeType::Directory) {
      for (const auto& [name, de] : inode.entries) {
        if (!state_.inodes.count(de.inode)) {
          ++issues;
          out << "BAD dir inode=" << id << " entry=" << name << " missing target=" << de.inode << "\n";
        }
      }
    }
  }
  for (const auto& [b, count] : blockRefs) {
    auto it = state_.blocks.find(b);
    if (it == state_.blocks.end()) {
      ++issues;
      out << "BAD inode references missing block " << b << "\n";
    } else if (it->second.refcount != count) {
      ++issues;
      out << "BAD block " << b << " refcount expected=" << count << " got=" << it->second.refcount << "\n";
      if (repair) it->second.refcount = count;
    }
  }
  if (repair) {
    state_.freeBlocks.clear();
    for (std::uint32_t b = config::kDataStart; b < config::kTotalBlocks; ++b) {
      if (!state_.blocks.count(b) || state_.blocks[b].refcount == 0) state_.freeBlocks.insert(b);
    }
    state_.freeInodes.clear();
    for (std::uint32_t i = 1; i <= config::kInodeCount; ++i) {
      if (!state_.inodes.count(i)) state_.freeInodes.insert(i);
    }
    recomputeSuper();
    saveState();
  }
  trace_.emit(0, light ? "fsck.light" : "fsck.full", "volume", "-", std::to_string(issues), repair ? "repair" : "check", issues ? "issues" : "ok");
  if (issues == 0) out << "fsck: clean\n";
  else out << "fsck: " << issues << " issue(s)" << (repair ? " repaired where possible" : "") << "\n";
  return out.str();
}

std::map<std::string, std::uint32_t> FileSystemKernel::flattenTree(std::uint32_t root) const {
  std::map<std::string, std::uint32_t> out;
  std::function<void(std::uint32_t, std::string)> walk = [&](std::uint32_t inodeId, std::string path) {
    auto it = state_.inodes.find(inodeId);
    if (it == state_.inodes.end()) return;
    out[path] = inodeId;
    if (it->second.type != NodeType::Directory) return;
    for (const auto& [name, de] : it->second.entries) {
      if (name == "." || name == "..") continue;
      walk(de.inode, pathJoin(path, name));
    }
  };
  walk(root, "/");
  return out;
}

std::string errorCodeName(ErrorCode code) {
  switch (code) {
    case ErrorCode::Ok: return "OK";
    case ErrorCode::NeedLogin: return "E_NEED_LOGIN";
    case ErrorCode::AlreadyLoggedIn: return "E_ALREADY_LOGIN";
    case ErrorCode::AuthFailed: return "E_AUTH";
    case ErrorCode::NotFound: return "E_NOT_FOUND";
    case ErrorCode::Exists: return "E_EXISTS";
    case ErrorCode::NotDirectory: return "E_NOT_DIR";
    case ErrorCode::IsDirectory: return "E_IS_DIR";
    case ErrorCode::DirectoryNotEmpty: return "E_DIR_NOT_EMPTY";
    case ErrorCode::PermissionDenied: return "E_PERMISSION";
    case ErrorCode::InvalidCommand: return "E_COMMAND";
    case ErrorCode::InvalidArgument: return "E_ARGUMENT";
    case ErrorCode::InvalidFd: return "E_FD";
    case ErrorCode::NoSpace: return "E_NOSPC";
    case ErrorCode::Busy: return "E_BUSY";
    case ErrorCode::CrashInjected: return "E_CRASH";
    case ErrorCode::IoError: return "E_IO";
  }
  return "E_UNKNOWN";
}

std::ostream& operator<<(std::ostream& out, const CommandResult& result) {
  if (!result.output.empty()) out << result.output;
  if (result.code != ErrorCode::Ok) out << errorCodeName(result.code) << ": " << result.message << "\n";
  return out;
}

} // namespace scopefs
