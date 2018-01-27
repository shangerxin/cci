#include "cci/lex/lexer.hpp"
#include "cci/basic/diagnostics.hpp"
#include "cci/basic/source_manager.hpp"
#include "cci/util/contracts.hpp"
#include "gtest/gtest.h"
#include <string>
#include <string_view>

namespace {

TEST(LexerTest, identifiers)
{
  const char *code = R"(
int
_abc123 escaped\
newline
)";
  cci::DiagnosticsOptions opts;
  cci::CompilerDiagnostics diag(opts);
  auto source = cci::SourceManager::from_buffer(diag, code);
  auto tstream = cci::TokenStream::tokenize(source);

  EXPECT_TRUE(tstream.peek().is(cci::TokenKind::kw_int));
  EXPECT_EQ("int", source.text_slice(tstream.consume().source_range()));

  EXPECT_TRUE(tstream.peek().is(cci::TokenKind::identifier));
  EXPECT_EQ("_abc123", source.text_slice(tstream.consume().source_range()));

  EXPECT_TRUE(tstream.peek().is(cci::TokenKind::identifier));
  EXPECT_EQ("escaped\\\nnewline", source.text_slice(tstream.consume().source_range()));

  EXPECT_FALSE(diag.has_errors() || diag.has_warnings());
  EXPECT_TRUE(tstream.empty());
}

TEST(LexerTest, universalCharacterNames)
{
  const char *code = R"(
\u1234 \UAABBCCDD \UABCD
)";
  cci::DiagnosticsOptions opts;
  cci::CompilerDiagnostics diag(opts);
  auto source = cci::SourceManager::from_buffer(diag, code);
  auto tstream = cci::TokenStream::tokenize(source);

  EXPECT_TRUE(tstream.peek().is(cci::TokenKind::identifier));
  EXPECT_EQ(R"(\u1234)", source.text_slice(tstream.consume().source_range()));

  EXPECT_TRUE(tstream.peek().is(cci::TokenKind::identifier));
  EXPECT_EQ(R"(\UAABBCCDD)", source.text_slice(tstream.consume().source_range()));

  EXPECT_FALSE(diag.has_errors() || diag.has_warnings());

  EXPECT_TRUE(tstream.peek().is(cci::TokenKind::unknown));
  EXPECT_EQ("\\", source.text_slice(tstream.consume().source_range()));

  EXPECT_TRUE(tstream.peek().is(cci::TokenKind::identifier));
  EXPECT_EQ("UABCD", source.text_slice(tstream.consume().source_range()));

  EXPECT_TRUE(tstream.empty());
  EXPECT_TRUE(diag.has_errors() || diag.has_warnings());
}

TEST(LexerTest, numericConstants)
{
  const char *code = R"(
42ULL 3.14f 161.80e-3 1.9E377P+1 .999
)";
  const std::string_view corrects[] {
    "42ULL",
    "3.14f",
    "161.80e-3",
    "1.9E377P+1",
    ".999",
  };
  cci::DiagnosticsOptions opts;
  cci::CompilerDiagnostics diag(opts);
  auto source = cci::SourceManager::from_buffer(diag, code);
  auto tstream = cci::TokenStream::tokenize(source);

  for (const auto correct : corrects)
  {
    EXPECT_TRUE(tstream.peek().is(cci::TokenKind::numeric_constant));
    EXPECT_EQ(correct, tstream.consume().spelling(source));
  }

  EXPECT_TRUE(tstream.empty());
  EXPECT_FALSE(diag.has_errors() || diag.has_warnings());
}

TEST(LexerTest, comments)
{
  // TODO: "a//b" // string literal
  const char *code = R"(
dont_skip_1 // this should be skipped, \
WE GET SIGNAL!
// skip this \too
/\
/ and this too
dont_skip_2
// */         // comment, not syntax error
f = g/**//h   // f = g / h
//\
x             // first two-line comment
/\
/ y           // second two-line comment
/*//*/ z      // z
m = n//**/o
  + p         // m = n + p
)";
  const std::pair<std::string_view, cci::TokenKind> corrects[]{
    {"dont_skip_1", cci::TokenKind::identifier},
    {"dont_skip_2", cci::TokenKind::identifier},
    {"f", cci::TokenKind::identifier},
    {"=", cci::TokenKind::equal},
    {"g", cci::TokenKind::identifier},
    {"/", cci::TokenKind::slash},
    {"h", cci::TokenKind::identifier}, // f = g / h
    {"z", cci::TokenKind::identifier}, // z
    {"m", cci::TokenKind::identifier},
    {"=", cci::TokenKind::equal},
    {"n", cci::TokenKind::identifier},
    {"+", cci::TokenKind::plus},
    {"p", cci::TokenKind::identifier}, // m = n + p
  };
  cci::DiagnosticsOptions opts;
  cci::CompilerDiagnostics diag(opts);
  auto source = cci::SourceManager::from_buffer(diag, code);
  auto tstream = cci::TokenStream::tokenize(source);

  for (const auto [spell, kind] : corrects)
  {
    EXPECT_EQ(kind, tstream.peek().kind);
    EXPECT_EQ(spell, tstream.consume().spelling(source));
  }

  EXPECT_TRUE(tstream.empty());
  EXPECT_FALSE(diag.has_errors() || diag.has_warnings());
}

} // namespace
