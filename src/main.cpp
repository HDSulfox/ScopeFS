#include <exception>
#include <iostream>

#include "scopefs/kernel.hpp"
#include "scopefs/shell.hpp"

int main(int argc, char** argv) {
  const bool scriptMode = argc > 1 && std::string(argv[1]) == "--script";
  scopefs::FileSystemKernel kernel;
  try {
    kernel.boot();
    scopefs::Shell shell(kernel);
    const int code = shell.run(std::cin, std::cout, !scriptMode);
    kernel.unmountClean();
    return code;
  } catch (const scopefs::CrashException& ex) {
    std::cerr << "E_CRASH: " << ex.what() << "\n";
    return 88;
  } catch (const std::exception& ex) {
    std::cerr << "E_FATAL: " << ex.what() << "\n";
    return 2;
  }
}
