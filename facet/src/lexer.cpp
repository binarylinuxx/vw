#include "syntax.hpp"

#include <cctype>
#include <string_view>
#include <unordered_map>

namespace facet {
namespace {

bool identifier_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; }
bool identifier_part(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

const std::unordered_map<std::string_view, Token>& keywords() {
  static const std::unordered_map<std::string_view, Token> table{
      {"let", Token::let},       {"in", Token::in},       {"as", Token::as},
      {"view", Token::view},     {"for", Token::for_},    {"if", Token::if_},
      {"then", Token::then},     {"else", Token::else_},  {"key", Token::key},
      {"true", Token::true_},    {"false", Token::false_}, {"null", Token::null},
      {"include", Token::include},
  };
  return table;
}

}  // namespace

const char* describe(Token token) {
  switch (token) {
    case Token::end: return "end of file";
    case Token::identifier: return "a name";
    case Token::number: return "a number";
    case Token::string: return "a string";
    case Token::lparen: return "'('";
    case Token::rparen: return "')'";
    case Token::lbrace: return "'{'";
    case Token::rbrace: return "'}'";
    case Token::lbracket: return "'['";
    case Token::rbracket: return "']'";
    case Token::comma: return "','";
    case Token::colon: return "':'";
    case Token::dot: return "'.'";
    case Token::arrow: return "'=>'";
    case Token::assign: return "'='";
    case Token::in: return "'in'";
    case Token::as: return "'as'";
    case Token::then: return "'then'";
    case Token::else_: return "'else'";
    default: return "a token";
  }
}

std::vector<Lexeme> lex(const std::string& source) {
  std::vector<Lexeme> tokens;
  std::size_t at = 0;
  Location location;
  const auto peek = [&](std::size_t ahead = 0) { return at + ahead < source.size() ? source[at + ahead] : '\0'; };
  const auto advance = [&] {
    if (source[at] == '\n') {
      ++location.line;
      location.column = 1;
    } else {
      ++location.column;
    }
    ++at;
  };
  while (at < source.size()) {
    const char c = peek();
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      advance();
      continue;
    }
    if (c == '/' && peek(1) == '/') {
      while (at < source.size() && peek() != '\n') advance();
      continue;
    }
    if (c == '/' && peek(1) == '*') {
      const Location start = location;
      advance();
      advance();
      while (at < source.size() && !(peek() == '*' && peek(1) == '/')) advance();
      if (at >= source.size()) throw Error(start, "this comment never ends");
      advance();
      advance();
      continue;
    }
    const Location start = location;
    if (identifier_start(c)) {
      std::string word;
      // `space-between`: a dash between letters, with no spaces, stays in
      // the name. Write `a - b` for subtraction.
      while (identifier_part(peek()) ||
             (peek() == '-' && identifier_start(peek(1)) && !word.empty())) {
        word += peek();
        advance();
      }
      if (word == "cpp") {
        // cpp { … }: everything up to the matching brace, as C++ sees it:
        // braces in strings, character literals and comments do not count.
        std::size_t look = at;
        while (look < source.size() && std::isspace(static_cast<unsigned char>(source[look])) != 0) ++look;
        if (look < source.size() && source[look] == '{') {
          while (at <= look) advance();
          const Location body = location;
          const std::size_t begin = at;
          int depth = 1;
          while (at < source.size()) {
            const char next = peek();
            if (next == '/' && peek(1) == '/') {
              while (at < source.size() && peek() != '\n') advance();
            } else if (next == '/' && peek(1) == '*') {
              advance();
              advance();
              while (at < source.size() && !(peek() == '*' && peek(1) == '/')) advance();
              if (at < source.size()) { advance(); advance(); }
            } else if (next == '"' || next == '\'') {
              advance();
              while (at < source.size() && peek() != next && peek() != '\n') {
                if (peek() == '\\') advance();
                if (at < source.size()) advance();
              }
              if (at < source.size()) advance();
            } else {
              if (next == '{') ++depth;
              if (next == '}' && --depth == 0) break;
              advance();
            }
          }
          if (depth != 0) throw Error(start, "this cpp { } block never closes");
          tokens.push_back({Token::cpp, source.substr(begin, at - begin), body});
          advance();  // the closing brace
          continue;
        }
      }
      const auto keyword = keywords().find(word);
      tokens.push_back({keyword == keywords().end() ? Token::identifier : keyword->second, word, start});
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
      std::string number;
      while (std::isdigit(static_cast<unsigned char>(peek())) != 0 ||
             (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1))) != 0)) {
        number += peek();
        advance();
      }
      tokens.push_back({Token::number, number, start});
      continue;
    }
    if (c == '"') {
      std::string text;
      advance();
      while (peek() != '"') {
        if (at >= source.size() || peek() == '\n') throw Error(start, "this string never ends");
        if (peek() == '\\') {
          advance();
          switch (peek()) {
            case 'n': text += '\n'; break;
            case 't': text += '\t'; break;
            case '"': text += '"'; break;
            case '\\': text += '\\'; break;
            default: throw Error(location, std::string("unknown escape \\") + peek());
          }
        } else {
          text += peek();
        }
        advance();
      }
      advance();
      tokens.push_back({Token::string, text, start});
      continue;
    }
    const auto two = [&](char second) { return peek(1) == second; };
    Token kind = Token::end;
    int length = 1;
    switch (c) {
      case '(': kind = Token::lparen; break;
      case ')': kind = Token::rparen; break;
      case '{': kind = Token::lbrace; break;
      case '}': kind = Token::rbrace; break;
      case '[': kind = Token::lbracket; break;
      case ']': kind = Token::rbracket; break;
      case ',': kind = Token::comma; break;
      case ':': kind = Token::colon; break;
      case ';': kind = Token::semicolon; break;
      case '.': kind = Token::dot; break;
      case '?': kind = Token::question; break;
      case '+': kind = Token::plus; break;
      case '-': kind = Token::minus; break;
      case '*': kind = Token::star; break;
      case '/': kind = Token::slash; break;
      case '%': kind = Token::percent; break;
      case '=':
        if (two('=')) kind = Token::equal, length = 2;
        else if (two('>')) kind = Token::arrow, length = 2;
        else kind = Token::assign;
        break;
      case '!':
        if (two('=')) kind = Token::not_equal, length = 2;
        else kind = Token::bang;
        break;
      case '<':
        if (two('=')) kind = Token::less_equal, length = 2;
        else kind = Token::less;
        break;
      case '>':
        if (two('=')) kind = Token::greater_equal, length = 2;
        else kind = Token::greater;
        break;
      case '&':
        if (!two('&')) throw Error(start, "use && for 'and'");
        kind = Token::and_, length = 2;
        break;
      case '|':
        if (!two('|')) throw Error(start, "use || for 'or'");
        kind = Token::or_, length = 2;
        break;
      default: throw Error(start, std::string("unexpected character '") + c + "'");
    }
    tokens.push_back({kind, source.substr(at, static_cast<std::size_t>(length)), start});
    for (int index = 0; index < length; ++index) advance();
  }
  tokens.push_back({Token::end, "", location});
  return tokens;
}

}  // namespace facet
