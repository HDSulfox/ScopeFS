#include "scopefs/terminal.hpp"

#include <clocale>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace scopefs {

TerminalCaps initTerminal(bool interactive) {
  TerminalCaps caps;
  std::setlocale(LC_ALL, ".UTF-8");
#if defined(_WIN32)
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
  if (interactive) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != INVALID_HANDLE_VALUE) {
      DWORD mode = 0;
      if (GetConsoleMode(out, &mode)) {
        mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
#ifdef DISABLE_NEWLINE_AUTO_RETURN
        mode |= DISABLE_NEWLINE_AUTO_RETURN;
#endif
        if (!SetConsoleMode(out, mode)) caps.ansi = false;
      }
    }
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (in != INVALID_HANDLE_VALUE) {
      DWORD mode = 0;
      if (GetConsoleMode(in, &mode)) {
        mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
        SetConsoleMode(in, mode);
      }
    }
  }
#else
  (void)interactive;
#endif
  return caps;
}

void clearInteractiveScreen(std::ostream& out, const TerminalCaps& caps) {
  if (caps.ansi) {
    out << "\x1b[2J\x1b[H";
  }
}

} // namespace scopefs
