#include "scopefs/kernel.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <queue>
#include <sstream>
#include <thread>

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
    case NodeType::GroupObject: return "group";
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

std::string displayTimestamp(const std::string& timestamp) {
  if (timestamp.empty()) return "-";
  std::string out = timestamp;
  const auto t = out.find('T');
  if (t != std::string::npos) out[t] = ' ';
  if (out.size() >= 16) out.resize(16);
  return out;
}

std::string plusOneVisibleMinute(const std::string& timestamp) {
  std::tm tm{};
  std::istringstream in(timestamp);
  in >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
  if (in.fail()) return nowIso();
  const std::time_t value = std::mktime(&tm);
  if (value == static_cast<std::time_t>(-1)) return nowIso();
  const std::time_t bumped = value + 60;
  std::tm outTm{};
#if defined(_WIN32)
  localtime_s(&outTm, &bumped);
#else
  localtime_r(&bumped, &outTm);
#endif
  std::ostringstream out;
  out << std::put_time(&outTm, "%Y-%m-%dT%H:%M:%S");
  return out.str();
}

std::string nextModifiedTimestamp(const std::string& previous) {
  const auto current = nowIso();
  if (previous.empty() || displayTimestamp(current) > displayTimestamp(previous)) return current;
  return plusOneVisibleMinute(previous);
}

std::string plainTypeLabel(NodeType type, const std::string& lang) {
  const bool zh = lang == "zh";
  switch (type) {
    case NodeType::Regular: return zh ? "文件" : "file";
    case NodeType::Directory: return zh ? "目录" : "directory";
    case NodeType::SnapshotRoot: return zh ? "快照根" : "snapshot root";
    case NodeType::GroupObject: return zh ? "用户组对象" : "group object";
  }
  return zh ? "未知" : "unknown";
}

std::string rightForFlag(const std::string& flags) {
  if (flags.find('w') != std::string::npos || flags.find("append") != std::string::npos ||
      flags.find("truncate") != std::string::npos) {
    return "w";
  }
  return "r";
}

bool isUnsignedNumber(const std::string& text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char ch) {
    return std::isdigit(ch) != 0;
  });
}

std::string commandTraceType(const std::string& cmd, const std::vector<std::string>& args) {
  if (cmd == "login") return "session.login";
  if (cmd == "chdir" || cmd == "cd") return "session.chdir";
  if (cmd == "mkdir") return "dir.mkdir";
  if (cmd == "rmdir") return "dir.rmdir";
  if (cmd == "create") return "file.create";
  if (cmd == "open") return "open.fd";
  if (cmd == "read") return "file.read";
  if (cmd == "write") return "file.write";
  if (cmd == "delete" || cmd == "rm") return "file.delete";
  if (cmd == "cp") return "file.clone";
  if (cmd == "chmod") return "inode.chmod";
  if (cmd == "chown") return "inode.chown";
  if (cmd == "chgroup") return "inode.chgroup";
  if (cmd == "fsck") return "fsck.full";
  if (cmd == "snapshot" && args.size() >= 2) {
    const auto sub = lower(args[1]);
    if (sub == "create") return "snapshot.create";
    if (sub == "rollback") return "snapshot.rollback";
    if (sub == "delete") return "snapshot.delete";
  }
  if (cmd == "group" && args.size() >= 2) {
    const auto sub = lower(args[1]);
    if (sub == "create") return "group.create";
    if (sub == "delete") return "group.delete";
    if (sub == "grant") return "group.grant";
    if (sub == "revoke") return "group.revoke";
  }
  if (cmd == "acl" && args.size() >= 2) {
    const auto sub = lower(args[1]);
    if (sub == "grant") return "acl.grant";
    if (sub == "revoke") return "acl.revoke";
  }
  return "";
}

std::string commandTraceObject(const std::string& cmd, const std::vector<std::string>& args) {
  if (cmd == "cp" && args.size() >= 3) return args[1] + " -> " + args[2];
  if (cmd == "group" && args.size() >= 3) return args[2];
  if (cmd == "snapshot" && args.size() >= 3) return args[2];
  if (cmd == "acl" && args.size() >= 3) return args[2];
  if (args.size() >= 2) return args[1];
  return cmd;
}

std::string blockTraceObject(const FsState& state, const BlockInfo& block, const std::string& ownerHint) {
  auto owner = ownerHint.empty() ? std::string("block") : ownerHint;
  const auto inodeIt = state.inodes.find(block.ownerInode);
  if (inodeIt != state.inodes.end()) owner = nodeMarker(inodeIt->second.type);
  return owner + ":" + std::to_string(block.id);
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
  return "legend: . free 1 reference count 1 2 reference count 2 # reference count >2";
}

std::string plainMapLegendZh(const std::string& what) {
  if (what == "blocks") return "图例: S super R refmeta I inode J journal P snapshot U users D data . 空闲";
  if (what == "inode") return "图例: I inode-table 0-f owner-inode 数据块 * 共享 . 无关/空闲";
  if (what == "journal") return "图例: J journal 保留区 0-f last-writer tx S super . 无 journal 信号";
  if (what == "owner") return "图例: 0-f owner inode modulo 16 . 无 owner";
  return "图例: . 空闲 1 引用数 1 2 引用数 2 # 引用数 >2";
}

} // namespace

std::string toString(NodeType type) {
  switch (type) {
    case NodeType::Regular: return "regular";
    case NodeType::Directory: return "directory";
    case NodeType::SnapshotRoot: return "snapshot_root";
    case NodeType::GroupObject: return "group_object";
  }
  return "regular";
}

NodeType nodeTypeFromString(const std::string& value) {
  if (value == "directory") return NodeType::Directory;
  if (value == "snapshot_root") return NodeType::SnapshotRoot;
  if (value == "group_object" || value == "class_object") return NodeType::GroupObject;
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

FileSystemKernel::FileSystemKernel() : device_(trace_), coordinator_(trace_) {}

TraceSink& FileSystemKernel::trace() { return trace_; }

bool FileSystemKernel::isMounted() const { return mounted_; }

void FileSystemKernel::checkExternalCrashSignal() {
  std::string reason;
  if (!coordinator_.hasPendingCrashSignal(&reason)) return;
  coordinator_.markAbnormalExit();
  trace_.emit(0, "system.crash.detected", "terminal", "-", reason, "external crash signal received", "crash");
  throw CrashException("signal:" + reason);
}

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
  if (!mounted_ || activeUser_.empty()) return "$ ";
  return session().cwd + " " + activeUser_ + " $ ";
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
      {"<user_or_group>", "<用户或用户组>"},
      {"<group_name>", "<用户组名>"},
      {"[password]", "[密码]"},
      {"[mode]", "[模式]"},
      {"[size]", "[大小]"},
      {"[constraints]", "[约束]"},
      {"<password>", "<密码>"},
      {"<path|fd>", "<路径|fd>"},
      {"<constraints>", "<约束>"},
      {"<rights>", "<权限>"},
      {"<event>", "<事件>"},
      {"<group>", "<用户组>"},
      {"<file>", "<文件>"},
      {"<user>", "<用户>"},
      {"<path>", "<路径>"},
      {"<mode>", "<模式>"},
      {"<data>", "<数据>"},
      {"<size>", "<大小>"},
      {"<name>", "<名称>"},
      {"<src>", "<源>"},
      {"<dst>", "<目标>"},
      {"<ms>", "<毫秒>"},
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
      "help", "exit", "format", "login", "trace", "theme", "lang", "sleep"};
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
  coordinator_.startSession();
  device_.ensureWorkspace();
  if (!device_.volumeExists()) {
    trace_.emit(0, "mount.missing", "volume", "-", device_.volumePath(), "volume not found", "need_format");
    mounted_ = false;
    return;
  }
  deserializeState(device_.readAll());
  mounted_ = true;
  recoverIfNeeded();
  {
    auto txLock = coordinator_.acquireTxLock("mount.open");
    reloadVolumeFromDisk("mount.open");
    setMountState(MountState::Dirty);
    saveState();
    coordinator_.bumpEpoch();
  }
  trace_.emit(0, "mount.open", "volume", "clean", "dirty", "normal mount", "ok");
}

void FileSystemKernel::unmountClean() {
  if (!mounted_) return;
  for (auto& [user, sess] : sessions_) {
    for (auto& [fd, of] : sess.openFiles) {
      auto it = state_.inodes.find(of.inode);
      if (it != state_.inodes.end() && it->second.openCount > 0) --it->second.openCount;
      if (systemOpen_[of.inode] > 0) --systemOpen_[of.inode];
      if (it != state_.inodes.end() && it->second.deletePending && it->second.openCount == 0) {
        releaseInode(of.inode, true);
      }
    }
    sess.openFiles.clear();
  }
  if (!coordinator_.hasOtherActiveSessions() && coordinator_.listLocks(false).empty()) {
    auto txLock = coordinator_.acquireTxLock("mount.close");
    reloadVolumeFromDisk("mount.close");
    setMountState(MountState::Clean);
    saveState();
    device_.clearJournal();
    coordinator_.bumpEpoch();
  }
  trace_.save(config::tracePath(), nullptr);
  trace_.emit(0, "mount.close", "volume", "dirty", "clean", "normal unmount", "ok");
  coordinator_.shutdownClean();
}

CommandResult FileSystemKernel::execute(const std::vector<std::string>& args, const std::string& rawCommand) {
  if (args.empty()) return ok();
  const auto cmd = lower(args[0]);
  trace_.setContext(currentUser(), rawCommand);
  const auto commandType = commandTraceType(cmd, args);
  auto commandSpan = commandType.empty()
      ? TraceSink::Span()
      : trace_.span(0, commandType, commandTraceObject(cmd, args), "-", "-", "command entered", "start");
  try {
    coordinator_.heartbeat(currentUser(), rawCommand);
    checkExternalCrashSignal();
    VolumeCoordinator::ScopedLock txLock;
    VolumeCoordinator::ScopedLock readLock;
    if (mounted_) crashPoint("command.dispatch", "before");
    const bool mutation = isMutationCommand(cmd, args);
    if (mutation) {
      txLock = coordinator_.acquireTxLock(cmd);
      if (mounted_ && device_.volumeExists()) reloadVolumeFromDisk("pre-mutation " + cmd);
    } else if (mounted_ && cmd != "trace" && cmd != "sleep" && cmd != "help" && cmd != "exit" &&
               cmd != "theme" && cmd != "lang") {
      readLock = coordinator_.acquireReadLock(cmd);
      reloadIfEpochChanged();
    } else if (mounted_ && cmd != "trace") {
      reloadIfEpochChanged();
    }
    if (!mounted_ && cmd != "format" && cmd != "help" && cmd != "exit" && cmd != "trace" && cmd != "lang" && cmd != "sleep") {
      auto result = err(ErrorCode::InvalidCommand, msg("volume is not mounted; run format first", "卷尚未挂载；请先运行 format"));
      commandSpan.finish(commandTraceObject(cmd, args), "-", result.message, "command returned error", "error");
      return result;
    }
    if (requiresLogin(cmd)) {
      CommandResult result;
      if (!ensureLogged(&result)) {
        commandSpan.finish(commandTraceObject(cmd, args), "-", result.message, "command returned error", "error");
        return result;
      }
    }
    CommandResult result;
    if (cmd == "format") result = cmdFormat();
    else if (cmd == "login") result = cmdLogin(args);
    else if (cmd == "logout") result = cmdLogout();
    else if (cmd == "whoami") result = cmdWhoami();
    else if (cmd == "mkdir") result = cmdMkdir(args);
    else if (cmd == "rmdir") result = cmdRmdir(args);
    else if (cmd == "chdir" || cmd == "cd") result = cmdChdir(args);
    else if (cmd == "dir" || cmd == "ls") result = cmdDir(args);
    else if (cmd == "create") result = cmdCreate(args);
    else if (cmd == "open") result = cmdOpen(args);
    else if (cmd == "read") result = cmdRead(args);
    else if (cmd == "write") result = cmdWrite(args, rawCommand);
    else if (cmd == "close") result = cmdClose(args);
    else if (cmd == "delete" || cmd == "rm") result = cmdDelete(args);
    else if (cmd == "truncate") result = cmdTruncate(args);
    else if (cmd == "trace") result = cmdTrace(args, rawCommand);
    else if (cmd == "scope") result = cmdScope(args);
    else if (cmd == "map") result = cmdMap(args);
    else if (cmd == "snapshot") result = cmdSnapshot(args);
    else if (cmd == "cp") result = cmdCopy(args);
    else if (cmd == "group") result = cmdGroup(args);
    else if (cmd == "acl") result = cmdAcl(args);
    else if (cmd == "chmod") result = cmdChmod(args);
    else if (cmd == "chown") result = cmdChown(args);
    else if (cmd == "chgroup") result = cmdChgroup(args);
    else if (cmd == "fsck") result = cmdFsck(args);
    else if (cmd == "crash") result = cmdCrash(args);
    else if (cmd == "sleep") result = cmdSleep(args);
    else if (cmd == "theme") result = cmdTheme(args);
    else if (cmd == "lang") result = cmdLang(args);
    else if (cmd == "help") result = cmdHelp();
    else result = err(ErrorCode::InvalidCommand, msg("unknown command: ", "未知命令: ") + args[0]);
    if (result.code != ErrorCode::Ok) {
      commandSpan.finish(commandTraceObject(cmd, args), "-", result.message, "command returned error", "error");
    }
    return result;
  } catch (const CrashException&) {
    throw;
  } catch (const std::runtime_error& ex) {
    const std::string message = ex.what();
    commandSpan.finish(commandTraceObject(cmd, args), "-", message, "runtime exception", "error");
    trace_.emit(0, "command.error", cmd, "-", message, "runtime exception", "error");
    if (startsWith(message, "E_LOCK_BUSY:")) return err(ErrorCode::LockBusy, message.substr(std::string("E_LOCK_BUSY: ").size()));
    return err(ErrorCode::IoError, message);
  } catch (const std::exception& ex) {
    commandSpan.finish(commandTraceObject(cmd, args), "-", ex.what(), "uncaught exception", "error");
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
    u.groups.insert(name);
    state_.users[name] = u;
    GroupDef c;
    c.name = name;
    c.owner = "root";
    c.members.insert(name);
    c.grantOption = false;
    state_.groups[name] = c;
  };
  makeUser("root", "root", "/root");
  makeUser("admin", "admin", "/root");
  for (int i = 1; i <= 8; ++i) {
    const auto u = "usr" + std::to_string(i);
    makeUser(u, u, "/home/" + u);
  }

  auto addGroup = [&](const std::string& name,
                      std::set<std::string> parents = {},
                      bool grantOption = false) {
    GroupDef g;
    g.name = name;
    g.owner = "root";
    g.parents = std::move(parents);
    g.grantOption = grantOption;
    state_.groups[name] = g;
  };
  auto addMember = [&](const std::string& group, const std::string& user) {
    state_.users[user].groups.insert(group);
    state_.groups[group].members.insert(user);
  };

  addGroup("system", {}, true);
  addGroup("teacher");
  addGroup("assistant");
  addGroup("student");
  addGroup("cs101");
  addGroup("cs102");
  addGroup("cs101_teacher", {"teacher", "cs101"});
  addGroup("cs101_assistant", {"assistant", "cs101"});
  addGroup("cs101_student", {"student", "cs101"});
  addGroup("cs102_teacher", {"teacher", "cs102"});
  addGroup("cs102_assistant", {"assistant", "cs102"});
  addGroup("cs102_student", {"student", "cs102"});

  addMember("system", "root");
  addMember("system", "admin");
  addMember("teacher", "usr1");
  addMember("teacher", "usr2");
  addMember("assistant", "usr3");
  addMember("assistant", "usr4");
  for (const auto& user : {"usr5", "usr6", "usr7", "usr8"}) addMember("student", user);
  for (const auto& user : {"usr1", "usr3", "usr5", "usr6"}) addMember("cs101", user);
  for (const auto& user : {"usr2", "usr4", "usr7", "usr8"}) addMember("cs102", user);
  addMember("cs101_teacher", "usr1");
  addMember("cs101_assistant", "usr3");
  addMember("cs101_student", "usr5");
  addMember("cs101_student", "usr6");
  addMember("cs102_teacher", "usr2");
  addMember("cs102_assistant", "usr4");
  addMember("cs102_student", "usr7");
  addMember("cs102_student", "usr8");

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
    refreshDirBlock(node, 0, false);
  }
  refreshDirBlock(homeNode, 0, false);
  refreshDirBlock(rh, 0, false);
  refreshDirBlock(rootNode, 0, false);
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
        << encode(inode.owner) << '|' << encode(inode.group) << '|' << inode.mode << '|'
        << inode.size << '|' << inode.nlink << '|' << inode.openCount << '|' << inode.refcount << '|'
        << yn(inode.deletePending) << '|' << csvNumbers(inode.blocks) << '|'
        << encode(inode.createdAt) << '|' << encode(inode.modifiedAt) << '\n';
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
        << '|' << encode(joinSet(user.groups, ',')) << '\n';
  }
  for (const auto& [name, cls] : state_.groups) {
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
      inode.group = decode(f[5]);
      inode.mode = std::stoi(f[6]);
      inode.size = std::stoull(f[7]);
      inode.nlink = static_cast<std::uint32_t>(std::stoul(f[8]));
      inode.openCount = static_cast<std::uint32_t>(std::stoul(f[9]));
      inode.refcount = static_cast<std::uint32_t>(std::stoul(f[10]));
      inode.deletePending = parseBool(f[11]);
      inode.blocks = parseCsvNumbers<std::uint32_t>(f[12]);
      if (f.size() >= 15) {
        inode.createdAt = decode(f[13]);
        inode.modifiedAt = decode(f[14]);
      }
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
      u.groups = splitSet(decode(f[4]), ',');
      state_.users[u.name] = u;
    } else if (f[0] == "C" && f.size() >= 8) {
      GroupDef c;
      c.name = decode(f[1]);
      c.owner = decode(f[2]);
      c.parents = splitSet(decode(f[3]), ',');
      c.members = splitSet(decode(f[4]), ',');
      c.grantOption = parseBool(f[5]);
      c.constraints = decode(f[6]);
      c.generation = static_cast<std::uint32_t>(std::stoul(f[7]));
      state_.groups[c.name] = c;
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
  auto txLock = coordinator_.acquireTxLock("recovery");
  reloadVolumeFromDisk("recovery begin");
  const auto lockedJournal = device_.readJournal();
  if (state_.super.mountState == MountState::Clean && lockedJournal.empty()) return;
  trace_.emit(0, "recovery.begin", "journal", toString(state_.super.mountState), std::to_string(journal.size()), "dirty mount or journal present", "start");
  setMountState(MountState::Recovering);
  recoveryFromJournal(lockedJournal);
  const auto report = fsck(false, true);
  trace_.emit(0, "fsck.light", "volume", "-", report, "automatic recovery check", "ok");
  device_.clearJournal();
  setMountState(MountState::Dirty);
  saveState();
  coordinator_.bumpEpoch();
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

void FileSystemKernel::reloadVolumeFromDisk(const std::string& reason) {
  if (!device_.volumeExists()) return;
  const auto before = state_.super.nextTxid;
  deserializeState(device_.readAll());
  rebuildOpenTables();
  mounted_ = true;
  trace_.emit(0, "coord.epoch.reload", "volume", std::to_string(before), std::to_string(state_.super.nextTxid), reason, "ok");
}

void FileSystemKernel::rebuildOpenTables() {
  systemOpen_.clear();
  for (auto& [id, inode] : state_.inodes) {
    inode.openCount = 0;
    (void)id;
  }
  for (const auto& [user, sess] : sessions_) {
    for (const auto& [fd, of] : sess.openFiles) {
      auto it = state_.inodes.find(of.inode);
      if (it == state_.inodes.end()) continue;
      ++it->second.openCount;
      ++systemOpen_[of.inode];
      (void)fd;
    }
    (void)user;
  }
}

bool FileSystemKernel::reloadIfEpochChanged() {
  std::uint64_t before = 0;
  std::uint64_t after = 0;
  if (!coordinator_.observeEpochChange(&before, &after)) return false;
  reloadVolumeFromDisk("epoch " + std::to_string(before) + " -> " + std::to_string(after));
  return true;
}

bool FileSystemKernel::isMutationCommand(const std::string& cmd, const std::vector<std::string>& args) const {
  static const std::set<std::string> always = {
      "format", "mkdir", "rmdir", "create", "write", "close", "delete", "rm", "truncate",
      "cp", "chmod", "chown", "chgroup", "logout"};
  if (always.count(cmd)) return true;
  if (cmd == "open" && args.size() >= 3 && lower(args[2]).find("truncate") != std::string::npos) return true;
  if (cmd == "snapshot" && args.size() >= 2) {
    const auto sub = lower(args[1]);
    return sub == "create" || sub == "rollback" || sub == "delete";
  }
  if (cmd == "group" && args.size() >= 2) {
    const auto sub = lower(args[1]);
    return sub == "create" || sub == "delete" || sub == "grant" || sub == "revoke";
  }
  if (cmd == "acl" && args.size() >= 2) {
    const auto sub = lower(args[1]);
    return sub == "grant" || sub == "revoke";
  }
  if (cmd == "fsck" && args.size() >= 2 && args[1] == "--repair") return true;
  return false;
}

FileSystemKernel::Tx FileSystemKernel::beginTx(const std::string& name) {
  Tx tx;
  tx.span = trace_.span(0, "journal.begin", name, "-", "-", "transaction begin", "start");
  tx.id = state_.super.nextTxid++;
  tx.span.setTxid(tx.id);
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
  coordinator_.bumpEpoch();
  crashPoint("journal.checkpoint", "after");
  tx.span.finish(tx.name, "-", "home_blocks", "transaction complete", "ok");
  tx.active = false;
}

void FileSystemKernel::journalLine(const std::string& line) {
  device_.appendJournal(line);
}

void FileSystemKernel::crashPoint(const std::string& event, const std::string& phase) {
  if (crashMode_ == "before" && crashEvent_ == event && phase == "before") maybeCrashNow();
  if (crashMode_ == "after" && crashEvent_ == event && phase == "after") maybeCrashNow();
  if (crashMode_ == "at" && trace_.nextSeq() >= crashSeq_) maybeCrashNow();
}

void FileSystemKernel::maybeCrashNow() {
  coordinator_.broadcastCrash(crashMode_.empty() ? "now" : crashMode_ + ":" + crashEvent_);
  coordinator_.markAbnormalExit();
  throw CrashException(crashMode_.empty() ? "now" : crashMode_ + ":" + crashEvent_);
}

std::uint32_t FileSystemKernel::allocateInode(NodeType type, const std::string& owner, const std::string& group, int mode) {
  if (state_.freeInodes.empty()) throw std::runtime_error("no free inode");
  const auto id = *state_.freeInodes.begin();
  state_.freeInodes.erase(id);
  Inode inode;
  inode.id = id;
  inode.type = type;
  inode.owner = owner;
  inode.group = group;
  inode.mode = mode;
  inode.generation = 1;
  inode.refcount = 1;
  inode.createdAt = nowIso();
  inode.modifiedAt = inode.createdAt;
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
  for (auto b : it->second.blocks) releaseBlock(b, nodeMarker(it->second.type));
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

void FileSystemKernel::releaseBlock(std::uint32_t block, const std::string& ownerHint) {
  auto it = state_.blocks.find(block);
  if (it == state_.blocks.end()) return;
  const auto object = blockTraceObject(state_, it->second, ownerHint);
  bool fileBlock = ownerHint == "file";
  if (!fileBlock) {
    const auto inodeIt = state_.inodes.find(it->second.ownerInode);
    fileBlock = inodeIt != state_.inodes.end() && inodeIt->second.type == NodeType::Regular;
  }
  if (it->second.refcount > 0) --it->second.refcount;
  trace_.emit(0, "block.release", object, "-", std::to_string(it->second.refcount), "drop block ref", "ok");
  if (it->second.refcount == 0 && block >= config::kDataStart && fileBlock) {
    trace_.emit(0, "block.free", object, "ref=0", "free", "last block reference dropped", "ok");
  }
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

void FileSystemKernel::setFileData(Inode& inode, const std::string& data, std::uint64_t txid, bool touchModified) {
  for (auto b : inode.blocks) {
    const auto it = state_.blocks.find(b);
    if (it != state_.blocks.end() && it->second.refcount > 1) {
      trace_.emit(txid, "cow.break", std::to_string(b), std::to_string(it->second.refcount), "new block", "write shared block", "ok");
    }
  }
  const auto oldBlocks = inode.blocks;
  inode.blocks.clear();
  for (auto b : oldBlocks) releaseBlock(b, nodeMarker(inode.type));
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
  if (touchModified) inode.modifiedAt = nextModifiedTimestamp(inode.modifiedAt);
  trace_.emit(txid, "inode.write_map", std::to_string(inode.id), csvNumbers(oldBlocks), csvNumbers(inode.blocks), "file block map updated", "ok");
}

void FileSystemKernel::refreshDirBlock(Inode& inode, std::uint64_t txid, bool touchModified) {
  std::ostringstream data;
  for (const auto& [name, de] : inode.entries) {
    data << name << ':' << de.inode << ':' << toString(de.type) << '\n';
  }
  setFileData(inode, data.str(), txid, touchModified);
}

FileSystemKernel::ResolvedPath FileSystemKernel::resolve(const std::string& path, bool mustExist) {
  ResolvedPath result;
  result.canonical = canonicalize(activeUser_.empty() ? "/" : session().cwd, path);
  const auto parts = pathParts(result.canonical);
  std::uint32_t current = state_.super.activeRoot;
  std::string currentPath = "/";
  std::vector<TraceSink::Span> pathSpans;
  pathSpans.push_back(trace_.span(0, "path.lookup", "/", "-", "-", "enter path lookup", "start"));
  pathSpans.back().update("/", "-", std::to_string(current), "start path lookup", "ok");
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
    pathSpans.push_back(trace_.span(0, "path.lookup", currentPath, "-", "-", "enter component " + parts[i], "start"));
    pathSpans.back().update(currentPath, "-", std::to_string(current), "resolve component " + parts[i], "ok");
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
    const auto newRoot = cloneInode(oldRoot, txid, true);
    releaseInode(oldRoot, false);
    state_.super.activeRoot = newRoot;
    auto& nr = state_.inodes[newRoot];
    nr.entries["."] = {".", NodeType::Directory, newRoot, nr.generation};
    nr.entries[".."] = {"..", NodeType::Directory, newRoot, nr.generation};
    refreshDirBlock(nr, txid, false);
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
      const auto replacement = cloneInode(child, txid, true);
      releaseInode(child, false);
      entryIt->second.inode = replacement;
      entryIt->second.generation = state_.inodes[replacement].generation;
      current = replacement;
      refreshDirBlock(parent, txid, false);
      trace_.emit(txid, "cow.path_node", parts[i], std::to_string(child), std::to_string(replacement), "path component was shared", "ok");
    } else {
      current = child;
    }
  }
  if (updated) *updated = resolve(canonical, includeLeaf);
  return true;
}

std::uint32_t FileSystemKernel::cloneInode(std::uint32_t src, std::uint64_t txid, bool preserveTimes) {
  const auto old = state_.inodes.at(src);
  const auto id = allocateInode(old.type, old.owner, old.group, old.mode);
  auto& clone = state_.inodes[id];
  clone.generation = old.generation + 1;
  clone.size = old.size;
  clone.nlink = old.nlink;
  clone.acl = old.acl;
  clone.entries = old.entries;
  clone.blocks = old.blocks;
  clone.refcount = 1;
  if (preserveTimes) {
    clone.createdAt = old.createdAt;
    clone.modifiedAt = old.modifiedAt;
  }
  for (auto b : clone.blocks) retainBlock(b);
  if (clone.type == NodeType::Directory) {
    for (const auto& [name, de] : clone.entries) {
      if (name == "." || name == "..") continue;
      retainInode(de.inode);
    }
    clone.entries["."] = {".", NodeType::Directory, id, clone.generation};
  }
  trace_.emit(txid, "inode.clone", std::to_string(src), "-", std::to_string(id), "COW inode copy", "ok");
  return id;
}

bool FileSystemKernel::authCheck(std::uint32_t inodeId, const std::string& right, const std::string& path, std::string* reason) {
  auto authSpan = trace_.span(0, "auth.check", path, "-", right, "enter permission check", "start");
  if (isRoot()) {
    if (reason) *reason = "root bypass";
    authSpan.finish(path, "-", right, "root bypass", "allow");
    return true;
  }
  const auto inodeIt = state_.inodes.find(inodeId);
  if (inodeIt == state_.inodes.end()) {
    if (reason) *reason = "target inode missing";
    authSpan.finish(path, "-", right, "target inode missing", "deny");
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
    authSpan.finish(path, "-", right, "owner mode", "allow");
    return true;
  }
  const auto groups = effectiveGroups(activeUser_, path);
  const auto course = courseForGroup(inode.group);
  if (!course.empty() && groups.count(course + "_teacher") != 0) {
    if (reason) *reason = "course teacher policy allows via " + course;
    authSpan.finish(path, "-", right, "course teacher policy", "allow");
    return true;
  }
  if (!course.empty() && groups.count(course + "_assistant") != 0 &&
      (right == "r" || right == "w" || right == "x" || right == "c" || right == "d")) {
    if (reason) *reason = "course assistant policy allows via " + course;
    authSpan.finish(path, "-", right, "course assistant policy", "allow");
    return true;
  }
  if (groups.count(inode.group) && modeAllows(3)) {
    if (reason) *reason = "file group mode allows via " + inode.group;
    authSpan.finish(path, "-", right, "group mode", "allow");
    return true;
  }
  if (modeAllows(0)) {
    if (reason) *reason = "other mode allows";
    authSpan.finish(path, "-", right, "other mode", "allow");
    return true;
  }
  for (const auto& acl : inode.acl) {
    const bool subjectMatch = acl.subject == activeUser_ || groups.count(acl.subject) != 0;
    if (subjectMatch && rightsContain(acl.rights, right) && constraintsAllow(acl.constraints, path)) {
      if (reason) *reason = "ACL allows " + acl.subject;
      authSpan.finish(path, "-", right, "ACL " + acl.subject, "allow");
      return true;
    }
  }
  if (reason) *reason = "default deny after owner/group/ACL checks";
  authSpan.finish(path, "-", right, "default deny", "deny");
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

std::set<std::string> FileSystemKernel::effectiveGroups(const std::string& user, const std::string& path) const {
  std::set<std::string> out;
  auto userIt = state_.users.find(user);
  if (userIt == state_.users.end()) return out;
  std::queue<std::string> q;
  for (const auto& c : userIt->second.groups) q.push(c);
  while (!q.empty()) {
    const auto c = q.front();
    q.pop();
    if (out.count(c)) continue;
    auto clsIt = state_.groups.find(c);
    if (clsIt != state_.groups.end() && !constraintsAllow(clsIt->second.constraints, path)) continue;
    out.insert(c);
    if (clsIt != state_.groups.end()) {
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

std::string FileSystemKernel::teachingRole(const std::string& user) const {
  const auto it = state_.users.find(user);
  if (it == state_.users.end()) return "-";
  if (it->second.groups.count("system")) return "system";
  if (it->second.groups.count("teacher")) return "teacher";
  if (it->second.groups.count("assistant")) return "assistant";
  if (it->second.groups.count("student")) return "student";
  return "-";
}

std::set<std::string> FileSystemKernel::teachingCourses(const std::string& user) const {
  std::set<std::string> out;
  const auto it = state_.users.find(user);
  if (it == state_.users.end()) return out;
  for (const auto& group : it->second.groups) {
    if (group == "cs101" || group == "cs102") out.insert(group);
  }
  return out;
}

bool FileSystemKernel::isBuiltInGroup(const std::string& group) const {
  static const std::set<std::string> builtIns = {
      "system", "teacher", "assistant", "student",
      "cs101", "cs102",
      "cs101_teacher", "cs101_assistant", "cs101_student",
      "cs102_teacher", "cs102_assistant", "cs102_student",
      "root", "admin", "usr1", "usr2", "usr3", "usr4", "usr5", "usr6", "usr7", "usr8"};
  return builtIns.count(group) != 0;
}

std::string FileSystemKernel::courseForGroup(const std::string& group) const {
  if (group == "cs101" || startsWith(group, "cs101_")) return "cs101";
  if (group == "cs102" || startsWith(group, "cs102_")) return "cs102";
  return "";
}

bool FileSystemKernel::isCourseTeacher(const std::string& user, const std::string& course) const {
  if (course.empty()) return false;
  return effectiveGroups(user, "/").count(course + "_teacher") != 0;
}

bool FileSystemKernel::isCourseAssistant(const std::string& user, const std::string& course) const {
  if (course.empty()) return false;
  return effectiveGroups(user, "/").count(course + "_assistant") != 0;
}

bool FileSystemKernel::canGrantAclSubject(std::uint32_t inodeId, const std::string& subject) const {
  if (isRoot()) return true;
  const auto inodeIt = state_.inodes.find(inodeId);
  if (inodeIt == state_.inodes.end()) return false;
  const auto course = courseForGroup(inodeIt->second.group);
  if (course.empty()) return true;

  if (!isCourseTeacher(activeUser_, course) && !isCourseAssistant(activeUser_, course)) return false;
  const auto groupCourse = courseForGroup(subject);
  if (groupCourse == course) return true;
  const auto userIt = state_.users.find(subject);
  if (userIt == state_.users.end()) return false;
  return teachingCourses(subject).count(course) != 0;
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
    if (it != state_.inodes.end() && it->second.deletePending && it->second.openCount == 0) {
      releaseInode(of.inode, true);
    }
    trace_.emit(0, "open.close", std::to_string(fd), "-", std::to_string(of.inode), "logout closes fd", "ok");
  }
  s.openFiles.clear();
  const auto old = activeUser_;
  activeUser_.clear();
  saveState();
  coordinator_.bumpEpoch();
  return ok(msg("logged out ", "已登出: ") + old + "\n");
}

CommandResult FileSystemKernel::cmdWhoami() {
  std::ostringstream out;
  out << "user: " << activeUser_ << "\n";
  out << "role: " << teachingRole(activeUser_) << "\n";
  out << "courses: " << joinSet(teachingCourses(activeUser_), ',') << "\n";
  out << "cwd : " << session().cwd << "\n";
  out << "groups: " << joinSet(effectiveGroups(activeUser_, session().cwd), ',') << "\n";
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
  refreshDirBlock(node, tx.id, false);
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
  const auto id = allocateInode(NodeType::Regular, activeUser_, activeUser_, args.size() >= 3 ? parseMode(args[2], 0640) : 0640);
  auto& parent = state_.inodes[rp.parent];
  parent.entries[rp.leaf] = {rp.leaf, NodeType::Regular, id, state_.inodes[id].generation};
  refreshDirBlock(parent, tx.id);
  trace_.emit(tx.id, "file.create", rp.canonical, "-", std::to_string(id), "create regular file", "ok");
  commitTx(tx);
  return ok(msg("created file ", "已创建文件 ") + rp.canonical + " inode=" + std::to_string(id) + "\n");
}

CommandResult FileSystemKernel::cmdOpen(const std::vector<std::string>& args) {
  if (args.size() != 3) return err(ErrorCode::InvalidArgument, usage("open <path> <r|w|rw|append|truncate>"));
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
  coordinator_.bumpEpoch();
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
  if (state_.inodes[rp.inode].openCount > 0) {
    state_.inodes[rp.inode].deletePending = true;
    trace_.emit(tx.id, "inode.delete_pending", rp.canonical, std::to_string(rp.inode), "open", "open file delays reclaim", "ok");
  } else {
    releaseInode(rp.inode, true);
  }
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
  if (byFd) {
    session().openFiles[fd].inode = inodeId;
  }
  auto data = readFileData(state_.inodes[inodeId]);
  data.resize(newSize, '\0');
  setFileData(state_.inodes[inodeId], data, tx.id);
  commitTx(tx);
  return ok("truncated " + path + " to " + std::to_string(newSize) + " bytes\n");
}

CommandResult FileSystemKernel::cmdTrace(const std::vector<std::string>& args, const std::string& rawCommand) {
  if (args.size() < 2 || isUnsignedNumber(args[1])) {
    const std::size_t n = args.size() >= 2 ? static_cast<std::size_t>(std::stoul(args[1])) : 0;
    std::ostringstream out;
    const auto events = trace_.recent(n);
    if (interactiveUi_) return ok(ui::renderTraceTimeline(ui::theme(ansiUi_, themeName_, langName_), ui::detectMetrics(), events));
    renderTraceEvents(out, events);
    return ok(out.str());
  }

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

  const auto tracePos = rawCommand.find("trace");
  const auto commandStart = rawCommand.find_first_not_of(" \t", tracePos == std::string::npos ? 5 : tracePos + 5);
  if (commandStart == std::string::npos) return err(ErrorCode::InvalidArgument, usage("trace [n]|trace <command>|trace on|off|save|replay|step|clear"));
  const auto innerCommand = trim(rawCommand.substr(commandStart));
  auto innerArgs = tokenize(innerCommand);
  if (innerArgs.empty()) return err(ErrorCode::InvalidArgument, usage("trace <command>"));
  if (lower(innerArgs[0]) == "trace") return err(ErrorCode::InvalidArgument, msg("trace cannot wrap trace", "trace 不能包装 trace"));

  const auto firstSeq = trace_.nextSeq();
  const bool oldInteractive = interactiveUi_;
  interactiveUi_ = false;
  CommandResult result;
  try {
    result = execute(innerArgs, innerCommand);
  } catch (...) {
    interactiveUi_ = oldInteractive;
    trace_.setContext(currentUser(), rawCommand);
    throw;
  }
  interactiveUi_ = oldInteractive;
  trace_.setContext(currentUser(), rawCommand);
  auto events = trace_.sinceForCommand(firstSeq, innerCommand);
  if (events.empty()) events = trace_.since(firstSeq);

  std::ostringstream out;
  if (interactiveUi_) {
    out << ui::renderTraceTimeline(ui::theme(ansiUi_, themeName_, langName_), ui::detectMetrics(), events, innerCommand);
  } else {
    out << (langName_ == "zh" ? "ScopeFS trace: " : "ScopeFS trace: ") << innerCommand << "\n";
    renderTraceEvents(out, events);
  }
  result.output = out.str();
  return result;
}

CommandResult FileSystemKernel::cmdScope(const std::vector<std::string>& args) {
  const auto what = args.size() >= 2 ? lower(args[1]) : "summary";
  static const std::set<std::string> allowed = {"summary", "inode", "block", "journal", "open", "tree"};
  if (!allowed.count(what)) return err(ErrorCode::InvalidArgument, usage("scope [inode|block|journal|open|tree]"));
  return ok(renderScope(what));
}

CommandResult FileSystemKernel::cmdSleep(const std::vector<std::string>& args) {
  if (args.size() < 2) return err(ErrorCode::InvalidArgument, usage("sleep <ms>"));
  std::uint64_t totalMs = 0;
  try {
    totalMs = std::stoull(args[1]);
  } catch (const std::exception&) {
    return err(ErrorCode::InvalidArgument, usage("sleep <ms>"));
  }
  constexpr std::uint64_t kStepMs = 100;
  std::uint64_t elapsed = 0;
  while (elapsed < totalMs) {
    const auto step = std::min(kStepMs, totalMs - elapsed);
    std::this_thread::sleep_for(std::chrono::milliseconds(step));
    elapsed += step;
    coordinator_.heartbeat(currentUser(), "sleep");
    checkExternalCrashSignal();
  }
  return ok("slept " + std::to_string(totalMs) + " ms\n");
}

CommandResult FileSystemKernel::cmdMap(const std::vector<std::string>& args) {
  auto what = args.size() >= 2 ? lower(args[1]) : "blocks";
  if (what != "blocks" && what != "inode" && what != "journal" && what != "refcount" && what != "owner") {
    return err(ErrorCode::InvalidArgument, usage("map [blocks|inode|journal|refcount|owner]"));
  }
  return ok(renderMap(what));
}

CommandResult FileSystemKernel::cmdSnapshot(const std::vector<std::string>& args) {
  auto renderSnapshots = [&]() {
    std::vector<ui::SnapshotRow> rows;
    for (const auto& [name, s] : state_.snapshots) {
      ui::SnapshotRow row;
      row.name = name;
      row.rootInode = s.rootInode;
      row.generation = s.generation;
      row.txid = s.txid;
      row.createdAt = displayTimestamp(s.createdAt);
      rows.push_back(row);
    }
    if (interactiveUi_) {
      return ok(ui::renderSnapshotList(ui::theme(ansiUi_, themeName_, langName_), ui::detectMetrics(), rows));
    }
    std::ostringstream out;
    out << "name                 root index node  version  transaction id  created\n";
    for (const auto& row : rows) {
      out << std::left << std::setw(20) << row.name << " " << std::right << std::setw(15) << row.rootInode
          << std::setw(9) << row.generation << std::setw(16) << row.txid << "  " << row.createdAt << "\n";
    }
    return ok(out.str());
  };
  if (args.size() < 2) {
    return renderSnapshots();
  }
  const auto sub = lower(args[1]);
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

CommandResult FileSystemKernel::cmdCopy(const std::vector<std::string>& args) {
  if (args.size() < 3) return err(ErrorCode::InvalidArgument, usage("cp <src> <dst>"));
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
  auto tx = beginTx("cp");
  ensureMutablePath(dst.parentPath, true, tx.id, nullptr);
  src = resolve(args[1], true);
  dst = resolve(args[2], false);
  const auto id = cloneInode(src.inode, tx.id, false);
  auto& clone = state_.inodes[id];
  clone.owner = activeUser_;
  auto& parent = state_.inodes[dst.parent];
  parent.entries[dst.leaf] = {dst.leaf, clone.type, id, clone.generation};
  refreshDirBlock(parent, tx.id);
  trace_.emit(tx.id, "file.clone", src.canonical, std::to_string(src.inode), std::to_string(id), "shared data blocks", "ok");
  commitTx(tx);
  return ok(msg("copied ", "已复制 ") + src.canonical + " -> " + dst.canonical + "\n");
}

CommandResult FileSystemKernel::cmdGroup(const std::vector<std::string>& args) {
  if (args.size() < 2) return err(ErrorCode::InvalidArgument, usage("group create|delete|grant|revoke|list|tree"));
  const auto sub = lower(args[1]);
  if (sub == "list") {
    std::ostringstream out;
    out << "group                owner      parents              members              grant constraints\n";
    for (const auto& [name, c] : state_.groups) {
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
    out << "ScopeFS group tree\n";
    std::set<std::string> emittedRoots;
    auto describeMember = [&](const std::string& member) {
      auto userIt = state_.users.find(member);
      if (userIt == state_.users.end()) return "group " + member;
      const auto courses = teachingCourses(member);
      return "member " + member + " role=" + teachingRole(member) + " courses=" +
             (courses.empty() ? "-" : joinSet(courses, ','));
    };
    auto childGroups = [&](const std::string& parent) {
      std::vector<std::string> children;
      for (const auto& [name, group] : state_.groups) {
        if (group.parents.count(parent)) children.push_back(name);
      }
      std::sort(children.begin(), children.end());
      return children;
    };
    std::function<void(const std::string&, const std::string&, const std::string&, std::set<std::string>)> emitGroup =
        [&](const std::string& name, const std::string& headerPrefix, const std::string& childPrefix, std::set<std::string> path) {
          auto it = state_.groups.find(name);
          if (it == state_.groups.end()) return;
          const auto& group = it->second;
          std::string header = "group " + name + " owner=" + group.owner + " grant=" + boolText(group.grantOption);
          if (!group.parents.empty()) header += " parents=" + joinSet(group.parents, ',');
          if (!group.constraints.empty()) header += " constraints=" + group.constraints;
          if (headerPrefix.empty()) {
            out << "● " << header << "\n";
            lines.push_back("● " + header);
          } else {
            out << headerPrefix << header << "\n";
            lines.push_back(headerPrefix + header);
          }
          if (path.count(name)) return;
          path.insert(name);

          std::vector<std::string> branches;
          for (const auto& child : childGroups(name)) branches.push_back("group:" + child);
          for (const auto& member : group.members) branches.push_back("member:" + member);
          std::sort(branches.begin(), branches.end());
          for (std::size_t i = 0; i < branches.size(); ++i) {
            const bool last = i + 1 == branches.size();
            const auto stem = last ? "└─ " : "├─ ";
            const auto branchPrefix = childPrefix + stem;
            const auto nextPrefix = childPrefix + (last ? "   " : "│  ");
            const auto value = branches[i].substr(branches[i].find(':') + 1);
            if (startsWith(branches[i], "group:")) {
              emitGroup(value, branchPrefix, nextPrefix, path);
            } else {
              const auto line = branchPrefix + describeMember(value);
              out << line << "\n";
              lines.push_back(line);
            }
          }
        };

    const std::vector<std::string> preferredRoots = {"system", "teacher", "assistant", "student", "cs101", "cs102"};
    for (const auto& root : preferredRoots) {
      if (state_.groups.count(root)) {
        emitGroup(root, "", "", {});
        emittedRoots.insert(root);
      }
    }
    for (const auto& [name, group] : state_.groups) {
      if (emittedRoots.count(name) || !group.parents.empty()) continue;
      emitGroup(name, "", "", {});
    }
    if (interactiveUi_) return ok(ui::renderGroupGraph(ui::theme(ansiUi_, themeName_, langName_), ui::detectMetrics(), lines));
    return ok(out.str());
  }
  if (sub == "create") {
    if (args.size() < 3) return err(ErrorCode::InvalidArgument, usage("group create <group_name>"));
    if (state_.groups.count(args[2])) return err(ErrorCode::Exists, "group exists");
    auto tx = beginTx("group.create");
    GroupDef c;
    c.name = args[2];
    c.owner = activeUser_;
    c.members.insert(activeUser_);
    c.grantOption = true;
    state_.groups[c.name] = c;
    state_.users[activeUser_].groups.insert(c.name);
    trace_.emit(tx.id, "group.create", c.name, "-", activeUser_, "create user group", "ok");
    commitTx(tx);
    return ok("group created " + c.name + "\n");
  }
  if (sub == "delete") {
    if (args.size() < 3) return err(ErrorCode::InvalidArgument, usage("group delete <group_name>"));
    const auto groupName = args[2];
    auto it = state_.groups.find(groupName);
    if (it == state_.groups.end()) return err(ErrorCode::NotFound, "group not found");
    if (!isRoot() && (isBuiltInGroup(groupName) || it->second.owner != activeUser_)) {
      return err(ErrorCode::PermissionDenied, "only owner/root can delete this group");
    }
    auto tx = beginTx("group.delete");
    for (auto& [userName, user] : state_.users) {
      user.groups.erase(groupName);
      (void)userName;
    }
    for (auto& [name, group] : state_.groups) {
      group.parents.erase(groupName);
      group.members.erase(groupName);
      (void)name;
    }
    state_.groups.erase(groupName);
    trace_.emit(tx.id, "group.delete", groupName, "-", "deleted", "delete user group", "ok");
    commitTx(tx);
    return ok("group deleted " + groupName + "\n");
  }
  if (sub == "grant") {
    if (args.size() < 5 || lower(args[3]) != "to") return err(ErrorCode::InvalidArgument, usage("group grant <group> to <user_or_group> [with grant option] [constraints]"));
    const auto cls = args[2];
    const auto target = args[4];
    if (!state_.groups.count(cls)) return err(ErrorCode::NotFound, "group not found");
    if (!state_.users.count(target) && !state_.groups.count(target)) return err(ErrorCode::NotFound, "user/group not found");
    if (!isRoot() && isBuiltInGroup(cls)) return err(ErrorCode::PermissionDenied, "only root/admin can modify built-in groups");
    if (!isRoot() && !state_.groups[cls].grantOption && state_.groups[cls].owner != activeUser_) return err(ErrorCode::PermissionDenied, "grant option missing");
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
    auto tx = beginTx("group.grant");
    if (state_.users.count(target)) {
      state_.users[target].groups.insert(cls);
      state_.groups[cls].members.insert(target);
    } else {
      state_.groups[target].parents.insert(cls);
      state_.groups[cls].members.insert(target);
    }
    if (grantOption) state_.groups[cls].grantOption = true;
    if (!constraints.empty()) state_.groups[cls].constraints = constraints;
    ++state_.groups[cls].generation;
    trace_.emit(tx.id, "group.grant", cls, "-", target, "grant user group", "ok");
    commitTx(tx);
    return ok("granted " + cls + " to " + target + "\n");
  }
  if (sub == "revoke") {
    if (args.size() < 5 || lower(args[3]) != "from") return err(ErrorCode::InvalidArgument, usage("group revoke <group> from <user_or_group>"));
    const auto cls = args[2];
    const auto target = args[4];
    if (!state_.groups.count(cls)) return err(ErrorCode::NotFound, "group not found");
    if (!isRoot() && isBuiltInGroup(cls)) return err(ErrorCode::PermissionDenied, "only root/admin can modify built-in groups");
    if (!isRoot() && state_.groups[cls].owner != activeUser_ && !state_.groups[cls].grantOption) return err(ErrorCode::PermissionDenied, "grant option missing");
    auto tx = beginTx("group.revoke");
    if (state_.users.count(target)) state_.users[target].groups.erase(cls);
    if (state_.groups.count(target)) state_.groups[target].parents.erase(cls);
    state_.groups[cls].members.erase(target);
    ++state_.groups[cls].generation;
    trace_.emit(tx.id, "group.revoke", cls, target, "revoked", "generation invalidates downstream grants", "ok");
    commitTx(tx);
    return ok("revoked " + cls + " from " + target + "\n");
  }
  return err(ErrorCode::InvalidArgument, msg("unknown group subcommand", "未知 group 子命令"));
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
    if (args.size() < 5) return err(ErrorCode::InvalidArgument, usage("acl grant <path> <user_or_group> <rights> [constraints]"));
    if (!state_.users.count(args[3]) && !state_.groups.count(args[3])) return err(ErrorCode::NotFound, "user/group not found");
    ResolvedPath rp;
    try { rp = resolve(args[2], true); } catch (const std::exception& ex) { return err(ErrorCode::NotFound, ex.what()); }
    std::string reason;
    if (!canTraverse(rp.canonical, false, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
    if (!authCheck(rp.inode, "g", rp.canonical, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
    if (!canGrantAclSubject(rp.inode, args[3])) return err(ErrorCode::PermissionDenied, "cannot grant ACL outside this course boundary");
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
    if (args.size() < 5) return err(ErrorCode::InvalidArgument, usage("acl revoke <path> <user_or_group> <rights>"));
    ResolvedPath rp;
    try { rp = resolve(args[2], true); } catch (const std::exception& ex) { return err(ErrorCode::NotFound, ex.what()); }
    std::string reason;
    if (!canTraverse(rp.canonical, false, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
    if (!authCheck(rp.inode, "g", rp.canonical, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
    if (!canGrantAclSubject(rp.inode, args[3])) return err(ErrorCode::PermissionDenied, "cannot revoke ACL outside this course boundary");
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
  if (!isRoot()) return err(ErrorCode::PermissionDenied, "only root/admin can chown");
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

CommandResult FileSystemKernel::cmdChgroup(const std::vector<std::string>& args) {
  if (args.size() < 3) return err(ErrorCode::InvalidArgument, usage("chgroup <path> <group>"));
  if (!state_.groups.count(args[2])) return err(ErrorCode::NotFound, "group not found");
  ResolvedPath rp;
  try { rp = resolve(args[1], true); } catch (const std::exception& ex) { return err(ErrorCode::NotFound, ex.what()); }
  std::string reason;
  if (!canTraverse(rp.canonical, false, &reason)) return err(ErrorCode::PermissionDenied, "access denied: " + reason);
  if (!isRoot() && state_.inodes[rp.inode].owner != activeUser_) return err(ErrorCode::PermissionDenied, "only owner/root can chgroup");
  auto tx = beginTx("chgroup");
  ensureMutablePath(rp.canonical, true, tx.id, &rp);
  const auto before = state_.inodes[rp.inode].group;
  state_.inodes[rp.inode].group = args[2];
  trace_.emit(tx.id, "inode.chgroup", rp.canonical, before, args[2], "file group changed", "ok");
  commitTx(tx);
  return ok("group updated\n");
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
        << "  trace [数量] | trace <命令> | trace on/off/save <文件>/replay <文件>/step/clear\n"
        << "  scope [inode|block|journal|open|tree] | map [blocks|inode|journal|refcount|owner]\n"
        << "  snapshot | snapshot create/diff/rollback/delete | cp <源> <目标>\n"
        << "  group create/delete/grant/revoke/list/tree | chmod/chown/chgroup | acl show/grant/revoke\n"
        << "  crash now/after/before/at/clear | sleep <毫秒> | fsck [--repair] | theme scope-dark|blue|mono | lang zh|en\n";
  } else {
    out << "ScopeFS commands\n"
        << "  format | login <user> [password] | logout | whoami | exit\n"
        << "  mkdir/rmdir/chdir/dir/create/open/read/write/close/delete/rm/truncate\n"
        << "  trace [n] | trace <command> | trace on/off/save <file>/replay <file>/step/clear\n"
        << "  scope [inode|block|journal|open|tree] | map [blocks|inode|journal|refcount|owner]\n"
        << "  snapshot | snapshot create/diff/rollback/delete | cp <src> <dst>\n"
        << "  group create/delete/grant/revoke/list/tree | chmod/chown/chgroup | acl show/grant/revoke\n"
        << "  crash now/after/before/at/clear | sleep <ms> | fsck [--repair] | theme scope-dark|blue|mono | lang zh|en\n";
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
      row.group = n.group;
      row.mode = modeString(n.mode, n.type == NodeType::Directory);
      row.inode = n.id;
      row.generation = n.generation;
      row.size = n.size;
      row.blockCount = n.blocks.size();
      row.refcount = n.refcount;
      row.shared = n.refcount > 1;
      row.createdAt = displayTimestamp(n.createdAt);
      row.modifiedAt = displayTimestamp(n.modifiedAt);
      rows.push_back(row);
    }
    return ui::renderDir(ui::theme(ansiUi_, themeName_, langName_), ui::detectMetrics(), path, rows);
  }
  std::ostringstream out;
  const bool zh = langName_ == "zh";
  out << (zh ? "目录 " : "directory ") << path << " (" << dir.entries.size() << " "
      << (zh ? "项" : "entries") << ")\n";
  if (zh) {
    out << "名称\t类型\t大小\t属主\t用户组\t模式\t索引节点\t版本号\t引用数\t创建\t修改\n";
  } else {
    out << "name\ttype\tsize\towner\tgroup\tmode\tindex node\tversion\treference count\tcreated\tmodified\n";
  }
  for (const auto& [name, de] : dir.entries) {
    const auto it = state_.inodes.find(de.inode);
    if (it == state_.inodes.end()) continue;
    const auto& n = it->second;
    out << name
        << "\t" << plainTypeLabel(n.type, langName_)
        << "\t" << (zh ? "大小 " : "size ") << n.size << "B"
        << "\t" << n.owner
        << "\t" << n.group
        << "\t" << modeString(n.mode, n.type == NodeType::Directory)
        << "\t" << (zh ? "索引节点 " : "index node ") << n.id
        << "\t" << (zh ? "版本号 " : "version ") << n.generation
        << "\t" << (zh ? "引用数 " : "reference count ") << n.refcount
        << "\t" << (zh ? "创建 " : "created ") << displayTimestamp(n.createdAt)
        << "\t" << (zh ? "修改 " : "modified ") << displayTimestamp(n.modifiedAt)
        << "\n";
  }
  return out.str();
}

std::string FileSystemKernel::renderScope(const std::string& what) const {
  std::ostringstream out;
  const auto th = ui::theme(ansiUi_, themeName_, langName_);
  const auto metrics = ui::detectMetrics();
  if (what == "tree") {
    std::set<std::uint32_t> seen;
    const auto tree = renderTree(state_.super.activeRoot, "", "/", true, seen);
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
        row.group = n.group;
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
    out << "index node type      version  reference count  open count  owner      group      size data blocks pending delete\n";
    for (const auto& [id, n] : state_.inodes) {
      out << std::right << std::setw(10) << id << " " << std::left << std::setw(9) << plainTypeLabel(n.type, langName_)
          << std::right << std::setw(8) << n.generation << std::setw(17) << n.refcount
          << std::setw(12) << n.openCount << " " << std::left << std::setw(10) << n.owner
          << std::setw(11) << n.group << std::right << std::setw(6) << (std::to_string(n.size) + "B")
          << std::setw(12) << n.blocks.size() << " " << boolText(n.deletePending) << "\n";
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
          body.push_back(user + " file descriptor " + std::to_string(fd) + " index node #" + std::to_string(of.inode) +
                         " offset " + std::to_string(of.offset) + " " + of.flags + " " + of.path);
        }
      }
      if (body.empty()) body.push_back("no open file descriptors");
      return ui::box(th, "Open file tables", body, std::min(metrics.columns, metrics.wide ? 132 : 112), "blue") + "\n";
    }
    out << "user file descriptor index node offset flags path\n";
    for (const auto& [user, sess] : sessions_) {
      for (const auto& [fd, of] : sess.openFiles) {
        out << user << " " << fd << " " << of.inode << " " << of.offset << " " << of.flags << " " << of.path << "\n";
      }
    }
    out << "system open file table\n";
    for (const auto& [inode, count] : systemOpen_) {
      if (count > 0) out << "index node " << inode << " open count " << count << "\n";
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
        row.group = n.group;
        row.size = n.size;
        row.blockCount = n.blocks.size();
        row.pending = n.deletePending;
        hot.push_back(row);
      }
    }
    if (hot.size() > 6) hot.resize(6);
    return ui::renderScope(th, metrics, status(), hot,
                           {{"cwd", activeUser_.empty() ? "/" : session().cwd},
                            {"groups", activeUser_.empty() ? "-" : joinSet(effectiveGroups(activeUser_, session().cwd), ',')},
                            {"trace", trace_.enabled() ? "on" : "off"}});
  }
  out << "ScopeFS\n";
  out << "mount state " << toString(state_.super.mountState) << "\n";
  out << "next transaction id " << state_.super.nextTxid << "\n";
  out << "root index node " << state_.super.activeRoot << "\n";
  out << "index nodes " << state_.inodes.size() << "/" << config::kInodeCount << "\n";
  out << "data blocks " << state_.blocks.size() << "/" << config::kTotalBlocks << "\n";
  out << "snapshots " << state_.snapshots.size() << "\n";
  out << "trace " << (trace_.enabled() ? "on" : "off") << "\n";
  return out.str();
}

std::string FileSystemKernel::renderTree(std::uint32_t inode, const std::string& prefix, const std::string& name, bool last, std::set<std::uint32_t>& seen) const {
  std::ostringstream out;
  auto it = state_.inodes.find(inode);
  if (it == state_.inodes.end()) return "";
  const bool root = prefix.empty() && name == "/";
  out << prefix;
  if (!root) out << (last ? "└─ " : "├─ ");
  out << name << " [" << it->second.id << ":" << plainTypeLabel(it->second.type, langName_)
      << " " << (langName_ == "zh" ? "引用数 " : "reference count ") << it->second.refcount << "]\n";
  if (it->second.type != NodeType::Directory || seen.count(inode)) return out.str();
  seen.insert(inode);
  std::vector<std::pair<std::string, DirEntry>> children;
  for (const auto& [entryName, de] : it->second.entries) {
    if (entryName != "." && entryName != "..") children.push_back({entryName, de});
  }
  const auto childPrefix = root ? "" : prefix + (last ? "   " : "│  ");
  for (std::size_t i = 0; i < children.size(); ++i) {
    out << renderTree(children[i].second.inode, childPrefix, children[i].first, i + 1 == children.size(), seen);
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
    if (what == "refcount") {
      std::size_t shown = 0;
      for (const auto& [id, block] : state_.blocks) {
        if (block.refcount <= 1) continue;
        if (shown == 0) out << "shared hotspots\n";
        out << "block " << id << " ref=" << block.refcount << " owner=#" << block.ownerInode << "\n";
        if (++shown >= 8) break;
      }
    }
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
    coordinator_.bumpEpoch();
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
    case ErrorCode::LockBusy: return "E_LOCK_BUSY";
    case ErrorCode::StaleLock: return "E_STALE_LOCK";
    case ErrorCode::ConcurrentReload: return "E_CONCURRENT_RELOAD";
    case ErrorCode::SignalCrash: return "E_SIGNAL_CRASH";
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
