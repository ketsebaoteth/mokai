#include "graph.hpp"

namespace mokai {
void Graph::writeString(std::ostream &os, const std::string &s) {
  size_t len = s.size();
  os.write(reinterpret_cast<const char *>(&len), sizeof(len));
  os.write(s.data(), len);
}

std::string Graph::readString(std::istream &is) {
  size_t len = 0;
  is.read(reinterpret_cast<char *>(&len), sizeof(len));
  std::string s(len, '\0');
  if (len > 0) {
    is.read(&s[0], len);
  }
  return s;
}

void Graph::writeVector(std::ostream &os, const std::vector<std::string> &v) {
  size_t sz = v.size();
  os.write(reinterpret_cast<const char *>(&sz), sizeof(sz));
  for (const auto &s : v)
    writeString(os, s);
}

std::vector<std::string> Graph::readVector(std::istream &is) {
  size_t sz = 0;
  is.read(reinterpret_cast<char *>(&sz), sizeof(sz));
  std::vector<std::string> v(sz);
  for (size_t i = 0; i < sz; ++i)
    v[i] = readString(is);
  return v;
}
} // namespace mokai
