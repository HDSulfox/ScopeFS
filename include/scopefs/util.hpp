#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace scopefs {

std::string nowIso();
std::string encode(const std::string& value);
std::string decode(const std::string& value);
std::string toHex(const std::string& bytes);
std::string fromHex(const std::string& hex);
std::uint32_t checksum32(const std::string& text);
std::uint32_t checksumFields(const std::vector<std::string>& fields);
std::vector<std::string> split(const std::string& text, char delim);
std::string join(const std::vector<std::string>& values, char delim);
std::string joinSet(const std::set<std::string>& values, char delim);
std::set<std::string> splitSet(const std::string& text, char delim);
std::string lower(std::string text);
bool startsWith(const std::string& text, const std::string& prefix);
std::string trim(const std::string& text);
std::vector<std::string> tokenize(const std::string& line);
std::string modeString(int mode, bool directory);
int parseMode(const std::string& text, int fallback);
std::string rightsNormalize(const std::string& rights);
bool rightsContain(const std::string& rights, const std::string& right);
std::string pathJoin(const std::string& base, const std::string& name);
std::vector<std::string> pathParts(const std::string& path);

template <typename T>
std::string csvNumbers(const std::vector<T>& values) {
  std::ostringstream out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i) out << ',';
    out << values[i];
  }
  return out.str();
}

template <typename T>
std::vector<T> parseCsvNumbers(const std::string& text) {
  std::vector<T> out;
  if (text.empty() || text == "-") return out;
  for (const auto& item : split(text, ',')) {
    if (item.empty()) continue;
    std::istringstream in(item);
    T value{};
    in >> value;
    if (!in.fail()) out.push_back(value);
  }
  return out;
}

} // namespace scopefs
