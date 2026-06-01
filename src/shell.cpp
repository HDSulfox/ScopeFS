#include "scopefs/shell.hpp"

#include <cctype>
#include <iostream>
#include <sstream>

#if defined(_WIN32)
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace scopefs {

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
  if (interactive) banner(out, caps);
  std::string line;
  while (true) {
    if (interactive) {
      out << kernel_.prompt();
      out.flush();
    }
    if (!std::getline(in, line)) break;
    if (!line.empty() && line.back() == '\r') line.pop_back();
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
    out << result;
    if (interactive && result.code == ErrorCode::Ok && !result.output.empty() &&
        line.rfind("trace show", 0) != 0 && line.rfind("scope", 0) != 0) {
      auto summary = kernel_.execute({"trace", "show", "5"}, "trace show 5");
      out << summary.output;
    }
  }
  return 0;
}

void Shell::banner(std::ostream& out, const TerminalCaps& caps) const {
  clearInteractiveScreen(out, caps);
  if (caps.ansi) {
    out << "\x1b[48;2;18;18;20m\x1b[38;2;224;224;224m";
  }
  out << "                                                                                \n";
  out << "        ███████╗ ██████╗ ██████╗ ██████╗ ███████╗███████╗███████╗             \n";
  out << "        ██╔════╝██╔════╝██╔═══██╗██╔══██╗██╔════╝██╔════╝██╔════╝             \n";
  out << "        ███████╗██║     ██║   ██║██████╔╝█████╗  █████╗  ███████╗             \n";
  out << "        ╚════██║██║     ██║   ██║██╔═══╝ ██╔══╝  ██╔══╝  ╚════██║             \n";
  out << "        ███████║╚██████╗╚██████╔╝██║     ███████╗██║     ███████║             \n";
  out << "        ╚══════╝ ╚═════╝ ╚═════╝ ╚═╝     ╚══════╝╚═╝     ╚══════╝             \n";
  out << "                                                                                \n";
  if (caps.ansi) {
    out << "\x1b[48;2;35;35;38m\x1b[38;2;235;235;235m";
  }
  out << "  ╭─ ScopeFS Workbench ─────────────────────────────────────────────────────╮  \n";
  out << "  │  observable C++17 teaching file system: inode · journal · COW · ACL     │  \n";
  out << "  │  try: format  →  login root root  →  qa/demo.scope in --script mode     │  \n";
  out << "  ╰─────────────────────────────────────────────────────────────────────────╯  \n";
  if (caps.ansi) {
    out << "\x1b[48;2;24;24;26m\x1b[38;2;190;190;190m";
  }
  out << "                                                                                \n";
  out << "  command input is below; outputs render as Unicode panels and heatmaps.        \n";
  out << "                                                                                \n";
  if (caps.ansi) out << "\x1b[0m";
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
