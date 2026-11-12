#pragma once

// Facet's tokens and syntax tree.

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace facet {

struct Location {
  int line = 1;
  int column = 1;
};

// A problem in the .facet source; main prints it with the offending line.
struct Error : std::runtime_error {
  Location at;
  Error(Location location, const std::string& message) : std::runtime_error(message), at(location) {}
};

enum class Token : std::uint8_t {
  end, identifier, number, string,
  // `cpp { … }`: the C++ between the braces, verbatim, in the token's text.
  cpp,
  // keywords
  let, in, as, view, for_, if_, then, else_, key, true_, false_, null, include,
  // punctuation
  lparen, rparen, lbrace, rbrace, lbracket, rbracket, comma, colon, dot, arrow, question,
  equal, not_equal, less, less_equal, greater, greater_equal, plus, minus, star, slash, percent,
  bang, and_, or_, assign, semicolon,
};

struct Lexeme {
  Token kind = Token::end;
  std::string text;
  Location at;
};

std::vector<Lexeme> lex(const std::string& source);
const char* describe(Token token);

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
  enum class Kind : std::uint8_t {
    number, string, boolean, null, identifier, member, index, call, unary, binary, conditional,
    lambda, object, array,
  };
  Kind kind;
  Location at;
  // number and string literal text, identifier and member names, operators.
  std::string text;
  // member/index/call: target first; unary/binary: operands; conditional:
  // condition, then, else; lambda: an expression body; array: the items.
  std::vector<ExprPtr> operands;
  std::vector<std::string> parameters;                     // lambda
  // object: its fields. A lambda with a { } body: its statements, where a
  // named entry is `let name = value` and an unnamed one an expression.
  std::vector<std::pair<std::string, ExprPtr>> fields;
  bool block = false;                                      // lambda with a { } body
  bool cpp_body = false;                                   // lambda with a cpp { } body:
  std::string code;                                        //   its C++, verbatim
  Location code_at;
};

struct Argument {
  std::string name;  // empty for a positional argument
  ExprPtr value;
  Location at;
};

struct Node;
using NodePtr = std::unique_ptr<Node>;
using Block = std::vector<NodePtr>;

struct Node {
  // render: `(index) => { … }` inside an element, e.g. a ListView row.
  // cpp: `cpp { … }` statements, verbatim, in `code`.
  enum class Kind : std::uint8_t { element, for_, if_, let, render, cpp };
  Kind kind;
  Location at;
  // element: `module.Name`, module empty for a bare name.
  std::string module;
  std::string name;  // element name, or the for/let variable
  std::vector<std::string> parameters;  // render
  std::vector<Argument> arguments;
  Block children;    // element children, for body, if branch
  ExprPtr value;     // for: iterable; if: condition; let: value
  ExprPtr key;       // for ... key
  Block otherwise;   // if: else branch
  bool has_else = false;
  std::string code;  // cpp
};

struct Import {
  std::string path;   // vw.ui
  std::string alias;  // ui
  Location at;
};

struct View {
  std::string name;
  std::vector<std::string> parameters;
  Block body;
  Location at;
};

// C++ written into the .facet file, emitted verbatim.
struct Code {
  std::string text;
  Location at;  // where the text starts
};

// let name = value at file level: a constant every view can use.
struct Constant {
  std::string name;
  ExprPtr value;
  Location at;
};

// properties(id: bar) { height: 36, … } at file level: properties for the
// element named `id: bar`. What the element sets itself wins.
struct Properties {
  std::string id;
  std::vector<Argument> arguments;
  Location at;
  bool used = false;
};

struct File {
  std::vector<Code> includes;  // include "x.hpp": the header names
  std::vector<Import> imports;
  std::vector<Code> prelude;   // file-level cpp { … }, before every view
  std::vector<Constant> constants;
  std::vector<Properties> properties;
  std::vector<View> views;
};

File parse(const std::string& source);

// C++ for every view in `file`; `source_name` goes into #line directives.
// Elements written without a module (Surface(…)) come from `default_module`,
// e.g. "JES.spectralium"; without one they are an error.
std::string generate(const File& file, const std::string& source_name, const std::string& namespace_name,
                     const std::string& default_module = {});

}  // namespace facet
