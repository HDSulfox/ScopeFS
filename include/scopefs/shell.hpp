#pragma once

#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include "scopefs/kernel.hpp"
#include "scopefs/terminal.hpp"
#include "scopefs/ui.hpp"

namespace scopefs {

std::vector<std::string> tokenize(const std::string& line);

class Shell {
 public:
  explicit Shell(FileSystemKernel& kernel);
  int run(std::istream& in, std::ostream& out, bool interactive, const TerminalCaps& caps);

 private:
  FileSystemKernel& kernel_;
  std::vector<std::string> history_;
  void banner(std::ostream& out, const TerminalCaps& caps) const;
  std::string readInteractiveLine(std::ostream& out, const TerminalCaps& caps);
  void redrawInteractiveLine(std::ostream& out, const std::string& line, std::size_t cursor) const;
  std::vector<std::string> completionCandidates(const std::string& line, std::size_t cursor, std::size_t* tokenStart) const;
  std::string readPassword();
};

} // namespace scopefs
