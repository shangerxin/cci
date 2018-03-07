#pragma once

#include "cci/basic/source_manager.hpp"
#include "cci/lex/lexer.hpp"

namespace cci {

// TODO: This really should be migrated to the main lexer.
struct NumericConstantParser
{
  const char *digit_begin; //< First meaningful digit.
  const char *digit_end; //< Past the last meaningful digit.

  bool has_error = false;
  bool has_period = false;
  bool has_exponent = false;
  bool is_unsigned = false;
  bool is_long = false;
  bool is_long_long = false;
  bool is_float = false;

  int32_t radix = 0;

  NumericConstantParser(Lexer &, std::string_view tok_spelling,
                        SourceLocation tok_loc);

  // Evaluates and returns the numeric constant to an integer constant value, as
  // well as whether the evaluation overflowed.
  auto eval_to_integer() -> std::pair<uint64_t, bool>;

  bool is_floating_literal() const { return has_period || has_exponent; }
  bool is_integer_literal() const { return !is_floating_literal(); }
};

struct CharConstantParser
{
  uint64_t value = 0;

  CharConstantParser(Lexer &, std::string_view tok_spelling,
                     SourceLocation tok_loc, TokenKind char_kind);
};

} // namespace cci
