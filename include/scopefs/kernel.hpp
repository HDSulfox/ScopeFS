#pragma once

#include <functional>
#include <map>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "scopefs/block_device.hpp"
#include "scopefs/model.hpp"
#include "scopefs/ui.hpp"

namespace scopefs {

enum class ErrorCode {
  Ok,
  NeedLogin,
  AlreadyLoggedIn,
  AuthFailed,
  NotFound,
  Exists,
  NotDirectory,
  IsDirectory,
  DirectoryNotEmpty,
  PermissionDenied,
  InvalidCommand,
  InvalidArgument,
  InvalidFd,
  NoSpace,
  Busy,
  CrashInjected,
  IoError
};

struct CommandResult {
  ErrorCode code = ErrorCode::Ok;
  std::string message = "ok";
  std::string output;
};

struct CrashException : std::runtime_error {
  explicit CrashException(const std::string& event)
      : std::runtime_error("crash injected at " + event), eventName(event) {}
  std::string eventName;
};

class FileSystemKernel {
 public:
  FileSystemKernel();
  void boot();
  void unmountClean();
  CommandResult execute(const std::vector<std::string>& args, const std::string& rawCommand);
  void setInteractiveUi(bool enabled, bool ansi);
  ui::KernelStatus status() const;
  std::string uiThemeName() const;
  std::string uiLanguageName() const;
  bool uiAnsiEnabled() const;
  TraceSink& trace();
  std::string prompt() const;
  std::string currentUser() const;
  bool isMounted() const;
  std::vector<std::string> completePath(const std::string& token, bool directoriesOnly) const;

 private:
  struct ResolvedPath {
    std::string canonical;
    std::string parentPath;
    std::string leaf;
    std::uint32_t inode = 0;
    std::uint32_t parent = 0;
  };

  struct Tx {
    std::uint64_t id = 0;
    std::string name;
    std::string before;
    bool active = false;
  };

  TraceSink trace_;
  BlockDevice device_;
  FsState state_;
  std::map<std::string, UserSession> sessions_;
  std::map<std::uint32_t, int> systemOpen_;
  std::string activeUser_;
  bool mounted_ = false;
  bool dirty_ = false;
  bool interactiveUi_ = false;
  bool ansiUi_ = false;
  std::string themeName_ = "scope-dark";
  std::string langName_ = "zh";
  std::string crashMode_;
  std::string crashEvent_;
  std::uint64_t crashSeq_ = 0;

  CommandResult ok(const std::string& out = "");
  CommandResult err(ErrorCode code, const std::string& message, const std::string& out = "");
  std::string usage(const std::string& spec) const;
  std::string msg(const std::string& en, const std::string& zh) const;
  bool requiresLogin(const std::string& command) const;
  bool ensureLogged(CommandResult* result) const;
  bool isRoot() const;
  UserSession& session();
  const UserSession& session() const;

  void initFreshState();
  void saveState();
  void loadState(const std::string& text);
  std::string serializeState() const;
  void deserializeState(const std::string& text);
  void recomputeSuper();
  void setMountState(MountState state);
  void recoverIfNeeded();
  void recoveryFromJournal(const std::vector<std::string>& journal);

  Tx beginTx(const std::string& name);
  void commitTx(Tx& tx);
  void journalLine(const std::string& line);
  void crashPoint(const std::string& event, const std::string& phase);
  void maybeCrashNow();

  std::uint32_t allocateInode(NodeType type, const std::string& owner, const std::string& klass, int mode);
  std::uint32_t allocateBlock(std::uint32_t ownerInode, const std::string& data, const std::string& flags, std::uint64_t txid);
  void retainInode(std::uint32_t inode);
  void releaseInode(std::uint32_t inode, bool recursive);
  void retainBlock(std::uint32_t block);
  void releaseBlock(std::uint32_t block);
  std::string readFileData(const Inode& inode) const;
  void setFileData(Inode& inode, const std::string& data, std::uint64_t txid);
  void refreshDirBlock(Inode& inode, std::uint64_t txid);

  ResolvedPath resolve(const std::string& path, bool mustExist);
  std::string canonicalize(const std::string& base, const std::string& path) const;
  bool ensureMutablePath(const std::string& targetPath, bool includeLeaf, std::uint64_t txid, ResolvedPath* updated);
  std::uint32_t cloneInode(std::uint32_t src, std::uint64_t txid);

  bool authCheck(std::uint32_t inode, const std::string& right, const std::string& path, std::string* reason);
  bool canTraverse(const std::string& canonical, bool includeTarget, std::string* reason);
  std::set<std::string> effectiveClasses(const std::string& user, const std::string& path) const;
  bool constraintsAllow(const std::string& constraints, const std::string& path) const;

  CommandResult cmdFormat();
  CommandResult cmdLogin(const std::vector<std::string>& args);
  CommandResult cmdLogout();
  CommandResult cmdWhoami();
  CommandResult cmdMkdir(const std::vector<std::string>& args);
  CommandResult cmdRmdir(const std::vector<std::string>& args);
  CommandResult cmdChdir(const std::vector<std::string>& args);
  CommandResult cmdDir(const std::vector<std::string>& args);
  CommandResult cmdCreate(const std::vector<std::string>& args);
  CommandResult cmdOpen(const std::vector<std::string>& args);
  CommandResult cmdRead(const std::vector<std::string>& args);
  CommandResult cmdWrite(const std::vector<std::string>& args, const std::string& rawCommand);
  CommandResult cmdClose(const std::vector<std::string>& args);
  CommandResult cmdDelete(const std::vector<std::string>& args);
  CommandResult cmdTruncate(const std::vector<std::string>& args);
  CommandResult cmdTrace(const std::vector<std::string>& args);
  CommandResult cmdScope(const std::vector<std::string>& args);
  CommandResult cmdMap(const std::vector<std::string>& args);
  CommandResult cmdSnapshot(const std::vector<std::string>& args);
  CommandResult cmdClone(const std::vector<std::string>& args);
  CommandResult cmdClass(const std::vector<std::string>& args);
  CommandResult cmdAcl(const std::vector<std::string>& args);
  CommandResult cmdChmod(const std::vector<std::string>& args);
  CommandResult cmdChown(const std::vector<std::string>& args);
  CommandResult cmdChclass(const std::vector<std::string>& args);
  CommandResult cmdFsck(const std::vector<std::string>& args);
  CommandResult cmdCrash(const std::vector<std::string>& args);
  CommandResult cmdTheme(const std::vector<std::string>& args);
  CommandResult cmdLang(const std::vector<std::string>& args);
  CommandResult cmdHelp();

  std::string renderDir(std::uint32_t inode, const std::string& path) const;
  std::string renderScope(const std::string& what) const;
  std::string renderTree(std::uint32_t inode, const std::string& prefix, const std::string& name, std::set<std::uint32_t>& seen) const;
  std::string renderMap(const std::string& what) const;
  std::string fsck(bool repair, bool light);
  std::map<std::string, std::uint32_t> flattenTree(std::uint32_t root) const;
};

std::string errorCodeName(ErrorCode code);
std::ostream& operator<<(std::ostream& out, const CommandResult& result);

} // namespace scopefs
