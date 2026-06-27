#include "ConditionEval.hpp"
#include <algorithm>
#include <string>
#include <vector>

namespace mokai {

inline bool safe_isspace(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
         c == '\r';
}

void ConditionEngine::registerSystemPropreties() {
#if defined(_WIN32) || defined(_WIN64)
  m_registry["os"] = "windows";
  m_registry["platform"] = "windows";
#elif defined(__APPLE__)
  m_registry["os"] = "macos";
  m_registry["platform"] = "darwin";
#else
  m_registry["os"] = "linux";
  m_registry["platform"] = "unix";
#endif

#if defined(__clang__)
  m_registry["compiler"] = "clang";
  m_registry["compiler_family"] = "llvm";
  m_registry["compiler_version"] =
      std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__);
#elif defined(_MSC_VER)
  m_registry["compiler"] = "msvc";
  m_registry["compiler_family"] = "msvc";
  m_registry["compiler_version"] = std::to_string(_MSC_VER);
#elif defined(__GNUC__)
  m_registry["compiler"] = "gcc";
  m_registry["compiler_family"] = "gnu";
  m_registry["compiler_version"] =
      std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__);
#else
  m_registry["compiler"] = "unknown";
  m_registry["compiler_family"] = "unknown";
  m_registry["compiler_version"] = "0.0";
#endif

#if defined(__x86_64__) || defined(_M_X64)
  m_registry["arch"] = "x86_64";
  m_registry["bits"] = "64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  m_registry["arch"] = "arm64";
  m_registry["bits"] = "64";
#elif defined(__i386__) || defined(_M_IX86)
  m_registry["arch"] = "x86";
  m_registry["bits"] = "32";
#else
  m_registry["arch"] = "unknown";
  m_registry["bits"] = "unknown";
#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  m_registry["endian"] = "big";
#else
  m_registry["endian"] = "little";
#endif

  m_registry["target_os"] = m_registry["os"];
  m_registry["target_arch"] = m_registry["arch"];

#if defined(_MSC_VER)
  m_registry["target_env"] = "msvc";
#elif defined(__musl__)
  m_registry["target_env"] = "musl";
#else
  m_registry["target_env"] = "gnu";
#endif
}

std::vector<std::string>
ConditionEngine::tokenize(const std::string &expr) const {
  std::vector<std::string> tokens;
  std::string lexeme;

  auto pushLexeme = [&]() {
    if (!lexeme.empty()) {
      tokens.push_back(lexeme);
      lexeme.clear();
    }
  };

  for (size_t i = 0; i < expr.size(); ++i) {
    char c = expr[i];

    if (i + 1 < expr.size()) {
      char next = expr[i + 1];
      bool is_two_char_op = false;
      std::string op;

      if ((c == '&' && next == '&') || (c == '|' && next == '|') ||
          (c == '=' && next == '=') || (c == '!' && next == '=') ||
          (c == '<' && next == '=') || (c == '>' && next == '=')) {
        op = {c, next};
        is_two_char_op = true;
      }

      if (is_two_char_op) {
        pushLexeme();
        tokens.push_back(op);
        ++i;
        continue;
      }
    }

    if (safe_isspace(static_cast<unsigned char>(c))) {
      pushLexeme();
    } else if (c == '(' || c == ')' || c == '!' || c == '<' || c == '>') {
      pushLexeme();
      tokens.push_back(std::string(1, c));
    } else {
      lexeme += c;
    }
  }
  pushLexeme();
  return tokens;
}

bool ConditionEngine::evalAtom(const std::string &left, const std::string &op,
                               const std::string &right) const {
  // resolve values from the registry or fallback to literals
  auto it_left = m_registry.find(left);
  std::string_view final_left =
      (it_left != m_registry.end()) ? it_left->second : left;

  auto it_right = m_registry.find(right);
  std::string_view final_right =
      (it_right != m_registry.end()) ? it_right->second : right;

  // String operations
  if (op == "==")
    return final_left == final_right;
  if (op == "!=")
    return final_left != final_right;

  try {
    if (op == "<=")
      return std::stof(std::string(final_left)) <=
             std::stof(std::string(final_right));
    if (op == ">=")
      return std::stof(std::string(final_left)) >=
             std::stof(std::string(final_right));
    if (op == "<")
      return std::stof(std::string(final_left)) <
             std::stof(std::string(final_right));
    if (op == ">")
      return std::stof(std::string(final_left)) >
             std::stof(std::string(final_right));
  } catch (...) {
    // Fallback: If std::stof fails due to non-numeric types,
    // treated basic alphanumeric string bounds comparisons
    m_logger.Error("Invalid conditional comparison: Operator '" + op +
                   " cannot be applied to non-numeric operands " +
                   std::string(final_left) + "' and '" +
                   std::string(final_right) + "'.");
  }

  return false;
}

bool ConditionEngine::evaluate(const std::string &expression) const {
  std::vector<std::string> tokens = tokenize(expression);
  if (tokens.empty()) {
    return true;
  }

  size_t index = 0;
  bool result = parseLogicalExpr(tokens, index);

  if (index < tokens.size()) {
    m_logger.Error("Mokai Condition Error: Unexpected trailing tokens after "
                   "expression parsing boundary.");
  }
  return result;
}

bool ConditionEngine::parseLogicalExpr(const std::vector<std::string> &tokens,
                                       size_t &index) const {
  bool result = parseBooleanFactor(tokens, index);

  while (index < tokens.size()) {
    std::string op = tokens[index];
    if (op == "&&") {
      index++;
      bool right = parseBooleanFactor(tokens, index);
      result = result && right;
    } else if (op == "||") {
      index++;
      bool right = parseBooleanFactor(tokens, index);
      result = result || right;
    } else {
      break;
    }
  }
  return result;
}

bool ConditionEngine::parseBooleanFactor(const std::vector<std::string> &tokens,
                                         size_t &index) const {
  if (index < tokens.size() && tokens[index] == "!") {
    index++;
    return !parseBooleanFactor(tokens, index);
  }

  if (index < tokens.size() && tokens[index] == "(") {
    index++;
    bool result = parseLogicalExpr(tokens, index);
    if (index < tokens.size() && tokens[index] == ")") {
      index++;
    } else {
      m_logger.Error("Mokai Condition Error: Mismatched parentheses around "
                     "logical expression.");
    }
    return result;
  }

  std::string left_val = parseValue(tokens, index);

  if (index < tokens.size()) {
    std::string op = tokens[index];
    if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" ||
        op == ">=") {
      index++;
      if (index >= tokens.size()) {
        m_logger.Error("Mokai Condition Error: Missing right-hand operand "
                       "after operator '" +
                       op + "'.");
        return false;
      }
      std::string right_val = parseValue(tokens, index);
      return evalAtom(left_val, op, right_val);
    }
  }

  return (left_val == "true");
}

std::string ConditionEngine::parseValue(const std::vector<std::string> &tokens,
                                        size_t &index) const {
  if (index >= tokens.size()) {
    m_logger.Error("Mokai Condition Error: Expected an operand identifier or "
                   "literal value.");
    return "";
  }

  std::string token = tokens[index];
  index++;

  if (token.size() >= 2 && token.front() == '"' && token.back() == '"') {
    return token.substr(1, token.size() - 2);
  }

  auto it = m_registry.find(token);
  if (it != m_registry.end()) {
    return it->second;
  }
  bool is_number = std::all_of(token.begin(), token.end(), ::isdigit);
  if (!is_number && token != "true" && token != "false") {
    m_logger.Warn("Unknown identifier '" + token + "'");
  }

  return token;
}
} // namespace mokai
