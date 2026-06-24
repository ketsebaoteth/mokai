#include "log.h"
#include <chrono>
#include <ctime>
#include <print>

namespace mokai::log {

Logger::Logger() {}
Logger::~Logger() {}

void Logger::Init() {}
void Logger::Shutdown() {}

void Logger::SetPrefix(const std::string &prefix) { m_prefix = prefix; }
void Logger::SetLevel(Level minLevel) { m_minLevel = minLevel; }

bool Logger::ShouldLog(Level level) const {
  return static_cast<int>(level) >= static_cast<int>(m_minLevel);
}

std::string Logger::FormatTimestamp() const {
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tmBuf{};
  localtime_r(&t, &tmBuf);

  char buf[16];
  std::strftime(buf, sizeof(buf), "%H:%M:%S", &tmBuf);
  return std::string(buf);
}

void Logger::WriteLine(Level level, const std::string &label,
                       const std::string &msg) {
  if (!ShouldLog(level)) {
    return;
  }

  const char *badgeColor = theme::info_label;
  const char *badgeText = "ℹ INFO ";

  switch (level) {
  case Level::Debug:
    badgeColor = theme::debug_label;
    badgeText = "⚙ DEBUG";
    break;
  case Level::Info:
    badgeColor = theme::info_label;
    badgeText = "ℹ INFO ";
    break;
  case Level::Warn:
    badgeColor = theme::warn_label;
    badgeText = "⚠ WARN ";
    break;
  case Level::Error:
    badgeColor = theme::error_label;
    badgeText = "✖ ERROR";
    break;
  case Level::Success:
    badgeColor = theme::success_label;
    badgeText = "✔ SUCCESS";
    break;
  }

  std::println("{} {} {}{}{} {}", theme::timestamp, FormatTimestamp(),
               badgeColor, badgeText, theme::reset, msg);
}

void Logger::Debug(const std::string &msg) {
  WriteLine(Level::Debug, "debug", msg);
}
void Logger::Info(const std::string &msg) {
  WriteLine(Level::Info, "info", msg);
}
void Logger::Warn(const std::string &msg) {
  WriteLine(Level::Warn, "warn", msg);
}
void Logger::Error(const std::string &msg) {
  WriteLine(Level::Error, "error", msg);
}
void Logger::Success(const std::string &msg) {
  WriteLine(Level::Success, "success", msg);
}

void Logger::Step(int current, int total, const std::string &msg) {
  if (!ShouldLog(Level::Info)) {
    return;
  }

  std::println("{} {} {}{}[{}/{}]{} {}", theme::timestamp, FormatTimestamp(),
               theme::info_label, "◐ STEP  ", current, total, theme::reset,
               msg);
}

void Logger::ErrorInline(const std::string &sourceLine, const std::string &hint,
                         int lineNumber, int caretStart, int caretLength) {
  if (!ShouldLog(Level::Error)) {
    return;
  }

  std::string lineNoStr = std::to_string(lineNumber);
  std::string gutter(lineNoStr.size(), ' ');

  int underlineLen = (caretLength == -1)
                         ? static_cast<int>(sourceLine.size()) - caretStart
                         : caretLength;
  if (underlineLen < 1) {
    underlineLen = 1;
  }

  std::println("");
  std::println(" {} {} |{} {}", theme::line_no, lineNoStr, theme::reset,
               sourceLine);
  std::println(" {} {} |{} {}{}{}  {}{}{}", theme::line_no, gutter,
               theme::reset, std::string(caretStart, ' '), theme::caret,
               std::string(underlineLen, '^'), theme::hint, hint, theme::reset);
  std::println("");
}

} // namespace mokai::log
