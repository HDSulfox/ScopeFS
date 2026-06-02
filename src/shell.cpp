#include "scopefs/shell.hpp"

#include "scopefs/util.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

#if defined(_WIN32)
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace scopefs {

namespace {

enum class EditorKey {
  Character,
  Enter,
  Backspace,
  Tab,
  Left,
  Right,
  Up,
  Down,
  Delete,
  CtrlC,
  CtrlD,
  Unknown
};

struct KeyPress {
  EditorKey key = EditorKey::Unknown;
  char ch = 0;
};

#if !defined(_WIN32)
class RawTerminalMode {
 public:
  RawTerminalMode() {
    if (!isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &old_) != 0) return;
    termios raw = old_;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) active_ = true;
  }

  ~RawTerminalMode() {
    if (active_) tcsetattr(STDIN_FILENO, TCSANOW, &old_);
  }

 private:
  termios old_{};
  bool active_ = false;
};
#endif

KeyPress readEditorKey() {
#if defined(_WIN32)
  const int ch = _getch();
  if (ch == 0 || ch == 224) {
    const int ext = _getch();
    if (ext == 72) return {EditorKey::Up, 0};
    if (ext == 80) return {EditorKey::Down, 0};
    if (ext == 75) return {EditorKey::Left, 0};
    if (ext == 77) return {EditorKey::Right, 0};
    if (ext == 83) return {EditorKey::Delete, 0};
    return {EditorKey::Unknown, 0};
  }
  if (ch == 27) {
    const int a = _getch();
    if (a == '[') {
      const int b = _getch();
      if (b == 'A') return {EditorKey::Up, 0};
      if (b == 'B') return {EditorKey::Down, 0};
      if (b == 'C') return {EditorKey::Right, 0};
      if (b == 'D') return {EditorKey::Left, 0};
      if (b == '3') {
        (void)_getch();
        return {EditorKey::Delete, 0};
      }
    }
    return {EditorKey::Unknown, 0};
  }
  if (ch == '\r' || ch == '\n') return {EditorKey::Enter, 0};
  if (ch == '\b' || ch == 127) return {EditorKey::Backspace, 0};
  if (ch == '\t') return {EditorKey::Tab, 0};
  if (ch == 3) return {EditorKey::CtrlC, 0};
  if (ch == 4) return {EditorKey::CtrlD, 0};
  if (ch >= 32) return {EditorKey::Character, static_cast<char>(ch)};
  return {EditorKey::Unknown, 0};
#else
  char ch = 0;
  if (read(STDIN_FILENO, &ch, 1) != 1) return {EditorKey::CtrlD, 0};
  if (ch == '\r' || ch == '\n') return {EditorKey::Enter, 0};
  if (ch == 127 || ch == '\b') return {EditorKey::Backspace, 0};
  if (ch == '\t') return {EditorKey::Tab, 0};
  if (ch == 3) return {EditorKey::CtrlC, 0};
  if (ch == 4) return {EditorKey::CtrlD, 0};
  if (ch == 27) {
    char seq[3] = {};
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return {EditorKey::Unknown, 0};
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return {EditorKey::Unknown, 0};
    if (seq[0] == '[') {
      if (seq[1] == 'A') return {EditorKey::Up, 0};
      if (seq[1] == 'B') return {EditorKey::Down, 0};
      if (seq[1] == 'C') return {EditorKey::Right, 0};
      if (seq[1] == 'D') return {EditorKey::Left, 0};
      if (seq[1] == '3') {
        (void)read(STDIN_FILENO, &seq[2], 1);
        return {EditorKey::Delete, 0};
      }
    }
    return {EditorKey::Unknown, 0};
  }
  if (static_cast<unsigned char>(ch) >= 32) return {EditorKey::Character, ch};
  return {EditorKey::Unknown, 0};
#endif
}

bool isPathCommand(const std::string& cmd, std::size_t tokenIndex, const std::vector<std::string>& prior) {
  static const std::set<std::string> firstPath = {
      "cd", "chdir", "dir", "ls", "mkdir", "rmdir", "create", "open", "delete", "rm", "truncate", "chmod", "chown", "chclass"};
  if (firstPath.count(cmd) && tokenIndex == 1) return true;
  if (cmd == "clone" && (tokenIndex == 1 || tokenIndex == 2)) return true;
  if (cmd == "acl" && prior.size() >= 2 && tokenIndex == 2) return true;
  return false;
}

bool wantsDirectoryOnly(const std::string& cmd) {
  return cmd == "cd" || cmd == "chdir" || cmd == "dir" || cmd == "ls" || cmd == "mkdir" || cmd == "rmdir";
}

} // namespace

std::vector<std::string> tokenize(const std::string& line) {
  std::vector<std::string> out;
  std::string current;
  bool quote = false;
  char quoteChar = 0;
  bool escape = false;
  for (char ch : line) {
    if (escape) {
      current.push_back(ch);
      escape = false;
      continue;
    }
    if (ch == '\\') {
      escape = true;
      continue;
    }
    if (quote) {
      if (ch == quoteChar) {
        quote = false;
      } else {
        current.push_back(ch);
      }
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = true;
      quoteChar = ch;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(ch))) {
      if (!current.empty()) {
        out.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(ch);
  }
  if (!current.empty()) out.push_back(current);
  return out;
}

Shell::Shell(FileSystemKernel& kernel) : kernel_(kernel) {}

int Shell::run(std::istream& in, std::ostream& out, bool interactive, const TerminalCaps& caps) {
  kernel_.setInteractiveUi(interactive, caps.ansi);
  if (interactive) banner(out, caps);
  std::string line;
  while (true) {
    const bool useEditor = interactive && caps.inputTty;
    if (useEditor) {
      line = readInteractiveLine(out, caps);
    } else if (interactive) {
      out << ui::renderPrompt(ui::theme(kernel_.uiAnsiEnabled(), kernel_.uiThemeName(), kernel_.uiLanguageName()), ui::detectMetrics(), kernel_.status());
      out.flush();
      if (!std::getline(in, line)) break;
      if (interactive && kernel_.uiAnsiEnabled()) {
        out << ui::theme(true, kernel_.uiThemeName(), kernel_.uiLanguageName()).reset;
      }
    } else {
      if (!std::getline(in, line)) break;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() >= 3 &&
        static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB &&
        static_cast<unsigned char>(line[2]) == 0xBF) {
      line.erase(0, 3);
    }
    auto args = tokenize(line);
    if (args.empty()) continue;
    const auto cmd = args[0];
    if (cmd == "exit" || cmd == "quit") break;
    if (interactive && cmd == "login" && args.size() == 2) {
      out << "password: ";
      out.flush();
      args.push_back(readPassword());
      out << "\n";
    }
    auto result = kernel_.execute(args, line);
    if (interactive) {
      out << ui::renderResult(ui::theme(kernel_.uiAnsiEnabled(), kernel_.uiThemeName(), kernel_.uiLanguageName()), ui::detectMetrics(), line, result.code == ErrorCode::Ok,
                              errorCodeName(result.code), result.message, result.output);
    } else {
      out << result;
    }
  }
  return 0;
}

std::string Shell::readInteractiveLine(std::ostream& out, const TerminalCaps& caps) {
  (void)caps;
#if !defined(_WIN32)
  RawTerminalMode raw;
#endif
  std::string line;
  std::string draft;
  std::size_t cursor = 0;
  std::size_t historyIndex = history_.size();
  std::vector<std::string> tabCandidates;
  std::string tabPrefixLine;
  std::size_t tabStart = 0;
  std::size_t tabIndex = 0;
  redrawInteractiveLine(out, line, cursor);
  while (true) {
    const auto key = readEditorKey();
    bool redraw = true;
    if (key.key != EditorKey::Tab) tabCandidates.clear();
    switch (key.key) {
      case EditorKey::Enter:
        redrawInteractiveLine(out, line, line.size());
        if (kernel_.uiAnsiEnabled()) out << ui::theme(true, kernel_.uiThemeName(), kernel_.uiLanguageName()).reset;
        out << "\n";
        if (!line.empty() && (history_.empty() || history_.back() != line)) {
          history_.push_back(line);
          if (history_.size() > 200) history_.erase(history_.begin());
        }
        return line;
      case EditorKey::CtrlC:
        line.clear();
        cursor = 0;
        redrawInteractiveLine(out, line, cursor);
        if (kernel_.uiAnsiEnabled()) out << ui::theme(true, kernel_.uiThemeName(), kernel_.uiLanguageName()).reset;
        out << "^C\n";
        return "";
      case EditorKey::CtrlD:
        if (line.empty()) {
          if (kernel_.uiAnsiEnabled()) out << ui::theme(true, kernel_.uiThemeName(), kernel_.uiLanguageName()).reset;
          out << "\n";
          return "exit";
        }
        redraw = false;
        break;
      case EditorKey::Backspace:
        if (cursor > 0) {
          line.erase(cursor - 1, 1);
          --cursor;
        }
        break;
      case EditorKey::Delete:
        if (cursor < line.size()) line.erase(cursor, 1);
        break;
      case EditorKey::Left:
        if (cursor > 0) {
          const auto oldCursor = cursor;
          --cursor;
          if (moveInteractiveCursor(out, line, oldCursor, cursor)) redraw = false;
        }
        break;
      case EditorKey::Right:
        if (cursor < line.size()) {
          const auto oldCursor = cursor;
          ++cursor;
          if (moveInteractiveCursor(out, line, oldCursor, cursor)) redraw = false;
        }
        break;
      case EditorKey::Up:
        if (!history_.empty() && historyIndex > 0) {
          if (historyIndex == history_.size()) draft = line;
          --historyIndex;
          line = history_[historyIndex];
          cursor = line.size();
        }
        break;
      case EditorKey::Down:
        if (!history_.empty() && historyIndex < history_.size()) {
          ++historyIndex;
          line = historyIndex == history_.size() ? draft : history_[historyIndex];
          cursor = line.size();
        }
        break;
      case EditorKey::Tab: {
        std::size_t tokenStart = cursor;
        const bool sameTabContext = !tabCandidates.empty() && tabStart <= line.size() && line.substr(0, tabStart) == tabPrefixLine;
        if (!sameTabContext) {
          tabCandidates = completionCandidates(line, cursor, &tokenStart);
          tabStart = tokenStart;
          tabPrefixLine = line.substr(0, tokenStart);
          tabIndex = 0;
        } else {
          tabIndex = (tabIndex + 1) % tabCandidates.size();
        }
        if (tabCandidates.empty()) {
          redraw = false;
          break;
        }
        const auto replacement = tabCandidates[tabIndex % tabCandidates.size()];
        line.replace(tabStart, cursor - tabStart, replacement);
        cursor = tabStart + replacement.size();
        break;
      }
      case EditorKey::Character:
        line.insert(cursor, 1, key.ch);
        ++cursor;
        break;
      case EditorKey::Unknown:
        redraw = false;
        break;
    }
    if (redraw) redrawInteractiveLine(out, line, cursor);
  }
}

void Shell::redrawInteractiveLine(std::ostream& out, const std::string& line, std::size_t cursor) const {
  const auto metrics = ui::detectMetrics();
  const auto status = kernel_.status();
  const auto th = ui::theme(kernel_.uiAnsiEnabled(), kernel_.uiThemeName(), kernel_.uiLanguageName());
  const auto prompt = ui::renderPromptLine(th, metrics, status, line, cursor);

  if (!th.ansi || th.mono) {
    out << "\r" << prompt.row << "  ";
    out.flush();
    return;
  }

  out << "\x1b[?25l\r" << prompt.row << "\r";
  if (prompt.cursorColumn > 0) out << "\x1b[" << prompt.cursorColumn << "C";
  out << "\x1b[?25h";
  out.flush();
}

bool Shell::moveInteractiveCursor(std::ostream& out, const std::string& line, std::size_t oldCursor, std::size_t newCursor) const {
  const auto metrics = ui::detectMetrics();
  const auto status = kernel_.status();
  const auto th = ui::theme(kernel_.uiAnsiEnabled(), kernel_.uiThemeName(), kernel_.uiLanguageName());
  if (!th.ansi || th.mono) return false;
  const auto oldPrompt = ui::renderPromptLine(th, metrics, status, line, oldCursor);
  const auto newPrompt = ui::renderPromptLine(th, metrics, status, line, newCursor);
  if (oldPrompt.visibleStart != newPrompt.visibleStart) return false;
  out << "\r";
  if (newPrompt.cursorColumn > 0) out << "\x1b[" << newPrompt.cursorColumn << "C";
  out.flush();
  return true;
}

std::vector<std::string> Shell::completionCandidates(const std::string& line, std::size_t cursor, std::size_t* tokenStart) const {
  const auto before = line.substr(0, cursor);
  const auto splitAt = before.find_last_of(" \t");
  const std::size_t start = splitAt == std::string::npos ? 0 : splitAt + 1;
  if (tokenStart) *tokenStart = start;
  const auto token = before.substr(start);
  const auto prior = tokenize(before.substr(0, start));

  static const std::vector<std::string> commands = {
      "login", "logout", "whoami", "format", "mkdir", "rmdir", "chdir", "cd", "dir", "ls",
      "create", "open", "read", "write", "close", "delete", "rm", "truncate", "trace", "scope",
      "map", "snapshot", "clone", "class", "chmod", "chown", "chclass", "acl", "fsck", "crash",
      "theme", "lang", "help", "exit"};
  static const std::map<std::string, std::vector<std::string>> subcommands = {
      {"map", {"blocks", "inode", "journal", "refcount", "owner"}},
      {"scope", {"inode", "block", "journal", "open", "tree"}},
      {"trace", {"on", "off", "show", "save", "replay", "step", "clear"}},
      {"snapshot", {"create", "list", "show", "diff", "rollback", "delete"}},
      {"class", {"create", "grant", "revoke", "list", "tree"}},
      {"acl", {"show", "grant", "revoke"}},
      {"crash", {"now", "after", "before", "at", "clear"}},
      {"theme", {"scope-dark", "blue", "mono"}},
      {"lang", {"zh", "en"}},
      {"open", {"r", "w", "rw", "append", "truncate"}},
      {"fsck", {"--repair"}}};

  std::vector<std::string> matches;
  auto addMatches = [&](const std::vector<std::string>& values) {
    for (const auto& value : values) {
      if (value.rfind(token, 0) == 0) matches.push_back(value);
    }
  };

  if (prior.empty()) {
    addMatches(commands);
    return matches;
  }

  const auto cmd = lower(prior.front());
  const std::size_t tokenIndex = prior.size();
  const auto subIt = subcommands.find(cmd);
  if (subIt != subcommands.end() && tokenIndex == 1) {
    addMatches(subIt->second);
    return matches;
  }

  if (isPathCommand(cmd, tokenIndex, prior)) {
    return kernel_.completePath(token, wantsDirectoryOnly(cmd));
  }

  return matches;
}

void Shell::banner(std::ostream& out, const TerminalCaps& caps) const {
  clearInteractiveScreen(out, caps);
  out << ui::renderDashboard(ui::theme(caps.ansi, kernel_.uiThemeName(), kernel_.uiLanguageName()), ui::detectMetrics(), kernel_.status());
}

std::string Shell::readPassword() {
  std::string password;
#if defined(_WIN32)
  while (true) {
    const int ch = _getch();
    if (ch == '\r' || ch == '\n') break;
    if (ch == '\b') {
      if (!password.empty()) password.pop_back();
      continue;
    }
    if (ch == 3) break;
    password.push_back(static_cast<char>(ch));
  }
#else
  termios oldt{};
  tcgetattr(STDIN_FILENO, &oldt);
  termios newt = oldt;
  newt.c_lflag &= ~ECHO;
  tcsetattr(STDIN_FILENO, TCSANOW, &newt);
  std::getline(std::cin, password);
  tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
#endif
  return password;
}

} // namespace scopefs
