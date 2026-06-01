#pragma once

#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include "scopefs/kernel.hpp"
#include "scopefs/terminal.hpp"

namespace scopefs {

std::vector<std::string> tokenize(const std::string& line);

class Shell {
 public:
  explicit Shell(FileSystemKernel& kernel);
  int run(std::istream& in, std::ostream& out, bool interactive, const TerminalCaps& caps);

 private:
  FileSystemKernel& kernel_;
  void banner(std::ostream& out, const TerminalCaps& caps) const;
  std::string readPassword();
};

} // namespace scopefs
