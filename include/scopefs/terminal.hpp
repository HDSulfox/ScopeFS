#pragma once

#include <ostream>

namespace scopefs {

struct TerminalCaps {
  bool ansi = true;
  bool utf8 = true;
  bool inputTty = false;
};

TerminalCaps initTerminal(bool interactive);
void clearInteractiveScreen(std::ostream& out, const TerminalCaps& caps);

} // namespace scopefs
