#include "cci/lex/lexer.hpp"
#include "cci/basic/diagnostics.hpp"
#include "cci/basic/source_manager.hpp"
#include "cci/util/contracts.hpp"
#include <algorithm>
#include <cassert>
#include <memory>
#include <utility>

namespace cci {
namespace diag {
enum Lex
{
#define DIAG(CODE, LEVEL, FORMAT) CODE,
#include "cci/basic/diagnostics_lex.inc"
#undef DIAG
};
} // namespace diag

template <>
struct diagnostics_error_code<diag::Lex>
{
  constexpr static auto info(diag::Lex code) -> ErrorCodeInfo
  {
#define DIAG(CODE, LEVEL, FORMAT)                                              \
  case diag::CODE: return {LEVEL, FORMAT};
    switch (code)
    {
#include "cci/basic/diagnostics_lex.inc"
    }
#undef DIAG
  }
};

template <>
struct is_diagnostics_error_code<diag::Lex> : std::true_type
{
};

// token:
//   keyword
//   identifier
//   constant
//   string-literal
//   punctuator

static constexpr TokenKind KEYWORD_KINDS[]{
  TokenKind::kw_auto,           TokenKind::kw_break,
  TokenKind::kw_case,           TokenKind::kw_char,
  TokenKind::kw_const,          TokenKind::kw_continue,
  TokenKind::kw_default,        TokenKind::kw_do,
  TokenKind::kw_double,         TokenKind::kw_else,
  TokenKind::kw_enum,           TokenKind::kw_extern,
  TokenKind::kw_float,          TokenKind::kw_for,
  TokenKind::kw_goto,           TokenKind::kw_if,
  TokenKind::kw_inline,         TokenKind::kw_int,
  TokenKind::kw_long,           TokenKind::kw_register,
  TokenKind::kw_restrict,       TokenKind::kw_return,
  TokenKind::kw_short,          TokenKind::kw_signed,
  TokenKind::kw_sizeof,         TokenKind::kw_static,
  TokenKind::kw_struct,         TokenKind::kw_switch,
  TokenKind::kw_typedef,        TokenKind::kw_union,
  TokenKind::kw_unsigned,       TokenKind::kw_void,
  TokenKind::kw_volatile,       TokenKind::kw_while,
  TokenKind::kw__Alignas,       TokenKind::kw__Alignof,
  TokenKind::kw__Atomic,        TokenKind::kw__Bool,
  TokenKind::kw__Complex,       TokenKind::kw__Generic,
  TokenKind::kw__Imaginary,     TokenKind::kw__Noreturn,
  TokenKind::kw__Static_assert, TokenKind::kw__Thread_local,
};

constexpr auto is_nondigit(char C) -> bool
{
  return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || C == '_';
}

constexpr auto is_digit(char C) -> bool { return C >= '0' && C <= '9'; }

constexpr auto is_hexdigit(char C) -> bool
{
  return (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f') ||
         (C >= 'A' && C <= 'F');
}

constexpr auto is_octdigit(char C) -> bool
{
  return (C >= '0' && C <= '7');
}

constexpr auto is_newline(char C) -> bool { return C == '\n' || C == '\r'; }

constexpr auto is_whitespace(char C) -> bool
{
  return C == ' ' || C == '\t' || C == '\v' || C == '\f' || is_newline(C);
}

constexpr auto hexdigit_value(char C) -> uint32_t
{
  if (is_hexdigit(C))
  {
    if (C >= '0' && C <= '9')
      return static_cast<uint32_t>(C - '0');
    if (C >= 'a' && C <= 'f')
      return static_cast<uint32_t>(C - 'a' + 10);
    if (C >= 'A' && C <= 'F')
      return static_cast<uint32_t>(C - 'A' + 10);
    cci_unreachable();
  }
  return -1U;
}

constexpr auto is_escape_sequence(char C) -> bool
{
  return C == '\'' || C == '"' || C == '?' || C == '\\' || C == 'a' ||
         C == 'b' || C == 'f' || C == 'n' || C == 'r' || C == 't' || C == 'v' ||
         is_octdigit(C) || C == 'x';
}

namespace {

template <typename ErrorCode, typename... Args>
void report(Lexer &lex, const char *ctx, ErrorCode err_code, Args&&... args)
{
  return lex.diag.report(lex.location_for_ptr(ctx), err_code,
                         std::forward<Args>(args)...);
}

// The following helper functions are based off on Clang's lexer implementation.
//
// The lexer works with a few useful functions. Because the C grammar is a
// little complex, it is not possible to implement a lexer that iterates over
// ASCII characters char by char without any special handling. There are things
// like escaped newlines, trigraphs and UCNs which make lexing a bit more
// difficult. With that said, these few functions implement a "peek and consume"
// interface that handles all of those special syntax. The ideia is that in
// order to consume a character, you specify the size of it, i.e. the number of
// characters that theoretically compose only one character. For example, a
// trigraph like '??|' would have size 3, where the first '?' would signal that
// it is not a trivial character, and needs special care. After peeking it, you
// get the first character ('?'), and the size to be skipped over (3). When
// consuming this peeked character, the buffer pointer will end up past the end
// of the trigraph, i.e. `ptr + 3`, where `ptr` is the current buffer pointer.

// Calculates the size of an escaped newline. Assumes that the slash character
// is already consumed. Whitespaces between the slash and the newline are
// considered as ill-formed.
//
// \param ptr The position past the backslash ('\') character.
//
// \return The distance between `ptr` and the first character after the escaped
//         newline.
auto size_for_escaped_newline(const char *ptr)
  -> int64_t
{
  // FIXME: This assert is wrong, could be a `??/` trigraph.
  cci_expects(*std::prev(ptr) == '\\');
  int64_t nl_size = 0;

  if (is_newline(ptr[nl_size]))
  {
    ++nl_size;

    // Consumes a pair of \r\n or \n\r if there is any.
    if (is_newline(ptr[nl_size]) && ptr[nl_size - 1] != ptr[nl_size])
      ++nl_size;

    return nl_size;
  }

  // Not a newline.
  return 0;
}

// Checks whether a character needs any special care.
//
// Things like trigraphs and escaped newlines are examples of such special
// characters. They need to be properly consumed, therefore you can't just
// advance the buffer pointer by 1.
//
// \param c The character to be checked.
// \return true if character doesn't need special care.
constexpr bool is_trivial_character(char c)
{
  return c != '?' && c != '\\';
}

// Peeks a character from the input stream and returns it, setting the size to
// how many characters are to be skipped over. This handles special cases like
// escaped newlines and trigraphs*.
//
// \param ptr Buffer pointer from which to peek a character.
// \param size Variable to set the distance to the next simple character.
// \param tok Token being formed, if any.
//
// \return The character pointed by `ptr`.
//
// * TODO: Will handle trigraphs eventually.
auto peek_char_and_size_nontrivial(const char *ptr, int64_t &size,
                                   Token *tok = nullptr) -> char
{
  if (*ptr == '\\')
  {
    ++ptr;
    ++size;

    // There's no need to escape anything other than whitespaces.
    if (!is_whitespace(*ptr)) return '\\';

    if (int64_t esc_nl_size = size_for_escaped_newline(ptr))
    {
      if (tok) tok->set_flags(Token::IsDirty);
      ptr += esc_nl_size;
      size += esc_nl_size;
      return peek_char_and_size_nontrivial(ptr, size, tok);
    }

    // Not a newline, just a regular whitespace.
    return '\\';
  }

  // TODO: Trigraphs.

  // Peek a simple character.
  ++size;
  return *ptr;
}

// Peeks a character from `ptr` and advances it.
//
// This is the same as peek_char_and_size, except that the buffer pointer is
// properly advanced to the next simple character in the buffer.
//
// \param ptr Buffer pointer from which to peek and advance.
// \param tok Token being formed.
//
// \return The character pointed by ptr before advancing.
auto peek_char_advance(const char *&ptr, Token &tok) -> char
{
  if (is_trivial_character(*ptr)) return *ptr++;
  int64_t size = 0;
  char c = peek_char_and_size_nontrivial(ptr, size, &tok);
  ptr += size;
  return c;
}

// Peeks a character from the buffer pointer.
//
// If the character pointed by `ptr` is simple, then this is the fast path: it
// returns `*ptr` and sets `size` to 1. Otherwise, this function falls back to
// `peek_char_and_size_nontrivial`.
//
// \param ptr Buffer pointer from which to peek a character.
// \param size Variable to set the distance to the next simple character.
//
// \return The character pointed by `ptr`.
auto peek_char_and_size(const char *ptr, int64_t &size)
  -> char
{
  if (is_trivial_character(*ptr))
  {
    size = 1;
    return *ptr;
  }
  size = 0;
  return peek_char_and_size_nontrivial(ptr, size, nullptr);
}

// Consumes the buffer pointer.
//
// This is meant to be used along with `peek_char_and_size`, as it returns a
// buffer pointer by repeeking the same character if `size` doesn't correspond
// to the size of a simple character.  This reiteration is needed in order to
// set any special flags to the token `tok` being formed.
//
// \param ptr Buffer pointer from which to consume a peeked character.
// \param size The size of the character to be consumed.
// \param tok Token being formed.
//
// \return A buffer pointer past the peeked (non-)trivial character.
auto consume_char(const char *ptr, int64_t size, Token &tok)
  -> const char *
{
  // Consumes a simple character.
  if (size == 1)
    return ptr + size;

  // Otherwise, reparse it to get the right size.
  size = 0;
  peek_char_and_size_nontrivial(ptr, size, &tok);
  return ptr + size;
}

// universal-character-name: [C11 6.4.3/1]
//     '\u' hex-quad
//     '\U' hex-quad  hex-quad
//
// hex-quad:
//   hexadecimal-digit hexadecimal-digit
//       hexadecimal-digit hexadecimal-digit
//
// Reads a universal character name.
//
// Parses a \u or \U UCN and calculates the code point represented by it. If
// such code point isn't in a valid range as defined by [C11 6.4.3], reports
// diagnostics and returns 0, but still consumes the buffer pointer.  Ill-formed
// UCNs prevent the buffer pointer from being consumed, however.
//
// \param lex The lexer.
// \param start_ptr Buffer pointer to the UCN kind ('u' or 'U').
// \param slash_ptr Buffer pointer to the UCN slash ('\') before its kind.
// \param tok The token being formed, if any.
//
// \return The code point represented by the UCN.
auto try_read_ucn(Lexer &lex, const char *&start_ptr, const char *slash_ptr,
                  Token *tok = nullptr) -> uint32_t
{
  int64_t char_size = 0;
  const auto kind = peek_char_and_size(start_ptr, char_size);
  const int num_hexdigits = kind == 'u' ? 4 : kind == 'U' ? 8 : 0;

  if (num_hexdigits == 0)
    return 0;

  auto cur_ptr = start_ptr + char_size;
  uint32_t code_point = 0;

  // Parses the UCN, ignoring any escaped newlines.
  for (int i = 0; i < num_hexdigits; ++i)
  {
    const char c = peek_char_and_size(cur_ptr, char_size);
    const uint32_t value = hexdigit_value(c);
    if (value == -1U)
    {
      report(lex, slash_ptr, diag::warn_ucn_incomplete);
      return 0;
    }
    code_point <<= 4;
    code_point += value;
    cur_ptr += char_size;
  }

  // Take into account that this token might have escaped newlines,
  // so make any needed changes to tok. If no token is passed, then
  // just set start_ptr, it's good to go.
  if (tok)
  {
    tok->set_flags(Token::HasUCN);
    // Just set start_ptr if the UCN isn't dirty.
    if (std::distance(start_ptr, cur_ptr) == num_hexdigits + 2)
      start_ptr = cur_ptr;
    else
      while (start_ptr != cur_ptr)
        peek_char_advance(start_ptr, *tok);
  }
  else
    start_ptr = cur_ptr;

  // C11 6.4.3/2: A universal character name shall not specify a character
  // whose short identifier is less than 00A0 other than 0024 ($), 0040 (@),
  // or 0060 ('), nor one in the range D800 through DFFF inclusive.
  if (code_point < 0xA0)
  {
    if (code_point != 0x24 && code_point != 0x40 && code_point != 0x60)
    {
      report(lex, slash_ptr, diag::err_ucn_invalid);
      return 0;
    }
  }
  else if (code_point >= 0xD800 && code_point <= 0xDFFF)
  {
    report(lex, slash_ptr, diag::err_ucn_invalid);
    return 0;
  }

  return code_point;
}

// Lexes a UCN that is part of an identifier.
//
// This makes sure that the lexed UCN is a valid character for an identifier.
//
// \param lex The lexer.
// \param cur_ptr Buffer pointer that points to the slash ('\'). This pointer
//                is updated to point past the end of the UCN only if the UCN
//                is well-formed in an identifier.
// \param size Size of the peeked slash character ('\').
// \param result Token being formed.
//
// \return true if UCN is well-formed for an identifier.
auto try_advance_identifier_ucn(Lexer &lex, const char *&cur_ptr, int64_t size,
                                Token &result) -> bool
{
  auto ucn_ptr = cur_ptr + size;
  if (uint32_t code_point = try_read_ucn(lex, ucn_ptr, cur_ptr, nullptr);
      code_point == 0)
    return false;
  const auto ucn_size = std::distance(cur_ptr, ucn_ptr);
  if ((ucn_size == 6 && cur_ptr[1] == 'u') ||
      (ucn_size == 10 && cur_ptr[1] == 'U'))
    cur_ptr = ucn_ptr;
  else
    while (cur_ptr != ucn_ptr)
      peek_char_advance(cur_ptr, result);
  return true;
}

// identifier: [C11 6.4.2]
//   identifier-nondigit
//   identifier  identifier-nondigit
//   identifier  digit
//
// identifier-nondigit:
//   nondigit
//   universal-character-name
//   other implementation-defined characters
//
// nondigit: one of
//   _abcdefghijklm
//   nopqrstuvwxyz
//   ABCDEFGHIJKLM
//   NOPQRSTUVWXYZ
//
// digit: one of
//   0123456789
//
// Lexes an identifier. Assumes that the identifier's head is already consumed.
//
// \param lex The lexer.
// \param cur_ptr A pointer into the buffer that is past the first identifier's
//                character.
// \param result Token being formed.
//
// \return true if identifier was successfully formed.
auto lex_identifier(Lexer &lex, const char *cur_ptr, Token &result) -> bool
{
  char c = *cur_ptr++;

  // Most of the heavy work can be avoided if the identifier is
  // formed by ASCII characters only.
  while (is_nondigit(c) || is_digit(c))
    c = *cur_ptr++;

  // Backs up to correspond to `c`.
  --cur_ptr;

  // There's dirt, lexes the rest of the identifier.
  if (c == '\\')
  {
    int64_t size = 0;
    c = peek_char_and_size(cur_ptr, size);
    while (true)
    {
      if (c == '\\' && try_advance_identifier_ucn(lex, cur_ptr, size, result))
      {
        c = peek_char_and_size(cur_ptr, size);
        continue;
      }
      else if (!(is_nondigit(c) || is_digit(c)))
        break; // We're done.

      cur_ptr = consume_char(cur_ptr, size, result);
      c = peek_char_and_size(cur_ptr, size);

      // Handles escaped newlines and trigraphs.
      while (is_nondigit(c) || is_digit(c))
      {
        cur_ptr = consume_char(cur_ptr, size, result);
        c = peek_char_and_size(cur_ptr, size);
      }
    }
  }

  lex.form_token(result, cur_ptr, TokenKind::identifier);

  // FIXME: Comparing the raw identifier is wrong. This check
  // should be delayed to be later done by a sane identifier checker.
  if (!result.has_UCN() && !result.is_dirty())
  {
    // Changes the token's kind to a keyword if this happens to be one.
    const auto tok_spell = lex.source_mgr.text_slice(result.range);
    for (const TokenKind kw : KEYWORD_KINDS)
    {
      if (tok_spell == to_string(kw))
      {
        result.kind = kw;
        break;
      }
    }
  }

  return true;
}

// Lexes a numeric literal constant, i.e. integer-constant and
// floating-constant. Assumes that the first digit is already consumed.
//
// This just matches a regex that validates such constants. Syntax checking is
// delayed to semantic analyses.
//
// \param lex The lexer.
// \param cur_ptr Pointer past the first digit of the numeric constant.
// \param result Token being formed.
//
// \return true if numeric constant was successfully formed.
auto lex_numeric_constant(Lexer &lex, const char *cur_ptr, Token &result)
  -> bool
{
  int64_t digit_size = 0;
  char c = peek_char_and_size(cur_ptr, digit_size);
  char prev = c;

  // Matches the regex /[0-9_a-zA-Z.]*/.
  while (is_digit(c) || is_nondigit(c) || c == '.')
  {
    cur_ptr = consume_char(cur_ptr, digit_size, result);
    prev = c;
    c = peek_char_and_size(cur_ptr, digit_size);
  }

  // If we stumbled upon something that doesn't seem to be part of a numeric
  // constant, then check whether it's an exponent of a floating constant. If
  // so, continue lexing, otherwise finish the token.

  // exponent-part: [C11 6.4.4.2]
  //   'e' sign[opt] digit-sequence
  //   'E' sign[opt] digit-sequence
  if ((c == '+' || c == '-') || (prev == 'e' || prev == 'E'))
    return lex_numeric_constant(
      lex, consume_char(cur_ptr, digit_size, result), result);

  // binary-exponent-part:
  //    'p' sign[opt] digit-sequence
  //    'P' sign[opt] digit-sequence
  if ((c == '+' || c == '-') || (prev == 'p' || prev == 'P'))
    return lex_numeric_constant(
      lex, consume_char(cur_ptr, digit_size, result), result);

  // Found a possibly UCN, lex it and continue.
  if (c == '\\' && try_advance_identifier_ucn(lex, cur_ptr, digit_size, result))
    return lex_numeric_constant(lex, cur_ptr, result);

  lex.form_token(result, cur_ptr, TokenKind::numeric_constant);
  result.set_flags(Token::IsLiteral);
  return true;
}

// Skips a line comment, returning a pointer past the end of the comment.
// Assumes that the // part is already lexed.
//
// \param lex The lexer.
// \param cur_ptr Buffer pointer which points past the second '/' comment
//                character.
//
// \return A pointer past the end of the comment, i.e. the newline.
auto skip_line_comment(Lexer &lex, const char *cur_ptr) -> const char *
{
  int64_t c_size = 0;
  char c = peek_char_and_size(cur_ptr, c_size);

  // C11 6.4.9/2: Except within a character constant, a string literal, or a
  // comment, the characters // introduce a comment that includes all multibyte
  // characters up to, but not including, the next new-line character. The
  // contents of such a comment are examined only to identify multibyte
  // characters and to find the terminating new-line character.
  while (true)
  {
    if (is_newline(c))
    {
      cur_ptr += c_size;
      break; // We're done.
    }

    // End of input; ill-formed program. Even though this is assured to never
    // happen, we still let this check here.
    if (c == '\0')
    {
      report(lex, cur_ptr, diag::err_unterminated_comment, "line");
      break;
    }

    cur_ptr += c_size;
    c = peek_char_and_size(cur_ptr, c_size);
  }

  return cur_ptr;
}

// Skips a block comment, returning a pointer past the end of the comment, i.e.
// after the */ part. Assumes that the // part is already lexed.
//
// \param lex The lexer.
// \param cur_ptr Buffer pointer which points past the '*' from /*
//                string.
//
// \return A pointer past the end of the comment.
auto skip_block_comment(Lexer &lex, const char *cur_ptr) -> const char *
{
  int64_t c_size = 0;
  char c = peek_char_and_size(cur_ptr, c_size);
  char prev = c;

  // Could be recursive, but that might not be a good idea.  This also could be
  // improved upon. Right now it handles trigraphs and escaped newlines, which
  // does the job just fine.
  while (true)
  {
    // C11 6.4.9/1: Except within a character constant, a string literal, or a
    // comment, the characters /* introduce a comment. The contents of such a
    // comment are examined only to identify multibyte characters and to find the
    // characters */ that terminate it. 83)
    //
    // 83) Thus, /* ... */ comments do not nest.
    if (c == '/' && prev == '*')
    {
      cur_ptr += c_size;
      break; // We're done.
    }

    // Missing the terminating */ block comment.
    if (c == '\0')
    {
      report(lex, cur_ptr, diag::err_unterminated_comment, "block");
      break;
    }

    cur_ptr += c_size;
    prev = c;
    c = peek_char_and_size(cur_ptr, c_size);
  }

  return cur_ptr;
}

auto lex_character_constant(Lexer &lex, const char *cur_ptr, Token &result,
                            const TokenKind char_kind) -> bool
{
  cci_expects(char_kind == TokenKind::utf8_char_constant ||
              char_kind == TokenKind::utf16_char_constant ||
              char_kind == TokenKind::utf32_char_constant ||
              char_kind == TokenKind::wide_char_constant);

  char c = peek_char_advance(cur_ptr, result);

  if (c == '\'')
  {
    report(lex, lex.buffer_ptr, diag::err_empty_character);
    lex.form_token(result, cur_ptr, TokenKind::unknown);
    return true;
  }

  while (c != '\'')
  {
    // Skips this character for now. Decoding and checking of escape sequences
    // occur later on in semantic analyses.
    if (c == '\\')
      c = *cur_ptr++;

    else if (is_newline(c) || c == '\0')
    {
      report(lex, lex.buffer_ptr, diag::err_unterminated_char_const);
      lex.form_token(result, cur_ptr, TokenKind::unknown);
      return true;
    }

    c = peek_char_advance(cur_ptr, result);
  }

  lex.form_token(result, cur_ptr, char_kind);
  return true;
}

// Lexes the next token in the source buffer.
//
// \param lex The lexer.
// \param cur_ptr Pointer into the source buffer from which to lex a token.
// \param result Output parameter to which a lexed token is set on success.
//
// \return true if a token was lexed, and false if end of input is reached.
auto lex_token(Lexer &lex, const char *cur_ptr, Token &result) -> bool
{
  // Skips any whitespace before the token.
  cur_ptr = std::find_if(cur_ptr, lex.buffer_end, std::not_fn(is_whitespace));
  lex.buffer_ptr = cur_ptr;

  int64_t ch_size = 0;
  char ch = peek_char_and_size(cur_ptr, ch_size);
  cur_ptr = consume_char(cur_ptr, ch_size, result);

  auto kind = TokenKind::unknown;

  switch (ch)
  {
    case '\0':
      return false; // End of input.

    case '\\':
      // FIXME: This might be wrong. A UCN may represent a whitespace, or some
      // other code point that isn't allowed to appear as the first character
      // in an identifier.
      if (uint32_t code_point = try_read_ucn(lex, cur_ptr, lex.buffer_ptr, nullptr);
          code_point != 0)
        // cur_ptr now points past the UCN.
        return lex_identifier(lex, cur_ptr, result);
      else
        break;

    case '[':
      kind = TokenKind::l_bracket;
      break;
    case ']':
      kind = TokenKind::r_bracket;
      break;
    case '(':
      kind = TokenKind::l_paren;
      break;
    case ')':
      kind = TokenKind::r_paren;
      break;
    case '{':
      kind = TokenKind::l_brace;
      break;
    case '}':
      kind = TokenKind::r_brace;
      break;

    case '.':
      ch = peek_char_and_size(cur_ptr, ch_size);
      if (is_digit(ch))
        return lex_numeric_constant(lex, consume_char(cur_ptr, ch_size, result),
                                    result);
      else if (ch == '.')
      {
        if (int64_t after_size; peek_char_and_size(cur_ptr + ch_size, after_size) == '.')
        {
          kind = TokenKind::ellipsis;
          cur_ptr = consume_char(consume_char(cur_ptr, ch_size, result),
                                 after_size, result);
        }
      }
      else
        kind = TokenKind::period;
      break;

    case '-':
      ch = peek_char_and_size(cur_ptr, ch_size);
      if (ch == '>')
      {
        kind = TokenKind::arrow;
        cur_ptr = consume_char(cur_ptr, ch_size, result);
      }
      else if (ch == '-')
      {
        kind = TokenKind::minusminus;
        cur_ptr = consume_char(cur_ptr, ch_size, result);
      }
      else if (ch == '=')
      {
        kind = TokenKind::minusequal;
        cur_ptr = consume_char(cur_ptr, ch_size, result);
      }
      else
        kind = TokenKind::minus;
      break;

    case '+':
      ch = peek_char_and_size(cur_ptr, ch_size);
      if (ch == '+')
      {
        kind = TokenKind::plusplus;
        cur_ptr = consume_char(cur_ptr, ch_size, result);
      }
      else if (ch == '=')
      {
        kind = TokenKind::plusequal;
        cur_ptr = consume_char(cur_ptr, ch_size, result);
      }
      else
        kind = TokenKind::plus;
      break;

    case '&':
      ch = peek_char_and_size(cur_ptr, ch_size);
      if (ch == '&')
      {
        kind = TokenKind::ampamp;
        cur_ptr = consume_char(cur_ptr, ch_size, result);
      }
      else if (ch == '=')
      {
        kind = TokenKind::ampequal;
        cur_ptr = consume_char(cur_ptr, ch_size, result);
      }
      else
        kind = TokenKind::ampersand;
      break;

    case '*':
      ch = peek_char_and_size(cur_ptr, ch_size);
      if (ch == '=')
      {
        kind = TokenKind::starequal;
        cur_ptr = consume_char(cur_ptr, ch_size, result);
      }
      else
        kind = TokenKind::star;
      break;

    case '~':
      kind = TokenKind::tilde;
      break;

    case '/':
      ch = peek_char_and_size(cur_ptr, ch_size);
      if (ch == '/')
      {
        // NOTE: Don't handle line comments that are actually an operator and a
        // block comment in C89. E.g. `a //**/ b`, which should be `a / b` in
        // C89, but is currently parsed as `a`, because C11 has line comments.
        lex.buffer_ptr = skip_line_comment(lex, cur_ptr + ch_size);
        return lex_token(lex, lex.buffer_ptr, result);
      }
      else if (ch == '*')
      {
        lex.buffer_ptr = skip_block_comment(lex, cur_ptr + ch_size);
        return lex_token(lex, lex.buffer_ptr, result);
      }
      else if (ch == '=')
      {
        kind = TokenKind::slashequal;
        cur_ptr = consume_char(cur_ptr, ch_size, result);
      }
      else
        kind = TokenKind::slash;
      break;

    case '%':
      ch = peek_char_and_size(cur_ptr, ch_size);
      if (ch == '=')
      {
        kind = TokenKind::percentequal;
        cur_ptr = consume_char(cur_ptr, ch_size, result);
      }
      else if (ch == '>') // %> digraph.
      {
        kind = TokenKind::r_brace;
        cur_ptr = consume_char(cur_ptr, ch_size, result);
      }
      else if (ch == ':') // %: digraph.
      {
        cur_ptr = consume_char(cur_ptr, ch_size, result);
        if (int64_t after_size;
            peek_char_and_size(cur_ptr, ch_size) == '%' &&
            peek_char_and_size(cur_ptr + ch_size, after_size) == ':')
        {
          // %:%: digraph
          kind = TokenKind::hashhash;
          cur_ptr = consume_char(consume_char(cur_ptr, ch_size, result),
                                 after_size, result);
        }
        else
          kind = TokenKind::hash;
      }
      else
        kind = TokenKind::percent;
      break;

    case '<':
      ch = peek_char_and_size(cur_ptr, ch_size);
      if (ch == '<')
      {
        int64_t after_size;
        if (char after = peek_char_and_size(cur_ptr + ch_size, after_size);
            after == '=')
        {
          kind = TokenKind::lesslessequal;
          cur_ptr = consume_char(consume_char(cur_ptr, ch_size, result),
                                 after_size, result);
        }
        else
        {
          kind = TokenKind::lessless;
          cur_ptr = consume_char(cur_ptr, ch_size, result);
        }
      }
      else if (ch == '=')
      {
        kind = TokenKind::lessequal;
        cur_ptr = consume_char(cur_ptr, ch_size, result);
      }
      else if (ch == ':') // <: digraph.
      {
        kind = TokenKind::l_bracket;
        cur_ptr = consume_char(cur_ptr, ch_size, result);
      }
      else if (ch == '%') // <% digraph.
      {
        kind = TokenKind::l_brace;
        cur_ptr = consume_char(cur_ptr, ch_size, result);
      }
      else
        kind = TokenKind::less;
      break;

    case '>':
      ch = peek_char_and_size(cur_ptr, ch_size);
      if (ch == '>')
      {
        int64_t after_size;
        if (char after = peek_char_and_size(cur_ptr + ch_size, after_size);
            after == '=')
        {
          kind = TokenKind::greatergreaterequal;
          cur_ptr = consume_char(consume_char(cur_ptr, ch_size, result),
                                 after_size, result);
        }
        else
        {
          kind = TokenKind::greater;
          cur_ptr = consume_char(cur_ptr, ch_size, result);
        }
      }
      else if (ch == '=')
      {
        kind = TokenKind::greaterequal;
        cur_ptr = consume_char(cur_ptr, ch_size, result);
      }
      else
        kind = TokenKind::greater;
      break;

    case '=':
      ch = peek_char_and_size(cur_ptr, ch_size);
      if (ch == '=')
      {
        kind = TokenKind::equalequal;
        cur_ptr = consume_char(cur_ptr, ch_size, result);
      }
      else
        kind = TokenKind::equal;
      break;

    case '!':
      ch = peek_char_and_size(cur_ptr, ch_size);
      if (ch == '=')
      {
        kind = TokenKind::exclamaequal;
        cur_ptr = consume_char(cur_ptr, ch_size, result);
      }
      else
        kind = TokenKind::exclama;
      break;

    case '^':
      ch = peek_char_and_size(cur_ptr, ch_size);
      if (ch == '=')
      {
        kind = TokenKind::caretequal;
        cur_ptr = consume_char(cur_ptr, ch_size, result);
      }
      else
        kind = TokenKind::caret;
      break;

    case '|':
      ch = peek_char_and_size(cur_ptr, ch_size);
      if (ch == '|')
      {
        kind = TokenKind::pipepipe;
        cur_ptr = consume_char(cur_ptr, ch_size, result);
      }
      else if (ch == '=')
      {
        kind = TokenKind::pipeequal;
        cur_ptr = consume_char(cur_ptr, ch_size, result);
      }
      else
        kind = TokenKind::pipe;
      break;

    case '?':
      kind = TokenKind::question;
      break;

    case ':':
      ch = peek_char_and_size(cur_ptr, ch_size);
      if (ch == '>') // :> digraph.
      {
        kind = TokenKind::r_bracket;
        cur_ptr = consume_char(cur_ptr, ch_size, result);
      }
      else
        kind = TokenKind::colon;
      break;

    case ';':
      kind = TokenKind::semi;
      break;

    case ',':
      kind = TokenKind::comma;
      break;

    case '#':
      ch = peek_char_and_size(cur_ptr, ch_size);
      if (ch == '#')
      {
        kind = TokenKind::hashhash;
        cur_ptr = consume_char(cur_ptr, ch_size, result);
      }
      else
        kind = TokenKind::hash;
      break;

    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
      return lex_numeric_constant(lex, cur_ptr, result);

    case 'L':
      ch = peek_char_and_size(cur_ptr, ch_size);
      if (ch == '\'')
        return lex_character_constant(lex,
                                      consume_char(cur_ptr, ch_size, result),
                                      result, TokenKind::wide_char_constant);
      return lex_identifier(lex, cur_ptr, result);

    case 'u':
      ch = peek_char_and_size(cur_ptr, ch_size);
      if (ch == '\'')
        return lex_character_constant(lex,
                                      consume_char(cur_ptr, ch_size, result),
                                      result, TokenKind::utf16_char_constant);
      return lex_identifier(lex, cur_ptr, result);

    case 'U':
      ch = peek_char_and_size(cur_ptr, ch_size);
      if (ch == '\'')
        return lex_character_constant(lex,
                                      consume_char(cur_ptr, ch_size, result),
                                      result, TokenKind::utf32_char_constant);
      [[fallthrough]];

    case 'a': case 'b': case 'c': case 'd': case 'e': case 'f': case 'g':
    case 'h': case 'i': case 'j': case 'k': case 'l': case 'm': case 'n':
    case 'o': case 'p': case 'q': case 'r': case 's': case 't': /*   'u'*/
    case 'v': case 'w': case 'x': case 'y': case 'z': case 'A': case 'B':
    case 'C': case 'D': case 'E': case 'F': case 'G': case 'H': case 'I':
    case 'J': case 'K': /*   'L'*/case 'M': case 'N': case 'O': case 'P':
    case 'Q': case 'R': case 'S': case 'T': /*   'U'*/case 'V': case 'W':
    case 'X': case 'Y': case 'Z': case '_':
      return lex_identifier(lex, cur_ptr, result);

    case '\'':
      return lex_character_constant(lex, cur_ptr, result,
                                    TokenKind::utf8_char_constant);

    default:
      break;
  }

  if (kind == TokenKind::unknown)
    report(lex, lex.buffer_ptr, diag::err_unknown_character, ch);

  lex.form_token(result, cur_ptr, kind);
  return true;
}

} // namespace

// Lexes a token from the current pointer into the source buffer `buffer_ptr`.
// Most of the lexing occurs here.
//
// \param result The token being formed. When it's done lexing, `result` is set
//               to the produced token on success.
//
// \return true if a token was lexed, or false if it has hit the end of the
//              input.
auto Lexer::lex(Token &result) -> bool
{
  return lex_token(*this, buffer_ptr, result);
}

auto TokenStream::tokenize(const SourceManager &source_mgr) -> TokenStream
{
  return TokenStream(Lexer(source_mgr));
}

auto TokenStream::peek() -> Token
{
  if (!cur_tok)
  {
    if (Token tok; lexer.lex(tok))
      cur_tok = tok;
    else
      cur_tok = Token(TokenKind::eof, SourceLocation());
  }

  return *cur_tok;
}

auto TokenStream::consume() -> Token
{
  cci_expects(!empty());
  Token tok = peek();
  cur_tok.reset();
  return tok;
}

auto TokenStream::empty() -> bool
{
  return peek().is(TokenKind::eof);
}

auto to_string(TokenKind k) -> std::string_view
{
  switch (k)
  {
    case TokenKind::kw_auto: return "auto";
    case TokenKind::kw_break: return "break";
    case TokenKind::kw_case: return "case";
    case TokenKind::kw_char: return "char";
    case TokenKind::kw_const: return "const";
    case TokenKind::kw_continue: return "continue";
    case TokenKind::kw_default: return "default";
    case TokenKind::kw_do: return "do";
    case TokenKind::kw_double: return "double";
    case TokenKind::kw_else: return "else";
    case TokenKind::kw_enum: return "enum";
    case TokenKind::kw_extern: return "extern";
    case TokenKind::kw_float: return "float";
    case TokenKind::kw_for: return "for";
    case TokenKind::kw_goto: return "goto";
    case TokenKind::kw_if: return "if";
    case TokenKind::kw_inline: return "inline";
    case TokenKind::kw_int: return "int";
    case TokenKind::kw_long: return "long";
    case TokenKind::kw_register: return "register";
    case TokenKind::kw_restrict: return "restrict";
    case TokenKind::kw_return: return "return";
    case TokenKind::kw_short: return "short";
    case TokenKind::kw_signed: return "signed";
    case TokenKind::kw_sizeof: return "sizeof";
    case TokenKind::kw_static: return "static";
    case TokenKind::kw_struct: return "struct";
    case TokenKind::kw_switch: return "switch";
    case TokenKind::kw_typedef: return "typedef";
    case TokenKind::kw_union: return "union";
    case TokenKind::kw_unsigned: return "unsigned";
    case TokenKind::kw_void: return "void";
    case TokenKind::kw_volatile: return "volatile";
    case TokenKind::kw_while: return "while";
    case TokenKind::kw__Alignas: return "_Alignas";
    case TokenKind::kw__Alignof: return "_Alignof";
    case TokenKind::kw__Atomic: return "_Atomic";
    case TokenKind::kw__Bool: return "_Bool";
    case TokenKind::kw__Complex: return "_Complex";
    case TokenKind::kw__Generic: return "_Generic";
    case TokenKind::kw__Imaginary: return "_Imaginary";
    case TokenKind::kw__Noreturn: return "_Noreturn";
    case TokenKind::kw__Static_assert: return "_Static_assert";
    case TokenKind::kw__Thread_local: return "_Thread_local";
    case TokenKind::identifier: return "identifier";
    case TokenKind::numeric_constant: return "numeric constant";
    case TokenKind::utf8_char_constant: return "character constant";
    case TokenKind::utf16_char_constant: return "char16_t character constant";
    case TokenKind::utf32_char_constant: return "char32_t character constant";
    case TokenKind::wide_char_constant: return "wide character constant";
    case TokenKind::l_bracket: return "[";
    case TokenKind::r_bracket: return "]";
    case TokenKind::l_paren: return "(";
    case TokenKind::r_paren: return ")";
    case TokenKind::l_brace: return "{";
    case TokenKind::r_brace: return "}";
    case TokenKind::period: return ".";
    case TokenKind::arrow: return "->";
    case TokenKind::plusplus: return "++";
    case TokenKind::minusminus: return "--";
    case TokenKind::ampersand: return "&";
    case TokenKind::star: return "*";
    case TokenKind::plus: return "+";
    case TokenKind::minus: return "-";
    case TokenKind::tilde: return "~";
    case TokenKind::exclama: return "!";
    case TokenKind::slash: return "/";
    case TokenKind::percent: return "%";
    case TokenKind::lessless: return "<<";
    case TokenKind::greatergreater: return ">>";
    case TokenKind::less: return "<";
    case TokenKind::greater: return ">";
    case TokenKind::lesslessequal: return "<<=";
    case TokenKind::greatergreaterequal: return ">>=";
    case TokenKind::equalequal: return "==";
    case TokenKind::exclamaequal: return "!=";
    case TokenKind::caret: return "^";
    case TokenKind::pipe: return "|";
    case TokenKind::ampamp: return "&&";
    case TokenKind::pipepipe: return "||";
    case TokenKind::question: return "?";
    case TokenKind::colon: return ":";
    case TokenKind::semi: return ";";
    case TokenKind::ellipsis: return "...";
    case TokenKind::equal: return "=";
    case TokenKind::starequal: return "*=";
    case TokenKind::slashequal: return "/=";
    case TokenKind::percentequal: return "%=";
    case TokenKind::plusequal: return "+=";
    case TokenKind::minusequal: return "-=";
    case TokenKind::lessequal: return "<=";
    case TokenKind::greaterequal: return ">=";
    case TokenKind::ampequal: return "&=";
    case TokenKind::caretequal: return "^=";
    case TokenKind::pipeequal: return "|=";
    case TokenKind::comma: return ",";
    case TokenKind::hash: return "#";
    case TokenKind::hashhash: return "##";
    case TokenKind::unknown: return "<unknown>";
    case TokenKind::eof: return "<end of input>";
  }

  cci_unreachable();
}

} // namespace cci
