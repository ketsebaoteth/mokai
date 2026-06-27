#include "graph.hpp"

namespace fs = std::filesystem;

namespace mokai {
std::string Graph::getNormalizedFileTimestamp(const fs::path &path) const {
  if (!fs::exists(path))
    return "";
  auto ftime = fs::last_write_time(path);
  auto duration = ftime.time_since_epoch();
  auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
  return std::to_string(millis);
}

bool Graph::tryLoadGraphCache() {
  std::string path = getCachePath();
  if (!fs::exists(path))
    return false;

  std::ifstream is(path, std::ios::binary);
  if (!is.is_open())
    return false;

  try {
    // Verify Format Headers
    uint32_t magic = 0;
    uint32_t version = 0;
    is.read(reinterpret_cast<char *>(&magic), sizeof(magic));
    is.read(reinterpret_cast<char *>(&version), sizeof(version));

    if (magic != MOKAI_CACHE_MAGIC) {
      m_logger.Warn("Invalid cache file signature. Ignoring stale cache.");
      return false;
    }
    if (version != MOKAI_CACHE_VERSION) {
      m_logger.Warn("Mokai cache schema version mismatch. Regenerating graph.");
      return false;
    }

    size_t mCount;
    is.read(reinterpret_cast<char *>(&mCount), sizeof(mCount));

    bool cache_is_partially_dirty = false;
    for (size_t i = 0; i < mCount; ++i) {
      std::string p = readString(is);
      std::string cached_t = readString(is);

      std::string current_t = getNormalizedFileTimestamp(p);
      if (current_t.empty() || current_t != cached_t) {
        cache_is_partially_dirty = true;
      }
      m_manifestTimestamps[p] = cached_t;
    }

    size_t eCount;
    is.read(reinterpret_cast<char *>(&eCount), sizeof(eCount));
    m_edges.clear();
    m_edges.reserve(eCount);
    for (size_t i = 0; i < eCount; ++i) {
      std::string f = readString(is);
      std::string t = readString(is);
      m_edges.push_back({f, t});
    }

    size_t sCount;
    is.read(reinterpret_cast<char *>(&sCount), sizeof(sCount));
    for (size_t i = 0; i < sCount; ++i) {
      std::string qn = readString(is);
      m_resolvedSourcesCache[qn] = readVector(is);
    }

    if (cache_is_partially_dirty) {
      m_logger.Warn("Incremental changes detected inside manifest layouts. "
                    "Re-validating out-of-date targets.");
    }

  } catch (const std::ios_base::failure &e) {
    m_logger.Error(
        std::string(
            "Mokai binary graph cache reading corrupted or truncated: ") +
        e.what());
    return false;
  } catch (const std::exception &e) {
    m_logger.Error(std::string("Failed to parse dependency graph cache: ") +
                   e.what());
    return false;
  }

  return true;
}

void Graph::saveGraphCache() {
  std::ofstream os(getCachePath(), std::ios::binary);
  if (!os.is_open()) {
    m_logger.Error(
        "Failed to open cache location for writing file telemetry data.");
    return;
  }

  try {
    //  Write Binary Schema Safe Guards
    os.write(reinterpret_cast<const char *>(&MOKAI_CACHE_MAGIC),
             sizeof(MOKAI_CACHE_MAGIC));
    os.write(reinterpret_cast<const char *>(&MOKAI_CACHE_VERSION),
             sizeof(MOKAI_CACHE_VERSION));

    size_t mCount = m_manifestTimestamps.size();
    os.write(reinterpret_cast<const char *>(&mCount), sizeof(mCount));
    for (auto const &[p, t] : m_manifestTimestamps) {
      writeString(os, p);
      writeString(os, getNormalizedFileTimestamp(p));
    }

    size_t eCount = m_edges.size();
    os.write(reinterpret_cast<const char *>(&eCount), sizeof(eCount));
    for (const auto &e : m_edges) {
      writeString(os, e.from);
      writeString(os, e.to);
    }

    size_t sCount = m_resolvedSourcesCache.size();
    os.write(reinterpret_cast<const char *>(&sCount), sizeof(sCount));
    for (auto const &[qn, srcs] : m_resolvedSourcesCache) {
      writeString(os, qn);
      writeVector(os, srcs);
    }
  } catch (const std::exception &e) {
    m_logger.Error(std::string("Abrupt error encountered while writing back "
                               "graph updates to disk: ") +
                   e.what());
  }
}

} // namespace mokai
