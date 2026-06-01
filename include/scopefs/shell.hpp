#pragma once

#include <istream>
#include <ostream>
#include <string>
#include <vector>

#include "scopefs/kernel.hpp"

namespace scopefs {

std::vector<std::string> tokenize(const std::string& line);

class Shell {
 public:
  explicit Shell(FileSystemKernel& kernel);
  int run(std::istream& in, std::ostream& out, bool interactive);

 private:
  FileSystemKernel& kernel_;
  void banner(std::ostream& out) const;
  std::string readPassword();
};

} // namespace scopefs
