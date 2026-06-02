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
  kernel_.setInteractiveUi(interactive, caps.ansi);
  if (interactive) banner(out, caps);
  std::string line;
  while (true) {
    if (interactive) {
      out << ui::renderPrompt(ui::theme(kernel_.uiAnsiEnabled(), kernel_.uiThemeName()), ui::detectMetrics(), kernel_.status());
      out.flush();
    }
    if (!std::getline(in, line)) break;
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
      out << ui::renderResult(ui::theme(kernel_.uiAnsiEnabled(), kernel_.uiThemeName()), ui::detectMetrics(), line, result.code == ErrorCode::Ok,
                              errorCodeName(result.code), result.message, result.output);
    } else {
      out << result;
    }
  }
  return 0;
}

void Shell::banner(std::ostream& out, const TerminalCaps& caps) const {
  clearInteractiveScreen(out, caps);
  out << ui::renderDashboard(ui::theme(caps.ansi), ui::detectMetrics(), kernel_.status());
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
