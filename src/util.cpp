#include "scopefs/util.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace scopefs {

std::string nowIso() {
  using clock = std::chrono::system_clock;
  const auto now = clock::now();
  const std::time_t t = clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
  return out.str();
}

std::string encode(const std::string& value) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (unsigned char ch : value) {
    out << std::setw(2) << static_cast<int>(ch);
  }
  return out.str();
}

std::string decode(const std::string& value) {
  return fromHex(value);
}

std::string toHex(const std::string& bytes) {
  return encode(bytes);
}

std::string fromHex(const std::string& hex) {
  std::string out;
  if (hex.size() % 2 != 0) return out;
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const auto byte = hex.substr(i, 2);
    char* end = nullptr;
    const long value = std::strtol(byte.c_str(), &end, 16);
    if (!end || *end != '\0') return {};
    out.push_back(static_cast<char>(value));
  }
  return out;
}

std::uint32_t checksum32(const std::string& text) {
  std::uint32_t hash = 2166136261u;
  for (unsigned char ch : text) {
    hash ^= ch;
    hash *= 16777619u;
  }
  return hash;
}

std::uint32_t checksumFields(const std::vector<std::string>& fields) {
  std::string joined;
  for (const auto& f : fields) {
    joined += f;
    joined.push_back('\x1f');
  }
  return checksum32(joined);
}

std::vector<std::string> split(const std::string& text, char delim) {
  std::vector<std::string> out;
  std::string current;
  std::istringstream in(text);
  while (std::getline(in, current, delim)) out.push_back(current);
  if (!text.empty() && text.back() == delim) out.emplace_back();
  return out;
}

std::string join(const std::vector<std::string>& values, char delim) {
  std::ostringstream out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i) out << delim;
    out << values[i];
  }
  return out.str();
}

std::string joinSet(const std::set<std::string>& values, char delim) {
  std::vector<std::string> tmp(values.begin(), values.end());
  return join(tmp, delim);
}

std::set<std::string> splitSet(const std::string& text, char delim) {
  std::set<std::string> out;
  if (text.empty() || text == "-") return out;
  for (const auto& item : split(text, delim)) {
    if (!item.empty()) out.insert(item);
  }
  return out;
}

std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

bool startsWith(const std::string& text, const std::string& prefix) {
  return text.rfind(prefix, 0) == 0;
}

std::string trim(const std::string& text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  const auto end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

std::string modeString(int mode, bool directory) {
  std::string out;
  out.push_back(directory ? 'd' : '-');
  const int masks[9] = {0400, 0200, 0100, 0040, 0020, 0010, 0004, 0002, 0001};
  const char chars[9] = {'r', 'w', 'x', 'r', 'w', 'x', 'r', 'w', 'x'};
  for (int i = 0; i < 9; ++i) out.push_back((mode & masks[i]) ? chars[i] : '-');
  return out;
}

int parseMode(const std::string& text, int fallback) {
  if (text.empty()) return fallback;
  int mode = 0;
  for (char ch : text) {
    if (ch < '0' || ch > '7') return fallback;
    mode = (mode << 3) + (ch - '0');
  }
  return mode;
}

std::string rightsNormalize(const std::string& rights) {
  static const std::string order = "rwxcdbsg";
  std::set<char> seen;
  for (char ch : rights) {
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (order.find(c) != std::string::npos) seen.insert(c);
  }
  std::string out;
  for (char c : order) {
    if (seen.count(c)) out.push_back(c);
  }
  return out;
}

bool rightsContain(const std::string& rights, const std::string& right) {
  const std::string normalized = rightsNormalize(rights);
  for (char ch : right) {
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (normalized.find(c) == std::string::npos) return false;
  }
  return true;
}

std::string pathJoin(const std::string& base, const std::string& name) {
  if (base.empty() || base == "/") return "/" + name;
  return base + "/" + name;
}

std::vector<std::string> pathParts(const std::string& path) {
  std::vector<std::string> out;
  for (const auto& p : split(path, '/')) {
    if (p.empty() || p == ".") continue;
    if (p == "..") {
      if (!out.empty()) out.pop_back();
      continue;
    }
    out.push_back(p);
  }
  return out;
}

} // namespace scopefs
