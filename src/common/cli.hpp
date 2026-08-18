// Small helpers shared by the two command-line programs.
#pragma once

#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace cli {

inline std::string readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open file: " + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

inline std::vector<uint8_t> readBinary(const std::string& path) {
  const std::string s = readFile(path);
  return std::vector<uint8_t>(s.begin(), s.end());
}

inline void writeBinary(const std::string& path, const std::vector<uint8_t>& data) {
  std::ofstream out(path, std::ios::binary);
  if (!out) throw std::runtime_error("cannot open file for writing: " + path);
  out.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
  if (!out) throw std::runtime_error("failed while writing: " + path);
}

// Reads all of stdin as bytes.
inline std::vector<uint8_t> readStdin() {
  std::ostringstream ss;
  ss << std::cin.rdbuf();
  const std::string s = ss.str();
  return std::vector<uint8_t>(s.begin(), s.end());
}

// Minimal --flag / --flag=value / --flag value parser.
class Args {
 public:
  Args(int argc, char** argv, int first = 1) {
    for (int i = first; i < argc; ++i) {
      const std::string a = argv[i];
      const bool long_flag = a.rfind("--", 0) == 0;
      const bool short_flag = !long_flag && a.size() > 1 && a[0] == '-';
      if (!long_flag && !short_flag) {
        positional_.push_back(a);
        continue;
      }
      const std::string body = a.substr(long_flag ? 2 : 1);
      const size_t eq = body.find('=');
      if (eq != std::string::npos) {
        flags_[body.substr(0, eq)] = body.substr(eq + 1);
      } else if (i + 1 < argc && argv[i + 1][0] != '-') {
        flags_[body] = argv[++i];
      } else {
        flags_[body] = "1";  // bare switch
      }
    }
  }

  bool has(const std::string& name) const { return flags_.count(name) > 0; }
  std::string get(const std::string& name, const std::string& fallback = "") const {
    auto it = flags_.find(name);
    return it == flags_.end() ? fallback : it->second;
  }
  long num(const std::string& name, long fallback) const {
    auto it = flags_.find(name);
    return it == flags_.end() ? fallback : std::stol(it->second);
  }
  double real(const std::string& name, double fallback) const {
    auto it = flags_.find(name);
    return it == flags_.end() ? fallback : std::stod(it->second);
  }
  const std::vector<std::string>& positional() const { return positional_; }

 private:
  std::map<std::string, std::string> flags_;
  std::vector<std::string> positional_;
};

}  // namespace cli
