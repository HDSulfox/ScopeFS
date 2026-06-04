#include "scopefs/ui.hpp"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace scopefs::ui {

namespace {

std::string esc(const std::string& code) { return "\x1b[" + code + "m"; }
const std::string kBoxRule = "\x1fscopefs.box.rule";

std::string zh(const std::string& key) {
  static const std::map<std::string, std::string> dict = {
      {"command_focus", "命令焦点"},
      {"command_surface", "命令工作区"},
      {"observable_kernel", "可观测索引节点/块/日志/COW 内核"},
      {"terminal_first", "终端优先"},
      {"volume", "卷状态"},
      {"session", "会话"},
      {"observability", "观测"},
      {"mount", "挂载"},
      {"blocks", "数据块"},
      {"inode", "索引节点"},
      {"journal", "日志"},
      {"tx", "事务"},
      {"cwd", "当前目录"},
      {"user", "用户"},
      {"open", "打开文件数"},
      {"trace", "跟踪"},
      {"snap", "快照"},
      {"tips", "提示"},
      {"tab_commands", "Tab 补全"},
      {"palette", "命令面板"},
      {"directory", "目录"},
      {"entries", "项"},
      {"gen", "版本号"},
      {"ref", "引用数"},
      {"owner_group", "归属"},
      {"size", "大小"},
      {"created", "创建时间"},
      {"modified", "修改时间"},
      {"kernel", "内核"},
      {"capacity", "容量"},
      {"hot_inodes", "热点索引节点"},
      {"no_inode_activity", "暂无索引节点活动"},
      {"tree_shared", "目录树 / 共享引用"},
      {"disk_map", "磁盘图"},
      {"trace_timeline", "跟踪时间线"},
      {"trace_replay", "跟踪回放 / 只读"},
      {"trace_step", "跟踪单步"},
      {"snapshot_diff", "快照 diff"},
      {"class_graph", "用户组图"},
      {"acl_graph", "ACL 图"},
      {"read_result", "读取结果"},
      {"offset", "偏移"},
      {"read_advance", "本次读取"},
      {"fd_offset", "fd 偏移"}};
  const auto it = dict.find(key);
  return it == dict.end() ? key : it->second;
}

std::string en(const std::string& key) {
  static const std::map<std::string, std::string> dict = {
      {"command_focus", "command focus"},
      {"command_surface", "command surface"},
      {"observable_kernel", "observable index-node/block/journal/COW kernel"},
      {"terminal_first", "terminal-first"},
      {"volume", "Volume"},
      {"session", "Session"},
      {"observability", "Observability"},
      {"mount", "mount"},
      {"blocks", "data blocks"},
      {"inode", "index node"},
      {"journal", "journal"},
      {"tx", "transaction"},
      {"cwd", "current directory"},
      {"user", "user"},
      {"open", "open files"},
      {"trace", "trace"},
      {"snap", "snapshots"},
      {"tips", "tips"},
      {"tab_commands", "tab commands"},
      {"palette", "ctrl+p palette"},
      {"directory", "Directory"},
      {"entries", "entries"},
      {"gen", "version"},
      {"ref", "reference count"},
      {"owner_group", "ownership"},
      {"size", "size"},
      {"created", "created time"},
      {"modified", "modified time"},
      {"kernel", "Kernel"},
      {"capacity", "Capacity"},
      {"hot_inodes", "Hot index nodes"},
      {"no_inode_activity", "no index-node activity"},
      {"tree_shared", "Tree / shared references"},
      {"disk_map", "Disk map"},
      {"trace_timeline", "Trace timeline"},
      {"trace_replay", "Trace replay / read-only"},
      {"trace_step", "Trace step"},
      {"snapshot_diff", "Snapshot diff"},
      {"class_graph", "Group graph"},
      {"acl_graph", "ACL graph"},
      {"read_result", "Read result"},
      {"offset", "offset"},
      {"read_advance", "read"},
      {"fd_offset", "fd offset"}};
  const auto it = dict.find(key);
  return it == dict.end() ? key : it->second;
}

std::string toneCode(const Theme& th, const std::string& tone) {
  if (tone == "amber") return th.amber;
  if (tone == "blue") return th.blue;
  if (tone == "green") return th.green;
  if (tone == "red") return th.red;
  if (tone == "magenta") return th.magenta;
  if (tone == "gray") return th.gray;
  if (tone == "dim") return th.dim;
  if (tone == "white") return th.white;
  if (tone == "panel") return th.panel;
  return th.border;
}

std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
  }
  if (lines.empty()) lines.emplace_back();
  return lines;
}

std::string iconForType(const std::string& type) {
  if (type == "dir") return "◆";
  if (type == "snap") return "◈";
  if (type == "class") return "◇";
  return "▪";
}

std::string typeTone(const std::string& type) {
  if (type == "dir") return "amber";
  if (type == "snap") return "magenta";
  if (type == "class") return "blue";
  return "white";
}

std::string dirTypeLabel(const Theme& th, const std::string& type) {
  if (type == "dir") return th.lang == "zh" ? "目录" : "directory";
  if (type == "snap") return th.lang == "zh" ? "快照根" : "snapshot root";
  if (type == "class") return th.lang == "zh" ? "用户组对象" : "class object";
  return th.lang == "zh" ? "文件" : "file";
}

std::string labelValue(const Theme& th, const std::string& label, const std::string& value, const std::string& tone = "white") {
  return color(th, th.dim, label + " ") + accent(th, toneCode(th, tone), value);
}

std::string traceTypeLabel(const Theme& th, const std::string& type) {
  static const std::map<std::string, std::string> zhMap = {
      {"coord.session.start", "会话启动"},
      {"coord.session.stop", "会话结束"},
      {"coord.lock.acquire", "获取锁"},
      {"coord.lock.release", "释放锁"},
      {"coord.lock.wait", "等待锁"},
      {"coord.lock.deny", "锁忙"},
      {"coord.lock.clear_stale", "清理陈旧锁"},
      {"coord.epoch.bump", "卷版本递增"},
      {"coord.epoch.reload", "重载卷状态"},
      {"coord.signal.crash", "广播崩溃信号"},
      {"coord.signal.receive", "收到崩溃信号"},
      {"system.crash.detected", "检测到终端崩溃"},
      {"block.read", "读取块层"},
      {"block.write", "写入块层"},
      {"block.alloc", "分配数据块"},
      {"block.retain", "保留块引用"},
      {"block.release", "释放块引用"},
      {"journal.begin", "事务开始"},
      {"journal.record", "记录事务镜像"},
      {"journal.record.write", "写入日志记录"},
      {"journal.commit", "事务提交"},
      {"journal.checkpoint", "事务检查点"},
      {"journal.clear", "清空日志"},
      {"mount.missing", "卷未找到"},
      {"mount.open", "挂载打开"},
      {"mount.close", "挂载关闭"},
      {"super.mount_state", "挂载状态切换"},
      {"recovery.begin", "恢复开始"},
      {"recovery.redo", "恢复重放"},
      {"recovery.ignore", "忽略未提交事务"},
      {"recovery.end", "恢复完成"},
      {"fsck.light", "轻量文件系统检查"},
      {"fsck.full", "完整文件系统检查"},
      {"crash.point", "崩溃注入点"},
      {"crash.inject", "触发崩溃"},
      {"path.lookup", "路径解析"},
      {"auth.check", "权限判定"},
      {"inode.alloc", "分配索引节点"},
      {"inode.retain", "保留索引节点引用"},
      {"inode.release", "释放索引节点引用"},
      {"inode.free", "回收索引节点"},
      {"inode.clone", "复制索引节点"},
      {"inode.write_map", "更新块映射"},
      {"inode.lock.acquire", "获取索引节点锁"},
      {"inode.lock.release", "释放索引节点锁"},
      {"inode.lock.conflict", "索引节点锁冲突"},
      {"inode.lock.delete_pending", "标记延迟删除"},
      {"inode.delete_pending", "标记延迟删除"},
      {"inode.chmod", "修改模式"},
      {"inode.chown", "修改属主"},
      {"inode.chclass", "修改所属用户组"},
      {"cow.break", "写时拷贝断开"},
      {"cow.path_root", "复制共享根"},
      {"cow.path_node", "复制共享路径"},
      {"session.login", "用户登录"},
      {"session.chdir", "切换目录"},
      {"open.fd", "打开文件描述符"},
      {"open.close", "关闭文件描述符"},
      {"file.create", "创建文件"},
      {"file.read", "读取文件"},
      {"file.write", "写入文件"},
      {"file.delete", "删除文件"},
      {"file.clone", "复制文件"},
      {"dir.mkdir", "创建目录"},
      {"dir.rmdir", "删除目录"},
      {"snapshot.create", "创建快照"},
      {"snapshot.rollback", "回滚快照"},
      {"snapshot.delete", "删除快照"},
      {"class.create", "创建用户组"},
      {"class.grant", "授予用户组"},
      {"class.revoke", "收回用户组"},
      {"acl.grant", "授予 ACL"},
      {"acl.revoke", "收回 ACL"},
      {"trace.on", "开启跟踪"},
      {"trace.off", "关闭跟踪"},
      {"trace.clear", "清空跟踪"},
      {"command.error", "命令错误"}};
  static const std::map<std::string, std::string> enMap = {
      {"coord.session.start", "session start"},
      {"coord.session.stop", "session stop"},
      {"coord.lock.acquire", "acquire lock"},
      {"coord.lock.release", "release lock"},
      {"coord.lock.wait", "wait lock"},
      {"coord.lock.deny", "lock busy"},
      {"coord.lock.clear_stale", "clear stale locks"},
      {"coord.epoch.bump", "bump volume epoch"},
      {"coord.epoch.reload", "reload volume state"},
      {"coord.signal.crash", "broadcast crash signal"},
      {"coord.signal.receive", "receive crash signal"},
      {"system.crash.detected", "detect terminal crash"},
      {"block.read", "read block layer"},
      {"block.write", "write block layer"},
      {"block.alloc", "allocate data block"},
      {"block.retain", "retain block ref"},
      {"block.release", "release block ref"},
      {"journal.begin", "begin transaction"},
      {"journal.record", "record transaction image"},
      {"journal.record.write", "write journal record"},
      {"journal.commit", "commit transaction"},
      {"journal.checkpoint", "checkpoint transaction"},
      {"journal.clear", "clear journal"},
      {"mount.missing", "volume missing"},
      {"mount.open", "mount open"},
      {"mount.close", "mount close"},
      {"super.mount_state", "mount state change"},
      {"recovery.begin", "recovery begin"},
      {"recovery.redo", "recovery redo"},
      {"recovery.ignore", "ignore uncommitted transaction"},
      {"recovery.end", "recovery complete"},
      {"fsck.light", "light file-system check"},
      {"fsck.full", "full file-system check"},
      {"crash.point", "crash point"},
      {"crash.inject", "trigger crash"},
      {"path.lookup", "path lookup"},
      {"auth.check", "permission check"},
      {"inode.alloc", "allocate index node"},
      {"inode.retain", "retain index-node ref"},
      {"inode.release", "release index-node ref"},
      {"inode.free", "free index node"},
      {"inode.clone", "copy index node"},
      {"inode.write_map", "update block map"},
      {"inode.lock.acquire", "acquire index-node lock"},
      {"inode.lock.release", "release index-node lock"},
      {"inode.lock.conflict", "index-node lock conflict"},
      {"inode.lock.delete_pending", "mark delete pending"},
      {"inode.delete_pending", "mark delete pending"},
      {"inode.chmod", "change mode"},
      {"inode.chown", "change owner"},
      {"inode.chclass", "change group"},
      {"cow.break", "break COW sharing"},
      {"cow.path_root", "copy shared root"},
      {"cow.path_node", "copy shared path"},
      {"session.login", "user login"},
      {"session.chdir", "change directory"},
      {"open.fd", "open file descriptor"},
      {"open.close", "close file descriptor"},
      {"file.create", "create file"},
      {"file.read", "read file"},
      {"file.write", "write file"},
      {"file.delete", "delete file"},
      {"file.clone", "copy file"},
      {"dir.mkdir", "make directory"},
      {"dir.rmdir", "remove directory"},
      {"snapshot.create", "create snapshot"},
      {"snapshot.rollback", "rollback snapshot"},
      {"snapshot.delete", "delete snapshot"},
      {"class.create", "create user group"},
      {"class.grant", "grant user group"},
      {"class.revoke", "revoke user group"},
      {"acl.grant", "grant ACL"},
      {"acl.revoke", "revoke ACL"},
      {"trace.on", "trace on"},
      {"trace.off", "trace off"},
      {"trace.clear", "clear trace"},
      {"command.error", "command error"}};
  const auto& map = th.lang == "zh" ? zhMap : enMap;
  const auto it = map.find(type);
  if (it != map.end()) return it->second;
  auto fallback = type;
  std::replace(fallback.begin(), fallback.end(), '.', ' ');
  std::replace(fallback.begin(), fallback.end(), '_', ' ');
  return fallback;
}

std::string traceObjectLabel(const Theme& th, const std::string& object) {
  static const std::map<std::string, std::string> zhMap = {
      {"tx", "卷级独占写"},
      {"read", "卷级共享读"},
      {"mutex", "协调互斥"},
      {"signal", "崩溃信号"},
      {"volume", "虚拟卷"},
      {"journal", "日志区"},
      {"superblock", "超级块"},
      {"epoch", "卷版本"},
      {"locks", "锁表"},
      {"trace", "跟踪开关"},
      {"ring", "跟踪缓冲区"},
      {"before.command.dispatch", "命令分发前"}};
  static const std::map<std::string, std::string> enMap = {
      {"tx", "volume-exclusive write"},
      {"read", "volume-shared read"},
      {"mutex", "coord mutex"},
      {"signal", "crash signal"},
      {"volume", "virtual volume"},
      {"journal", "journal area"},
      {"superblock", "superblock"},
      {"epoch", "volume epoch"},
      {"locks", "lock table"},
      {"trace", "trace switch"},
      {"ring", "trace ring"},
      {"before.command.dispatch", "before command dispatch"}};
  const auto& map = th.lang == "zh" ? zhMap : enMap;
  const auto it = map.find(object);
  if (it != map.end()) return it->second;
  return object;
}

std::string statusTone(const std::string& status) {
  if (status == "ok" || status == "allow" || status == "redo") return "green";
  if (status == "wait" || status == "busy" || status == "need_format" || status == "issues") return "amber";
  if (status == "deny" || status == "error" || status == "crash") return "red";
  if (status == "ignored") return "gray";
  return "gray";
}

bool startsWithText(const std::string& text, const std::string& prefix) {
  return text.rfind(prefix, 0) == 0;
}

std::string eventTone(const TraceEvent& event) {
  if (event.status == "deny" || event.status == "error" || event.status == "crash" || event.status == "busy") return "red";
  if (event.type.find("commit") != std::string::npos || event.type.find("checkpoint") != std::string::npos ||
      event.type.find("fsck") != std::string::npos) {
    return "green";
  }
  if (startsWithText(event.type, "snapshot.") || startsWithText(event.type, "cow.")) return "magenta";
  if (startsWithText(event.type, "path.") || event.type == "auth.check" || startsWithText(event.type, "coord.lock.")) return "blue";
  if (startsWithText(event.type, "journal.") || startsWithText(event.type, "block.")) return "amber";
  return "gray";
}

bool traceOperationEvent(const TraceEvent& e) {
  return startsWithText(e.type, "file.") || startsWithText(e.type, "dir.") || startsWithText(e.type, "snapshot.") ||
         startsWithText(e.type, "acl.") || startsWithText(e.type, "class.") || e.type == "open.fd" || e.type == "open.close" ||
         e.type == "inode.chmod" || e.type == "inode.chown" || e.type == "inode.chclass";
}

bool traceStorageEvent(const TraceEvent& e) {
  return startsWithText(e.type, "inode.") || startsWithText(e.type, "block.");
}

bool pathChildOf(const std::string& parent, const std::string& child) {
  if (parent.empty() || child.empty() || parent == child || child == "/") return false;
  if (parent == "/") return startsWithText(child, "/") && child.size() > 1;
  return startsWithText(child, parent + "/");
}

std::string txLabel(const Theme& th, std::uint64_t txid) {
  return std::string(th.lang == "zh" ? "事务" : "transaction") + "#" + std::to_string(txid);
}

std::string mapWhatLabel(const Theme& th, const std::string& what) {
  if (what == "inode") return th.lang == "zh" ? "索引节点" : "index node";
  if (what == "journal") return th.lang == "zh" ? "日志" : "journal";
  if (what == "blocks") return th.lang == "zh" ? "块" : "blocks";
  if (what == "refcount") return th.lang == "zh" ? "引用数" : "reference count";
  if (what == "owner") return th.lang == "zh" ? "属主" : "owner";
  return what;
}

std::string extraLabel(const Theme& th, const std::string& key) {
  if (key == "cwd") return th.lang == "zh" ? "当前目录" : "current directory";
  if (key == "trace") return th.lang == "zh" ? "跟踪" : "trace";
  if (key == "classes") return th.lang == "zh" ? "用户组" : "groups";
  if (key == "view") return th.lang == "zh" ? "视图" : "view";
  if (key == "mode") return th.lang == "zh" ? "模式" : "mode";
  return key;
}

std::string blockGlyph(std::uint32_t refcount) {
  if (refcount == 0) return "·";
  if (refcount == 1) return "░";
  if (refcount == 2) return "▒";
  return "█";
}

std::string blockTone(const MapCell& cell) {
  if (cell.flags == "journal") return "green";
  if (cell.flags == "inode") return "blue";
  if (cell.flags == "refcount") return "amber";
  if (cell.refcount > 1) return "magenta";
  if (cell.refcount == 1) return "gray";
  return "dim";
}

std::string layoutGlyph(const MapCell& cell) {
  if (cell.flags == "super") return "S";
  if (cell.flags == "refcount") return "R";
  if (cell.flags == "inode") return "I";
  if (cell.flags == "journal") return "J";
  if (cell.flags == "snapshot") return "P";
  if (cell.flags == "user") return "U";
  if (cell.refcount) return "D";
  return ".";
}

std::string ownerGlyph(std::uint32_t ownerInode) {
  if (ownerInode == 0) return ".";
  constexpr char digits[] = "0123456789abcdef";
  return std::string(1, digits[ownerInode % 16]);
}

std::string txGlyph(std::uint64_t txid) {
  if (txid == 0) return ".";
  constexpr char digits[] = "0123456789abcdef";
  return std::string(1, digits[txid % 16]);
}

std::string mapGlyph(const std::string& what, const MapCell& cell) {
  if (what == "blocks") return layoutGlyph(cell);
  if (what == "inode") {
    if (cell.flags == "inode") return "I";
    if (cell.flags == "data" && cell.ownerInode != 0) return ownerGlyph(cell.ownerInode);
    if (cell.refcount > 1) return "*";
    return ".";
  }
  if (what == "journal") {
    if (cell.flags == "journal") return "J";
    if (cell.lastWriterTxid != 0) return txGlyph(cell.lastWriterTxid);
    if (cell.flags == "super") return "S";
    return ".";
  }
  if (what == "owner") return ownerGlyph(cell.ownerInode);
  return blockGlyph(cell.refcount);
}

std::string mapTone(const Theme& th, const std::string& what, const MapCell& cell) {
  (void)th;
  if (what == "blocks") return blockTone(cell);
  if (what == "inode") {
    if (cell.flags == "inode") return "blue";
    if (cell.refcount > 1) return "magenta";
    if (cell.ownerInode != 0) return "amber";
    return "dim";
  }
  if (what == "journal") {
    if (cell.flags == "journal") return "green";
    if (cell.lastWriterTxid != 0) return "amber";
    if (cell.flags == "super") return "white";
    return "dim";
  }
  if (what == "owner") {
    if (cell.refcount > 1) return "magenta";
    return cell.ownerInode ? "amber" : "dim";
  }
  return blockTone(cell);
}

std::string mapLegend(const Theme& th, const std::string& what) {
  const auto inodeLabel = th.lang == "zh" ? "索引节点" : "index node";
  const auto journalLabel = th.lang == "zh" ? "日志" : "journal";
  if (what == "blocks") {
    return color(th, th.dim, th.lang == "zh" ? "图例 " : "legend ") +
           color(th, th.white, th.lang == "zh" ? "S 超级块 " : "S super ") +
           color(th, th.amber, th.lang == "zh" ? "R 引用元数据 " : "R refmeta ") +
           color(th, th.blue, "I " + std::string(inodeLabel) + " ") +
           color(th, th.green, "J " + std::string(journalLabel) + " ") +
           color(th, th.magenta, th.lang == "zh" ? "P 快照 " : "P snapshot ") +
           color(th, th.gray, th.lang == "zh" ? "U 用户 D 数据 . 空闲" : "U users D data . free");
  }
  if (what == "inode") {
    return color(th, th.dim, th.lang == "zh" ? "图例 " : "legend ") +
           color(th, th.blue, th.lang == "zh" ? "I 索引节点表 " : "I index-node table ") +
           color(th, th.amber, th.lang == "zh" ? "0-f 属主索引节点数据 " : "0-f owner index-node data ") +
           color(th, th.magenta, th.lang == "zh" ? "* 共享 " : "* shared ") +
           color(th, th.gray, th.lang == "zh" ? ". 无关/空闲" : ". unrelated/free");
  }
  if (what == "journal") {
    return color(th, th.dim, th.lang == "zh" ? "图例 " : "legend ") +
           color(th, th.green, th.lang == "zh" ? "J 日志保留区 " : "J journal reserved ") +
           color(th, th.amber, th.lang == "zh" ? "0-f 最近写入事务 " : "0-f last-writer transaction ") +
           color(th, th.white, th.lang == "zh" ? "S 超级块 " : "S super ") +
           color(th, th.gray, th.lang == "zh" ? ". 无日志信号" : ". no journal signal");
  }
  if (what == "owner") {
    return color(th, th.dim, th.lang == "zh" ? "图例 " : "legend ") +
           color(th, th.amber, th.lang == "zh" ? "0-f 属主索引节点 mod16 " : "0-f owner index-node modulo 16 ") +
           color(th, th.magenta, th.lang == "zh" ? "共享高亮 " : "shared highlighted ") +
           color(th, th.gray, th.lang == "zh" ? ". 无属主" : ". no owner");
  }
  return color(th, th.dim, th.lang == "zh" ? "图例 " : "legend ") +
         color(th, th.gray, th.lang == "zh" ? ". 空闲 " : ". free ") +
         color(th, th.white, th.lang == "zh" ? "░ 引用数 1 " : "░ reference count 1 ") +
         color(th, th.magenta, th.lang == "zh" ? "▒/█ 共享引用" : "▒/█ shared references");
}

std::string repeatText(const std::string& text, int count) {
  std::string out;
  for (int i = 0; i < count; ++i) out += text;
  return out;
}

std::string indentBlock(const std::string& text, int spaces) {
  const std::string indent(std::max(0, spaces), ' ');
  std::ostringstream out;
  std::istringstream in(text);
  std::string line;
  bool first = true;
  while (std::getline(in, line)) {
    if (!first) out << "\n";
    out << indent << line;
    first = false;
  }
  return out.str();
}

std::string rawFg(const std::string& code, const std::string& text) {
  return code + text;
}

} // namespace

TerminalMetrics detectMetrics() {
  TerminalMetrics metrics;
  if (const char* forced = std::getenv("SCOPEFS_WIDTH")) {
    const int w = std::atoi(forced);
    if (w > 0) metrics.columns = w;
  }
#if defined(_WIN32)
  CONSOLE_SCREEN_BUFFER_INFO info{};
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (out != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(out, &info)) {
    metrics.columns = info.srWindow.Right - info.srWindow.Left + 1;
    metrics.rows = info.srWindow.Bottom - info.srWindow.Top + 1;
  }
#else
  winsize size{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
    metrics.columns = size.ws_col;
    metrics.rows = size.ws_row > 0 ? size.ws_row : metrics.rows;
  }
#endif
  if (const char* forced = std::getenv("SCOPEFS_WIDTH")) {
    const int w = std::atoi(forced);
    if (w > 0) metrics.columns = w;
  }
  metrics.columns = std::max(72, metrics.columns);
  metrics.compact = metrics.columns < 88;
  metrics.wide = metrics.columns >= 140;
  return metrics;
}

Theme theme(bool ansi, const std::string& name, const std::string& lang) {
  Theme th;
  th.ansi = ansi;
  th.lang = lang == "en" ? "en" : "zh";
  th.mono = name == "mono" || !ansi;
  if (!ansi) return th;
  th.reset = esc("0");
  th.bg = esc("48;2;11;12;14");
  th.panel = esc("48;2;28;29;33");
  th.panel2 = esc("48;2;36;36;40");
  th.bold = esc("1");
  th.normalWeight = esc("22");
  th.amber = esc("38;2;255;181;118");
  th.blue = esc("38;2;112;170;255");
  th.green = esc("38;2;110;220;153");
  th.red = esc("38;2;255;110;110");
  th.magenta = esc("38;2;218;140;255");
  th.gray = esc("38;2;165;166;173");
  th.dim = esc("38;2;104;106;114");
  th.white = esc("38;2;242;243;245");
  th.border = esc("38;2;125;96;88");
  if (name == "blue") {
    th.amber = esc("38;2;112;170;255");
    th.blue = esc("38;2;255;181;118");
    th.border = esc("38;2;80;116;150");
  }
  return th;
}

std::string text(const Theme& th, const std::string& key) {
  return th.lang == "en" ? en(key) : zh(key);
}

int displayWidth(const std::string& text) {
  int width = 0;
  for (std::size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c == 0x1b) {
      while (i < text.size() && text[i] != 'm') ++i;
      if (i < text.size()) ++i;
      continue;
    }
    if (c < 0x80) {
      ++width;
      ++i;
    } else {
      std::uint32_t cp = 0;
      std::size_t len = 1;
      if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
        cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3F);
        len = 2;
      } else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
        cp = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6) |
             (static_cast<unsigned char>(text[i + 2]) & 0x3F);
        len = 3;
      } else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) {
        cp = ((c & 0x07) << 18) | ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 12) |
             ((static_cast<unsigned char>(text[i + 2]) & 0x3F) << 6) |
             (static_cast<unsigned char>(text[i + 3]) & 0x3F);
        len = 4;
      }
      const bool wide = (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
                        (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x1F300 && cp <= 0x1FAFF);
      width += wide ? 2 : 1;
      i += len;
    }
  }
  return width;
}

std::string stripAnsi(const std::string& text) {
  std::string out;
  for (std::size_t i = 0; i < text.size();) {
    if (text[i] == '\x1b') {
      while (i < text.size() && text[i] != 'm') ++i;
      if (i < text.size()) ++i;
    } else {
      out.push_back(text[i++]);
    }
  }
  return out;
}

std::string truncate(const std::string& text, int width) {
  if (width <= 0) return "";
  if (displayWidth(text) <= width) return text;
  const std::string plain = stripAnsi(text);
  std::string out;
  int used = 0;
  for (std::size_t i = 0; i < plain.size() && used < width - 1;) {
    unsigned char c = static_cast<unsigned char>(plain[i]);
    std::size_t len = 1;
    if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    const auto chunk = plain.substr(i, std::min(len, plain.size() - i));
    const int w = displayWidth(chunk);
    if (used + w > width - 1) break;
    out += chunk;
    used += w;
    i += len;
  }
  return out + "…";
}

std::string padRight(const std::string& text, int width) {
  const auto clipped = truncate(text, width);
  return clipped + std::string(std::max(0, width - displayWidth(clipped)), ' ');
}

std::string padLeft(const std::string& text, int width) {
  const auto clipped = truncate(text, width);
  return std::string(std::max(0, width - displayWidth(clipped)), ' ') + clipped;
}

std::string center(const std::string& text, int width) {
  const auto clipped = truncate(text, width);
  const int remain = std::max(0, width - displayWidth(clipped));
  return std::string(remain / 2, ' ') + clipped + std::string(remain - remain / 2, ' ');
}

std::string color(const Theme& th, const std::string& code, const std::string& text) {
  if (!th.ansi || th.mono) return text;
  return code + text + th.reset;
}

std::string accent(const Theme& th, const std::string& code, const std::string& text) {
  if (!th.ansi || th.mono) return text;
  return code + th.bold + text + th.reset;
}

std::string panelAccent(const Theme& th, const std::string& code, const std::string& text) {
  if (!th.ansi || th.mono) return text;
  return code + th.bold + text + th.normalWeight;
}

std::string badge(const Theme& th, const std::string& label, const std::string& tone) {
  const auto text = " " + label + " ";
  if (!th.ansi || th.mono) return "[" + label + "]";
  return toneCode(th, tone) + th.bold + text + th.reset;
}

std::string progress(const Theme& th, std::size_t used, std::size_t total, int width, const std::string& tone) {
  if (width <= 0) return "";
  const double ratio = total == 0 ? 0.0 : static_cast<double>(used) / static_cast<double>(total);
  const int fill = std::max(0, std::min(width, static_cast<int>(ratio * width + 0.5)));
  std::string bar = repeatText("█", fill) + repeatText("░", width - fill);
  return color(th, toneCode(th, tone), bar);
}

std::string box(const Theme& th, const std::string& title, const std::vector<std::string>& lines, int width, const std::string& tone) {
  width = std::max(24, width);
  const int inner = width - 4;
  const auto border = toneCode(th, tone);
  std::ostringstream out;
  const std::string titleText = title.empty() ? "" : " " + title + " ";
  const int titleWidth = displayWidth(titleText);
  out << color(th, border, "╭" + titleText + repeatText("─", std::max(0, width - 2 - titleWidth)) + "╮") << "\n";
  for (const auto& line : lines) {
    if (line == kBoxRule) {
      out << color(th, border, "├" + repeatText("─", width - 2) + "┤") << "\n";
      continue;
    }
    out << color(th, border, "│ ") << padRight(line, inner) << color(th, border, " │") << "\n";
  }
  out << color(th, border, "╰" + repeatText("─", width - 2) + "╯");
  return out.str();
}

std::string columns(const std::vector<std::string>& boxed, int gap) {
  std::vector<std::vector<std::string>> cols;
  std::size_t rows = 0;
  for (const auto& b : boxed) {
    cols.push_back(splitLines(b));
    rows = std::max(rows, cols.back().size());
  }
  std::vector<int> widths(cols.size(), 0);
  for (std::size_t c = 0; c < cols.size(); ++c) {
    for (const auto& line : cols[c]) widths[c] = std::max(widths[c], displayWidth(line));
  }
  std::ostringstream out;
  for (std::size_t r = 0; r < rows; ++r) {
    for (std::size_t c = 0; c < cols.size(); ++c) {
      if (c) out << std::string(gap, ' ');
      out << padRight(r < cols[c].size() ? cols[c][r] : "", widths[c]);
    }
    out << "\n";
  }
  return out.str();
}

std::string renderDashboard(const Theme& th, const TerminalMetrics& metrics, const KernelStatus& status) {
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  const int left = std::max(0, (metrics.columns - width) / 2);
  const std::string indent(left, ' ');
  const std::vector<std::pair<std::string, std::string>> logoLarge = {
      {"██████  ██████  ██████  ██████  ██████", "██████ ██████"},
      {"██      ██      ██  ██  ██  ██  ██    ", "██     ██    "},
      {"██████  ██      ██  ██  ██████  █████ ", "████   ██████"},
      {"    ██  ██      ██  ██  ██      ██    ", "██         ██"},
      {"██████  ██████  ██████  ██      ██████", "██     ██████"},
      {"  ░░░░    ░░░░    ░░░░  ░░        ░░░░", "░░       ░░░░"}};
  std::ostringstream out;
  if (th.ansi) out << th.bg;
  out << "\n";
  if (metrics.compact) {
    const auto scope = color(th, th.gray, "Scope");
    const auto fs = color(th, th.white, "FS");
    out << indent << center(scope + fs, width) << "\n";
  } else {
    for (const auto& [scope, fs] : logoLarge) {
      const auto plain = scope + "  " + fs;
      const int logoLeft = std::max(0, (width - displayWidth(plain)) / 2);
      out << indent << std::string(logoLeft, ' ')
          << color(th, scope.find("░") != std::string::npos ? th.dim : th.gray, scope)
          << "  " << color(th, fs.find("░") != std::string::npos ? th.dim : th.white, fs) << "\n";
    }
  }
  out << "\n";
  const int panelWidth = metrics.compact ? width : std::min(width - 8, 96);
  const int panelLeft = std::max(0, (metrics.columns - panelWidth) / 2);
  std::vector<std::string> command = {
      rawFg(th.amber, "/scope") + rawFg(th.gray, "  " + text(th, "command_surface")) + th.reset,
      rawFg(th.white, "$ ") + rawFg(th.gray, "format  ·  login root root  ·  scope tree  ·  map refcount") + th.reset,
      rawFg(th.blue, "Build") + rawFg(th.white, "  " + text(th, "observable_kernel") + " ") +
          rawFg(th.dim, text(th, "terminal_first")) + th.reset};
  out << indentBlock(box(th, text(th, "command_focus"), command, panelWidth, "amber"), panelLeft) << "\n\n";
  const int cardGap = 2;
  const int cardWidth = metrics.compact ? width : (width - 2 * cardGap) / 3;
  auto volume = box(th, text(th, "volume"), {
      text(th, "mount") + "  " + color(th, status.mountState == "clean" ? th.green : th.amber, status.mountState),
      color(th, th.white, txLabel(th, status.txid)),
      text(th, "blocks") + " " + progress(th, status.blocks, status.blockTotal, std::max(10, cardWidth - 18), "blue")}, cardWidth, "border");
  auto session = box(th, text(th, "session"), {
      text(th, "user") + "   " + color(th, th.white, status.user),
      text(th, "cwd") + "  " + color(th, th.gray, truncate(status.cwd, cardWidth - 12)),
      text(th, "open") + "   " + color(th, th.white, std::to_string(status.openFiles))}, cardWidth, "blue");
  auto obs = box(th, text(th, "observability"), {
      text(th, "trace") + "  " + color(th, status.trace ? th.green : th.red, status.trace ? "on" : "off"),
      text(th, "inode") + "  " + progress(th, status.inodes, status.inodeTotal, std::max(10, cardWidth - 18), "amber"),
      text(th, "snap") + "   " + color(th, th.magenta, std::to_string(status.snapshots))}, cardWidth, "magenta");
  if (metrics.compact) {
    out << indentBlock(volume, left) << "\n" << indentBlock(session, left) << "\n" << indentBlock(obs, left) << "\n";
  } else {
    out << indentBlock(columns({volume, session, obs}, cardGap), left) << "\n";
  }
  out << indent << color(th, th.dim, text(th, "tips") + "  " + text(th, "tab_commands") + "   " + text(th, "palette") + "   trace 20   trace <command>   fsck") << "\n\n";
  if (th.ansi) out << th.reset;
  return out.str();
}

PromptRender renderPromptLine(const Theme& th, const TerminalMetrics& metrics, const KernelStatus& status, const std::string& line, std::size_t cursor) {
  PromptRender rendered;
  const int width = std::min(metrics.columns, metrics.compact ? metrics.columns : 112);
  const int left = std::max(0, (metrics.columns - width) / 2);
  const bool loggedIn = !status.user.empty() && status.user != "-";
  const std::string plainPrefix = loggedIn
      ? truncate(status.cwd, metrics.compact ? 18 : (metrics.wide ? 34 : 24)) + " " +
            truncate(status.user, metrics.compact ? 10 : 14) + " $ "
      : "$ ";
  const int prefixWidth = displayWidth(plainPrefix);
  const int available = std::max(4, width - prefixWidth);

  std::size_t visibleStart = 0;
  if (cursor > static_cast<std::size_t>(available)) visibleStart = cursor - static_cast<std::size_t>(available);
  if (visibleStart > line.size()) visibleStart = line.size();
  const auto visible = line.substr(visibleStart, static_cast<std::size_t>(available));
  const auto beforeCursor = line.substr(visibleStart, cursor - visibleStart);
  const int commandWidth = displayWidth(visible);

  rendered.cursorColumn = left + prefixWidth + displayWidth(beforeCursor);
  rendered.visibleStart = visibleStart;
  rendered.commandWidth = commandWidth;

  if (!th.ansi || th.mono) {
    rendered.row = std::string(left, ' ') + plainPrefix + visible + std::string(std::max(0, available - commandWidth), ' ');
    return rendered;
  }

  std::string prefix = th.panel2;
  if (loggedIn) {
    const int cwdSlot = metrics.compact ? 18 : (metrics.wide ? 34 : 24);
    const int userSlot = metrics.compact ? 10 : 14;
    prefix += panelAccent(th, th.amber, truncate(status.cwd, cwdSlot)) + " " +
              panelAccent(th, th.blue, truncate(status.user, userSlot)) +
              th.white + th.bold + " $ " + th.normalWeight;
  } else {
    prefix += th.white + th.bold + "$ " + th.normalWeight;
  }
  rendered.row = std::string(left, ' ') + prefix + th.white + visible +
                 std::string(std::max(0, available - commandWidth), ' ') + th.reset;
  return rendered;
}

std::string renderPrompt(const Theme& th, const TerminalMetrics& metrics, const KernelStatus& status) {
  const auto prompt = renderPromptLine(th, metrics, status, "", 0);
  if (!th.ansi || th.mono) return prompt.row;
  std::ostringstream out;
  out << prompt.row << "\r";
  if (prompt.cursorColumn > 0) out << "\x1b[" << prompt.cursorColumn << "C";
  return out.str();
}

std::string renderResult(const Theme& th, const TerminalMetrics& metrics, const std::string& command, bool ok, const std::string& code, const std::string& message, const std::string& output) {
  std::ostringstream out;
  const auto tone = ok ? "green" : "red";
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  const int left = std::max(0, (metrics.columns - width) / 2);
  const std::string indent(left, ' ');
  if (th.ansi && !th.mono) out << th.reset << "\x1b[2K";
  out << indent << badge(th, ok ? "OK" : code, tone) << " "
      << color(th, th.dim, truncate(command, std::max(20, width - 28)));
  if (!ok) out << " " << color(th, th.red, message);
  if (th.ansi && !th.mono) out << th.reset << "\x1b[0K";
  out << "\n";
  if (!output.empty()) {
    for (const auto& line : splitLines(output)) {
      if (th.ansi && !th.mono) out << th.reset << "\x1b[2K";
      out << indent << line;
      if (th.ansi && !th.mono) out << th.reset << "\x1b[0K";
      out << "\n";
    }
  }
  return out.str();
}

std::string renderDir(const Theme& th, const TerminalMetrics& metrics, const std::string& path, const std::vector<DirRow>& rows) {
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  const bool narrow = width <= 96;
  const int nameWidth = narrow ? 10 : (metrics.wide ? 14 : 12);
  const int typeWidth = th.lang == "zh" ? 10 : 13;
  const int group1Width = th.lang == "zh" ? 25 : 29;
  const int group2Width = th.lang == "zh" ? 25 : 30;
  const int blockWidth = narrow ? 6 : (metrics.wide ? 12 : 10);
  const std::string groupGap = narrow ? "   " : "    ";
  const int nameCellWidth = nameWidth + 2;
  std::vector<std::string> list;
  list.push_back(accent(th, th.amber, truncate(path, width - 16)) + "  " + badge(th, std::to_string(rows.size()) + " " + text(th, "entries"), "blue"));
  bool firstRow = true;
  for (const auto& row : rows) {
    if (firstRow) firstRow = false;
    else list.push_back(kBoxRule);
    if (rows.size() > 0 && list.size() == 1) list.push_back(kBoxRule);
    const auto tone = typeTone(row.type);
    const auto nameTone = row.type == "dir" ? th.amber : (row.shared ? th.magenta : th.white);
    const auto nameCell = color(th, toneCode(th, tone), iconForType(row.type)) + " " +
                          accent(th, nameTone, padRight(row.name, nameWidth));
    const auto typeCell = color(th, th.white, padRight(dirTypeLabel(th, row.type), typeWidth));
    const auto emptyName = padRight("", nameCellWidth);
    const auto columnGap = "  ";
    const auto emptyType = padRight("", typeWidth);
    const auto meta1 = padRight(labelValue(th, text(th, "inode"), std::to_string(row.inode), "amber"), group1Width);
    const auto meta2 = padRight(labelValue(th, text(th, "gen"), std::to_string(row.generation)), group2Width);
    const auto meta3 = labelValue(th, text(th, "ref"), std::to_string(row.refcount), row.shared ? "magenta" : "white");
    const auto ownerClass = row.owner + ":" + row.klass;
    const auto blocks = progress(th, row.blockCount, 12, blockWidth, row.shared ? "magenta" : "blue");
    const auto detail1 = padRight(labelValue(th, text(th, "owner_group"), ownerClass), group1Width);
    const auto detail2 = padRight(labelValue(th, text(th, "size"), std::to_string(row.size) + "B"), group2Width);
    const auto detail3 = color(th, th.dim, text(th, "blocks") + " ") + blocks + " " + color(th, th.white, std::to_string(row.blockCount));
    const auto time1 = padRight(labelValue(th, text(th, "created"), row.createdAt.empty() ? "-" : row.createdAt), group1Width);
    const auto time2 = padRight(labelValue(th, text(th, "modified"), row.modifiedAt.empty() ? "-" : row.modifiedAt), group2Width);
    const auto summaryLine = nameCell + columnGap + typeCell + groupGap + meta1 + groupGap + meta2 + groupGap + meta3;
    const auto detailLine = emptyName + columnGap + color(th, th.white, padRight(row.mode, typeWidth)) + groupGap + detail1 + groupGap + detail2 + groupGap + detail3;
    if (narrow || displayWidth(summaryLine) > width - 4 || displayWidth(detailLine) > width - 4) {
      list.push_back(nameCell + columnGap + typeCell + groupGap + meta1);
      list.push_back(emptyName + columnGap + color(th, th.white, padRight(row.mode, typeWidth)) + groupGap + detail1);
      list.push_back(emptyName + columnGap + emptyType + groupGap + meta2 + groupGap + meta3);
      list.push_back(emptyName + columnGap + emptyType + groupGap + detail2 + groupGap + detail3);
      list.push_back(emptyName + columnGap + emptyType + groupGap + labelValue(th, text(th, "created"), row.createdAt.empty() ? "-" : row.createdAt));
      list.push_back(emptyName + columnGap + emptyType + groupGap + labelValue(th, text(th, "modified"), row.modifiedAt.empty() ? "-" : row.modifiedAt));
    } else {
      list.push_back(summaryLine);
      list.push_back(detailLine);
      const auto timeLine = emptyName + columnGap + emptyType + groupGap + time1 + groupGap + time2;
      if (displayWidth(timeLine) <= width - 4) {
        list.push_back(timeLine);
      } else {
        list.push_back(emptyName + columnGap + emptyType + groupGap + time1);
        list.push_back(emptyName + columnGap + emptyType + groupGap + time2);
      }
    }
  }
  return box(th, text(th, "directory"), list, width, "amber") + "\n";
}

std::string renderScope(const Theme& th, const TerminalMetrics& metrics, const KernelStatus& status, const std::vector<InodeRow>& inodeHot, const std::vector<std::pair<std::string, std::string>>& extra) {
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  const int gap = 2;
  const int card = metrics.compact ? width : (width - gap) / 2;
  auto a = box(th, text(th, "kernel"), {
      text(th, "mount") + "  " + color(th, status.mountState == "clean" ? th.green : th.amber, status.mountState),
      color(th, th.white, txLabel(th, status.txid)),
      "root   " + color(th, th.gray, status.mounted ? "visible" : "unmounted")}, card, "amber");
  auto b = box(th, text(th, "capacity"), {
      text(th, "inode") + "  " + progress(th, status.inodes, status.inodeTotal, std::max(12, card - 18), "amber") + " " + std::to_string(status.inodes),
      text(th, "blocks") + "  " + progress(th, status.blocks, status.blockTotal, std::max(12, card - 18), "blue") + " " + std::to_string(status.blocks),
      text(th, "snap") + "   " + color(th, th.magenta, std::to_string(status.snapshots))}, card, "blue");
  std::vector<std::string> hot;
  for (const auto& row : inodeHot) {
    hot.push_back(color(th, th.amber, "#" + std::to_string(row.inode)) + " " + padRight(row.type, 5) +
                  " " + labelValue(th, th.lang == "zh" ? "引用数" : "reference count", std::to_string(row.refcount), row.refcount > 1 ? "magenta" : "gray") +
                  "   " + labelValue(th, th.lang == "zh" ? "打开数" : "open count", std::to_string(row.openCount)) +
                  " " + truncate(row.owner + ":" + row.klass, card - 44));
  }
  if (hot.empty()) hot.push_back(color(th, th.dim, text(th, "no_inode_activity")));
  auto c = box(th, text(th, "hot_inodes"), hot, card, "magenta");
  std::vector<std::string> misc;
  for (const auto& item : extra) misc.push_back(color(th, th.gray, extraLabel(th, item.first)) + "  " + color(th, th.white, item.second));
  if (misc.empty()) misc.push_back(text(th, "trace") + "  " + color(th, status.trace ? th.green : th.red, status.trace ? "on" : "off"));
  auto d = box(th, text(th, "session"), misc, card, "border");
  if (metrics.compact) return a + "\n" + b + "\n" + c + "\n" + d + "\n";
  return columns({a, b}, gap) + columns({c, d}, gap);
}

std::string renderTree(const Theme& th, const TerminalMetrics& metrics, const std::vector<std::string>& lines) {
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  std::vector<std::string> out;
  for (auto line : lines) {
    std::string tone = line.find("ref=2") != std::string::npos || line.find("ref=3") != std::string::npos ? "magenta" : "gray";
    out.push_back(color(th, toneCode(th, tone), truncate(line, width - 4)));
  }
  return box(th, text(th, "tree_shared"), out, width, "blue") + "\n";
}

std::string renderMap(const Theme& th, const TerminalMetrics& metrics, const std::string& what, const std::vector<MapCell>& cells, std::uint32_t totalBlocks) {
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  const int mapWidth = std::max(48, width - 8);
  const int rows = metrics.compact ? 12 : 18;
  const int totalCells = mapWidth * rows;
  const int stride = std::max<int>(1, static_cast<int>(totalBlocks) / totalCells);
  std::map<std::uint32_t, MapCell> byBlock;
  for (const auto& c : cells) byBlock[c.block] = c;
  {
    std::vector<std::string> rendered;
    rendered.push_back(color(th, th.dim, "0") + std::string(std::max(0, mapWidth - 12), ' ') + color(th, th.dim, std::to_string(totalBlocks)));
    for (int r = 0; r < rows; ++r) {
      std::string row;
      for (int c = 0; c < mapWidth; ++c) {
        const auto block = static_cast<std::uint32_t>((r * mapWidth + c) * stride);
        const auto it = byBlock.find(block);
        MapCell cell;
        if (it != byBlock.end()) cell = it->second;
        else cell.block = block;
        row += color(th, toneCode(th, mapTone(th, what, cell)), mapGlyph(what, cell));
      }
      rendered.push_back(row);
    }
    rendered.push_back(mapLegend(th, what));

    std::vector<std::string> highlights;
    if (what == "journal") {
      for (const auto& c : cells) {
        if (c.lastWriterTxid == 0) continue;
        highlights.push_back("block " + color(th, th.amber, std::to_string(c.block)) +
                             " " + txLabel(th, c.lastWriterTxid) +
                             " owner=#" + std::to_string(c.ownerInode));
        if (highlights.size() >= 4) break;
      }
      if (!highlights.empty()) rendered.push_back(color(th, th.amber, th.lang == "zh" ? "事务写入块" : "transaction-written blocks"));
    } else {
      for (const auto& c : cells) {
        if (c.refcount <= 1) continue;
        highlights.push_back("block " + color(th, th.magenta, std::to_string(c.block)) +
                             " ref=" + std::to_string(c.refcount) +
                             " owner=#" + std::to_string(c.ownerInode));
        if (highlights.size() >= 4) break;
      }
      if (!highlights.empty()) rendered.push_back(color(th, th.magenta, "shared hotspots"));
    }
    rendered.insert(rendered.end(), highlights.begin(), highlights.end());
    return box(th, text(th, "disk_map") + " / " + mapWhatLabel(th, what), rendered, width, what == "journal" ? "green" : (what == "inode" ? "blue" : "amber")) + "\n";
  }
}

std::string renderTraceTimeline(const Theme& th, const TerminalMetrics& metrics, const std::vector<TraceEvent>& events, const std::string& title) {
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  std::vector<std::string> lines;
  if (!events.empty()) {
    const auto command = events.front().command;
    const bool singleCommand = std::all_of(events.begin(), events.end(), [&](const TraceEvent& e) {
      return e.command == command;
    });
    if (singleCommand && !command.empty() && command != "-") {
      lines.push_back(color(th, th.dim, th.lang == "zh" ? "命令 " : "command ") +
                      color(th, th.white, truncate(command, width - 12)));
      lines.push_back(kBoxRule);
    }
  }

  struct TraceNode {
    const TraceEvent* event = nullptr;
    int depth = 0;
  };
  struct LockFrame {
    std::string object;
    int depth = 0;
  };
  struct PathFrame {
    std::string path;
    int depth = 0;
  };

  std::vector<TraceNode> nodes;
  const bool structuredTrace = std::any_of(events.begin(), events.end(), [](const TraceEvent& e) {
    return e.parentSeq != 0 || e.depth != 0;
  });

  if (structuredTrace) {
    std::map<std::uint64_t, int> displayDepth;
    for (const auto& e : events) {
      int depth = 0;
      const auto parent = displayDepth.find(e.parentSeq);
      if (e.parentSeq != 0 && parent != displayDepth.end()) depth = parent->second + 1;
      displayDepth[e.seq] = depth;
      nodes.push_back({&e, depth});
    }
  } else {
    std::vector<LockFrame> locks;
    std::vector<PathFrame> paths;
    std::uint64_t activeTxid = 0;
    int activeTxDepth = -1;
    int activeAuthDepth = -1;
    int activeCowDepth = -1;
    int activeCheckpointDepth = -1;

    auto childOfActiveTx = [&]() {
      return activeTxid == 0 ? 0 : activeTxDepth + 1;
    };

    for (const auto& e : events) {
      int depth = 0;

      if (activeTxid != 0 && !startsWithText(e.type, "coord.lock.")) {
        depth = std::max(depth, childOfActiveTx());
      }
      if (activeCheckpointDepth >= 0 && (e.type == "journal.clear" || e.type == "coord.epoch.bump")) {
        depth = std::max(depth, activeCheckpointDepth + 1);
      }

      if (e.type == "coord.lock.acquire") {
        depth = locks.empty() ? 0 : locks.back().depth + 1;
        if (e.object == "mutex") locks.push_back({e.object, depth});
        paths.clear();
        activeAuthDepth = -1;
      } else if (e.type == "coord.lock.release") {
        if (e.object == "mutex") {
          for (std::size_t idx = locks.size(); idx > 0; --idx) {
            if (locks[idx - 1].object == e.object) {
              depth = locks[idx - 1].depth + 1;
              locks.erase(locks.begin() + static_cast<std::ptrdiff_t>(idx - 1));
              break;
            }
          }
        }
        if (e.object == "tx") {
          activeTxid = 0;
          activeTxDepth = -1;
          activeCowDepth = -1;
          activeCheckpointDepth = -1;
        }
      } else if (e.type == "journal.begin" && e.txid != 0) {
        activeTxid = e.txid;
        activeTxDepth = depth;
        activeCowDepth = -1;
        activeCheckpointDepth = -1;
        paths.clear();
        activeAuthDepth = -1;
      } else if (e.type == "journal.checkpoint" && e.txid == activeTxid) {
        activeCheckpointDepth = depth;
        activeCowDepth = -1;
      } else if (e.type == "path.lookup") {
        const int baseDepth = depth;
        while (!paths.empty() && !pathChildOf(paths.back().path, e.object)) paths.pop_back();
        if (!paths.empty()) depth = std::max(depth, paths.back().depth + 1);
        else depth = baseDepth;
        paths.push_back({e.object, depth});
        activeAuthDepth = -1;
        activeCowDepth = -1;
      } else if (e.type == "auth.check") {
        for (auto it = paths.rbegin(); it != paths.rend(); ++it) {
          if (it->path == e.object || pathChildOf(it->path, e.object)) {
            depth = std::max(depth, it->depth + 1);
            break;
          }
        }
        activeAuthDepth = depth;
      } else if (startsWithText(e.type, "cow.")) {
        if (activeTxid != 0 && e.txid == activeTxid) depth = std::max(depth, activeTxDepth + 1);
        activeCowDepth = depth;
      } else if (traceStorageEvent(e)) {
        if (activeCowDepth >= 0 && (activeTxid == 0 || e.txid == 0 || e.txid == activeTxid)) {
          depth = std::max(depth, activeCowDepth + 1);
        }
      } else if (traceOperationEvent(e)) {
        if (activeTxid == 0 && activeAuthDepth >= 0) depth = std::max(depth, activeAuthDepth + 1);
        activeCowDepth = -1;
      } else if (e.type == "journal.commit") {
        activeCowDepth = -1;
      }

      nodes.push_back({&e, depth});
    }
  }

  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const auto& e = *nodes[i].event;
    const int depth = nodes[i].depth;
    const bool failed = e.status == "deny" || e.status == "error" || e.status == "crash" || e.status == "busy";
    const auto tone = failed ? std::string("red") : eventTone(e);
    auto hasLaterAtDepth = [&](int targetDepth) {
      for (std::size_t j = i + 1; j < nodes.size(); ++j) {
        if (nodes[j].depth < targetDepth) break;
        if (nodes[j].depth == targetDepth) return true;
      }
      return false;
    };
    std::string branchPrefix;
    const auto marker = std::string("●");
    if (depth == 0) {
      branchPrefix.clear();
    } else {
      for (int d = 1; d < depth; ++d) branchPrefix += hasLaterAtDepth(d) ? "│ " : "  ";
      branchPrefix += hasLaterAtDepth(depth) ? "├─" : "└─";
    }

    const auto tx = e.txid > 0 ? color(th, th.amber, txLabel(th, e.txid)) + " " : "";
    const auto object = (!e.object.empty() && e.object != "-") ? traceObjectLabel(th, e.object) : "";
    const auto objectPart = object.empty() ? "" : color(th, th.gray, truncate(object, std::max(8, width - 62)));
    std::ostringstream line;
    line << color(th, th.dim, branchPrefix)
         << color(th, toneCode(th, tone), marker) << " "
         << color(th, th.dim, "#" + std::to_string(e.seq)) << " "
         << tx
         << color(th, th.white, truncate(traceTypeLabel(th, e.type), 24));
    if (!objectPart.empty()) line << " " << objectPart;
    line << " " << badge(th, e.status, statusTone(e.status));
    lines.push_back(line.str());
  }
  if (lines.empty()) lines.push_back(color(th, th.dim, th.lang == "zh" ? "暂无跟踪事件" : "no trace events"));
  std::string translatedTitle = title;
  if (title == "Trace timeline") translatedTitle = text(th, "trace_timeline");
  else if (title == "Trace replay / read-only") translatedTitle = text(th, "trace_replay");
  else if (title == "Trace step") translatedTitle = text(th, "trace_step");
  else if (!title.empty()) translatedTitle = text(th, "trace_timeline") + " / " + truncate(title, 40);
  return box(th, translatedTitle, lines, width, "blue") + "\n";
}

std::string renderReadData(const Theme& th, const TerminalMetrics& metrics, const std::string& data, std::uint64_t oldOffset, std::uint64_t newOffset) {
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  const int dataWidth = std::max(24, width - 8);
  std::string shown = truncate(data, dataWidth);
  const auto advanced = newOffset >= oldOffset ? newOffset - oldOffset : 0;
  std::string ruler(dataWidth, ' ');
  for (int i = 0; i < dataWidth; i += 8) {
    const auto tick = std::to_string(oldOffset + static_cast<std::uint64_t>(i));
    for (std::size_t j = 0; j < tick.size() && i + static_cast<int>(j) < dataWidth; ++j) ruler[i + j] = tick[j];
  }
  std::string pointer(dataWidth, ' ');
  const auto pos = static_cast<int>(std::min<std::uint64_t>(advanced, dataWidth ? dataWidth - 1 : 0));
  if (pos >= 0 && pos < dataWidth) pointer[pos] = '^';
  return box(th, text(th, "read_result"), {
      color(th, th.dim, ruler),
      color(th, th.white, padRight(shown, dataWidth)),
      color(th, th.blue, pointer),
      color(th, th.gray, text(th, "read_advance") + " +" + std::to_string(advanced) +
          "  " + text(th, "fd_offset") + " " + std::to_string(oldOffset) + " -> " + std::to_string(newOffset))}, width, "blue") + "\n";
}

std::string renderSnapshotDiff(const Theme& th, const TerminalMetrics& metrics, const std::vector<std::string>& diffLines) {
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  std::vector<std::string> lines;
  for (const auto& raw : diffLines) {
    std::string tone = "gray";
    if (!raw.empty() && raw[0] == '+') tone = "green";
    else if (!raw.empty() && raw[0] == '-') tone = "red";
    else if (!raw.empty() && raw[0] == '~') tone = "amber";
    lines.push_back(color(th, toneCode(th, tone), truncate(raw, width - 4)) + (tone == "amber" ? color(th, th.magenta, "  COW") : ""));
  }
  if (lines.empty()) lines.push_back(color(th, th.green, th.lang == "zh" ? "无差异" : "no differences"));
  return box(th, text(th, "snapshot_diff"), lines, width, "magenta") + "\n";
}

std::string renderClassGraph(const Theme& th, const TerminalMetrics& metrics, const std::vector<std::string>& linesIn) {
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  std::vector<std::string> lines;
  for (auto line : linesIn) {
    std::string tone = line.find("inherits") != std::string::npos ? "magenta" : "amber";
    lines.push_back(color(th, toneCode(th, tone), "◇ ") + truncate(line, width - 6));
  }
  return box(th, text(th, "class_graph"), lines, width, "magenta") + "\n";
}

std::string renderAclGraph(const Theme& th, const TerminalMetrics& metrics, const std::string& title, const std::vector<std::string>& linesIn) {
  const int width = std::min(metrics.columns, metrics.wide ? 132 : 112);
  std::vector<std::string> lines;
  for (auto line : linesIn) {
    std::string tone = line.find("rights=") != std::string::npos ? "blue" : "gray";
    lines.push_back(color(th, toneCode(th, tone), "● ") + truncate(line, width - 6));
  }
  if (lines.empty()) lines.push_back(color(th, th.dim, th.lang == "zh" ? "暂无 ACL grant" : "no ACL grants"));
  return box(th, title == "ACL graph" ? text(th, "acl_graph") : title, lines, width, "blue") + "\n";
}

} // namespace scopefs::ui
