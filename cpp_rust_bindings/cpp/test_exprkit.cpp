// Unit tests for the C++ exprkit library.
//
// These own the *logic*: precedence, parsing, the error taxonomy, environment
// behavior. The Rust tests in ../rust deliberately do not repeat any of it --
// they test the seam instead. See ../CLAUDE.md.

#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "exprkit.hpp"

namespace {

using exprkit::Evaluator;
using exprkit::ExprError;

// Results of arithmetic are compared with a tolerance; exact literals are not.
constexpr double kEpsilon = 1e-12;

TEST(Evaluate, AddsAndSubtractsLeftToRight) {
  EXPECT_DOUBLE_EQ(exprkit::evaluate("1 + 2 + 3"), 6.0);
  EXPECT_DOUBLE_EQ(exprkit::evaluate("10 - 3 - 2"), 5.0);
}

TEST(Evaluate, MultipliesBeforeAdding) {
  EXPECT_DOUBLE_EQ(exprkit::evaluate("2 + 3 * 4"), 14.0);
  EXPECT_DOUBLE_EQ(exprkit::evaluate("(2 + 3) * 4"), 20.0);
}

TEST(Evaluate, DividesAndTakesRemainder) {
  EXPECT_DOUBLE_EQ(exprkit::evaluate("7 / 2"), 3.5);
  EXPECT_DOUBLE_EQ(exprkit::evaluate("7 % 2"), 1.0);
  EXPECT_DOUBLE_EQ(exprkit::evaluate("-7 % 2"), -1.0); // fmod keeps the sign
}

TEST(Evaluate, RaisesRightAssociatively) {
  EXPECT_DOUBLE_EQ(exprkit::evaluate("2 ^ 3 ^ 2"), 512.0); // not 64
  EXPECT_DOUBLE_EQ(exprkit::evaluate("2 ^ -1"), 0.5);
}

TEST(Evaluate, BindsUnaryMinusLooserThanExponentiation) {
  EXPECT_DOUBLE_EQ(exprkit::evaluate("-2 ^ 2"), -4.0);
  EXPECT_DOUBLE_EQ(exprkit::evaluate("(-2) ^ 2"), 4.0);
}

TEST(Evaluate, AcceptsStackedSigns) {
  EXPECT_DOUBLE_EQ(exprkit::evaluate("--3"), 3.0);
  EXPECT_DOUBLE_EQ(exprkit::evaluate("+-+3"), -3.0);
}

TEST(Evaluate, ParsesEveryNumberSpelling) {
  EXPECT_DOUBLE_EQ(exprkit::evaluate("0.5"), 0.5);
  EXPECT_DOUBLE_EQ(exprkit::evaluate(".5"), 0.5);
  EXPECT_DOUBLE_EQ(exprkit::evaluate("6e2"), 600.0);
  EXPECT_DOUBLE_EQ(exprkit::evaluate("1.5E-2"), 0.015);
}

TEST(Evaluate, IgnoresWhitespaceIncludingNewlines) {
  EXPECT_DOUBLE_EQ(exprkit::evaluate("\t 1 +\n 2 \r\n"), 3.0);
}

TEST(Evaluate, CallsBuiltinFunctions) {
  EXPECT_DOUBLE_EQ(exprkit::evaluate("sqrt(16)"), 4.0);
  EXPECT_DOUBLE_EQ(exprkit::evaluate("abs(0 - 3)"), 3.0);
  EXPECT_DOUBLE_EQ(exprkit::evaluate("floor(2.7)"), 2.0);
  EXPECT_DOUBLE_EQ(exprkit::evaluate("ceil(2.1)"), 3.0);
  EXPECT_NEAR(exprkit::evaluate("ln(exp(1))"), 1.0, kEpsilon);
  EXPECT_NEAR(exprkit::evaluate("sin(0) + cos(0)"), 1.0, kEpsilon);
}

TEST(Evaluate, SeesTheConstantsButNotItsCallersVariables) {
  EXPECT_NEAR(exprkit::evaluate("pi"), std::numbers::pi, kEpsilon);
  EXPECT_NEAR(exprkit::evaluate("e"), std::numbers::e, kEpsilon);
  // The scratch environment is discarded, so this does not leak anywhere.
  EXPECT_DOUBLE_EQ(exprkit::evaluate("x = 3"), 3.0);
  EXPECT_THROW(exprkit::evaluate("x"), ExprError);
}

TEST(Evaluate, RejectsEmptyAndBlankInput) {
  EXPECT_THROW(exprkit::evaluate(""), ExprError);
  EXPECT_THROW(exprkit::evaluate("   "), ExprError);
}

TEST(Evaluate, RejectsDivisionByZero) {
  EXPECT_THROW(exprkit::evaluate("1 / 0"), ExprError);
  EXPECT_THROW(exprkit::evaluate("1 % 0"), ExprError);
  EXPECT_THROW(exprkit::evaluate("1 / (2 - 2)"), ExprError);
}

TEST(Evaluate, RejectsResultsThatAreNotFinite) {
  EXPECT_THROW(exprkit::evaluate("1e308 * 10"), ExprError);
  EXPECT_THROW(exprkit::evaluate("sqrt(0 - 1)"), ExprError); // nan
  EXPECT_THROW(exprkit::evaluate("ln(0)"), ExprError);       // -inf
  EXPECT_THROW(exprkit::evaluate("1e400"), ExprError);       // out of range
}

TEST(Evaluate, RejectsUnknownNames) {
  EXPECT_THROW(exprkit::evaluate("nope"), ExprError);
  EXPECT_THROW(exprkit::evaluate("nope(1)"), ExprError);
}

TEST(Evaluate, RejectsMalformedInput) {
  EXPECT_THROW(exprkit::evaluate("1 +"), ExprError);
  EXPECT_THROW(exprkit::evaluate("(1 + 2"), ExprError);
  EXPECT_THROW(exprkit::evaluate("1 2"), ExprError);
  EXPECT_THROW(exprkit::evaluate("1 $ 2"), ExprError);
  EXPECT_THROW(exprkit::evaluate("sqrt(4"), ExprError);
}

// The message text is part of the API: it is what a user sees on the CLI and
// what arrives in Rust as the Err payload, so a few are pinned exactly.
TEST(Evaluate, MessagesNameTheProblem) {
  EXPECT_STREQ(
      [] {
        try {
          exprkit::evaluate("2 / 0");
        } catch (const ExprError &err) {
          return err.what();
        }
        return "no exception";
      }(),
      "division by zero");

  EXPECT_STREQ(
      [] {
        try {
          exprkit::evaluate("1 + wat");
        } catch (const ExprError &err) {
          return err.what();
        }
        return "no exception";
      }(),
      "unknown name: 'wat'");

  EXPECT_STREQ(
      [] {
        try {
          exprkit::evaluate("1 2");
        } catch (const ExprError &err) {
          return err.what();
        }
        return "no exception";
      }(),
      "unexpected trailing input: '2'");
}

TEST(FormatValue, PrintsTheShortestRoundTrip) {
  EXPECT_EQ(exprkit::format_value(1.0), "1");
  EXPECT_EQ(exprkit::format_value(-0.5), "-0.5");
  EXPECT_EQ(exprkit::format_value(0.1 + 0.2), "0.30000000000000004");
  EXPECT_EQ(exprkit::format_value(1e21), "1e+21");
}

TEST(EvaluatorTest, StartsWithOnlyTheConstants) {
  Evaluator evaluator;
  EXPECT_EQ(evaluator.names(), (std::vector<std::string>{"e", "pi"}));
  EXPECT_TRUE(evaluator.has("pi"));
  EXPECT_FALSE(evaluator.has("x"));
}

TEST(EvaluatorTest, RemembersAssignmentsAcrossCalls) {
  Evaluator evaluator;
  EXPECT_DOUBLE_EQ(evaluator.eval("x = 2 + 3"), 5.0);
  EXPECT_DOUBLE_EQ(evaluator.eval("x * 2"), 10.0);
  EXPECT_DOUBLE_EQ(evaluator.eval("x = x + 1"), 6.0);
  EXPECT_DOUBLE_EQ(evaluator.get("x"), 6.0);
}

TEST(EvaluatorTest, ChainsAssignments) {
  Evaluator evaluator;
  EXPECT_DOUBLE_EQ(evaluator.eval("a = b = 4"), 4.0);
  EXPECT_DOUBLE_EQ(evaluator.get("a"), 4.0);
  EXPECT_DOUBLE_EQ(evaluator.get("b"), 4.0);
}

TEST(EvaluatorTest, LetsConstantsBeReassigned) {
  Evaluator evaluator;
  EXPECT_DOUBLE_EQ(evaluator.eval("pi = 3"), 3.0);
  EXPECT_DOUBLE_EQ(evaluator.eval("pi"), 3.0);
}

TEST(EvaluatorTest, SetsAndGetsDirectly) {
  Evaluator evaluator;
  evaluator.set("k", 1.5);
  EXPECT_TRUE(evaluator.has("k"));
  EXPECT_DOUBLE_EQ(evaluator.eval("k * 2"), 3.0);
  EXPECT_THROW(evaluator.get("nope"), ExprError);
}

TEST(EvaluatorTest, RefusesToStoreNonFiniteValues) {
  Evaluator evaluator;
  EXPECT_THROW(evaluator.set("bad", std::nan("")), ExprError);
  EXPECT_THROW(evaluator.set("bad", HUGE_VAL), ExprError);
  EXPECT_FALSE(evaluator.has("bad"));
}

TEST(EvaluatorTest, ListsNamesSorted) {
  Evaluator evaluator;
  evaluator.set("zeta", 1.0);
  evaluator.set("alpha", 2.0);
  EXPECT_EQ(evaluator.names(),
            (std::vector<std::string>{"alpha", "e", "pi", "zeta"}));
}

TEST(EvaluatorTest, ClearRestoresTheConstants) {
  Evaluator evaluator;
  evaluator.set("x", 1.0);
  evaluator.clear();
  EXPECT_FALSE(evaluator.has("x"));
  EXPECT_EQ(evaluator.names(), (std::vector<std::string>{"e", "pi"}));
}

TEST(EvaluatorTest, KeepsTheEnvironmentIntactAfterAFailedEval) {
  Evaluator evaluator;
  evaluator.eval("x = 1");
  EXPECT_THROW(evaluator.eval("y = 1 / 0"), ExprError);
  EXPECT_FALSE(evaluator.has("y"));
  EXPECT_DOUBLE_EQ(evaluator.get("x"), 1.0);
}

} // namespace
