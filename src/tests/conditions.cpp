
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "ext/doctest.h"
#include "graph/condition/ConditionEval.hpp"

TEST_CASE("basic equality and inequality") {
  mokai::ConditionEngine engine;
  engine.setVariable("var", "joe");

  CHECK(engine.evaluate("var == joe")); // true
  CHECK(engine.evaluate("var == someone") == false);
  CHECK(engine.evaluate("var != joe") == false);
  CHECK(engine.evaluate("var != someone")); // true
}

TEST_CASE("boolean negation cases") {
  mokai::ConditionEngine engine;

  engine.setVariable("var", "true");
  CHECK(engine.evaluate("var")); // true
  CHECK(engine.evaluate("!var") == false);

  engine.setVariable("var", "false");
  CHECK(engine.evaluate("var") == false);
  CHECK(engine.evaluate("!var")); // true

  engine.setVariable("var", "joe");
  CHECK(engine.evaluate("var") == false); // non-boolean string → false
  CHECK(engine.evaluate("!var"));         // negation → true
}

TEST_CASE("numeric comparisons") {
  mokai::ConditionEngine engine;
  engine.setVariable("num", "10");

  CHECK(engine.evaluate("num == 10")); // equality
  CHECK(engine.evaluate("num != 5"));  // inequality
  CHECK(engine.evaluate("num > 5"));   // greater
  CHECK(engine.evaluate("num >= 10")); // greater or equal
  CHECK(engine.evaluate("num < 20"));  // less
  CHECK(engine.evaluate("num <= 10")); // less or equal
}

TEST_CASE("boolean comparisons") {
  mokai::ConditionEngine engine;
  engine.setVariable("flag", "true");

  CHECK(engine.evaluate("flag == true"));  // true
  CHECK(engine.evaluate("flag != false")); // true
  CHECK(engine.evaluate("flag == false") == false);
}

TEST_CASE("numeric range with logical AND") {
  mokai::ConditionEngine engine;
  engine.setVariable("num", "15");

  CHECK(engine.evaluate("num > 10 && num < 20")); // true
  CHECK(engine.evaluate("num > 20 && num < 30") == false);
}

TEST_CASE("nested parentheses with mixed operators") {
  mokai::ConditionEngine engine;
  engine.setVariable("os", "linux");
  engine.setVariable("arch", "x86_64");
  engine.setVariable("num", "42");

  CHECK(engine.evaluate("(os == linux && arch == x86_64) || num < 10")); // true
  CHECK(
      engine.evaluate("!(os == windows) && (num >= 40 && num <= 50)")); // true
}
