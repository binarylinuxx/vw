#include "syntax.hpp"

namespace facet {
namespace {

class Parser {
 public:
  explicit Parser(std::vector<Lexeme> tokens) : tokens_(std::move(tokens)) {}

  File file() {
    File result;
    top_level(result);
    if (accept(Token::let)) {
      expect(Token::lbracket, "after 'let'");
      while (!at(Token::rbracket)) {
        Import import;
        import.at = current().at;
        import.path = expect(Token::identifier, "naming a module").text;
        while (accept(Token::dot)) import.path += "." + expect(Token::identifier, "after '.'").text;
        expect(Token::as, "after the module name");
        import.alias = expect(Token::identifier, "after 'as'").text;
        result.imports.push_back(std::move(import));
        if (!accept(Token::comma)) break;
      }
      expect(Token::rbracket, "to close the imports");
      expect(Token::in, "after the imports");
    }
    for (;;) {
      top_level(result);
      if (at(Token::end)) break;
      result.views.push_back(view());
    }
    return result;
  }

  // include "header.hpp" and file-level cpp { … }, before or between views.
  void top_level(File& result) {
    for (;;) {
      if (at(Token::include)) {
        ++position_;
        const Lexeme& header = expect(Token::string, "naming a header after 'include'");
        result.includes.push_back({header.text, header.at});
      } else if (at(Token::cpp)) {
        result.prelude.push_back({current().text, current().at});
        ++position_;
      } else {
        return;
      }
    }
  }

 private:
  const Lexeme& current() const { return tokens_[position_]; }
  const Lexeme& ahead(std::size_t offset) const {
    return tokens_[std::min(position_ + offset, tokens_.size() - 1)];
  }
  bool at(Token kind) const { return current().kind == kind; }
  bool accept(Token kind) {
    if (!at(kind)) return false;
    ++position_;
    return true;
  }
  const Lexeme& expect(Token kind, const char* where) {
    if (!at(kind))
      throw Error(current().at, std::string("expected ") + describe(kind) + " " + where + ", found " +
                                    (current().text.empty() ? describe(current().kind) : "'" + current().text + "'"));
    return tokens_[position_++];
  }

  View view() {
    View result;
    result.at = current().at;
    expect(Token::view, "to start a view");
    result.name = expect(Token::identifier, "naming the view").text;
    expect(Token::lparen, "after the view name");
    while (!at(Token::rparen)) {
      result.parameters.push_back(expect(Token::identifier, "naming a parameter").text);
      if (!accept(Token::comma)) break;
    }
    expect(Token::rparen, "to close the parameters");
    result.body = block();
    return result;
  }

  Block block() {
    expect(Token::lbrace, "to open a block");
    Block nodes;
    while (!at(Token::rbrace)) {
      if (at(Token::end)) throw Error(current().at, "this block never closes");
      nodes.push_back(statement());
    }
    expect(Token::rbrace, "to close the block");
    return nodes;
  }

  NodePtr statement() {
    auto node = std::make_unique<Node>();
    node->at = current().at;
    if (at(Token::cpp)) {
      node->kind = Node::Kind::cpp;
      node->code = current().text;
      ++position_;
      return node;
    }
    if (accept(Token::let)) {
      node->kind = Node::Kind::let;
      node->name = expect(Token::identifier, "after 'let'").text;
      expect(Token::assign, "after the name");
      node->value = expression();
      return node;
    }
    if (accept(Token::for_)) {
      node->kind = Node::Kind::for_;
      node->name = expect(Token::identifier, "after 'for'").text;
      expect(Token::in, "after the loop variable");
      node->value = expression();
      if (accept(Token::key)) node->key = expression();
      node->children = block();
      return node;
    }
    if (accept(Token::if_)) {
      node->kind = Node::Kind::if_;
      node->value = expression();
      node->children = block();
      if (accept(Token::else_)) {
        node->has_else = true;
        if (at(Token::if_)) node->otherwise.push_back(statement());
        else node->otherwise = block();
      }
      return node;
    }
    if (lambda_ahead()) {
      node->kind = Node::Kind::render;
      expect(Token::lparen, "");
      while (!at(Token::rparen)) {
        node->parameters.push_back(expect(Token::identifier, "naming a parameter").text);
        if (!accept(Token::comma)) break;
      }
      expect(Token::rparen, "to close the parameters");
      expect(Token::arrow, "after the parameters");
      node->children = block();
      return node;
    }
    node->kind = Node::Kind::element;
    node->name = expect(Token::identifier, "naming an element").text;
    if (accept(Token::dot)) {
      node->module = node->name;
      node->name = expect(Token::identifier, "after '.'").text;
    }
    expect(Token::lparen, "after the element name");
    while (!at(Token::rparen)) {
      Argument argument;
      argument.at = current().at;
      if (at(Token::identifier) && ahead(1).kind == Token::colon) {
        argument.name = current().text;
        position_ += 2;
      }
      argument.value = expression();
      node->arguments.push_back(std::move(argument));
      if (!accept(Token::comma)) break;
    }
    expect(Token::rparen, "to close the arguments");
    if (at(Token::lbrace)) node->children = block();
    return node;
  }

  static ExprPtr make(Expr::Kind kind, Location location, std::string text = {}) {
    auto expr = std::make_unique<Expr>();
    expr->kind = kind;
    expr->at = location;
    expr->text = std::move(text);
    return expr;
  }

  ExprPtr expression() { return conditional(); }

  ExprPtr conditional() {
    auto condition = binary(0);
    if (!at(Token::question)) return condition;
    auto result = make(Expr::Kind::conditional, current().at);
    ++position_;
    auto then = expression();
    expect(Token::colon, "in '? :'");
    auto otherwise = expression();
    result->operands.push_back(std::move(condition));
    result->operands.push_back(std::move(then));
    result->operands.push_back(std::move(otherwise));
    return result;
  }

  static int precedence(Token kind) {
    switch (kind) {
      case Token::or_: return 1;
      case Token::and_: return 2;
      case Token::equal: case Token::not_equal: return 3;
      case Token::less: case Token::less_equal: case Token::greater: case Token::greater_equal: return 4;
      case Token::plus: case Token::minus: return 5;
      case Token::star: case Token::slash: case Token::percent: return 6;
      default: return 0;
    }
  }

  ExprPtr binary(int minimum) {
    auto left = unary();
    while (precedence(current().kind) > minimum) {
      const int level = precedence(current().kind);
      auto node = make(Expr::Kind::binary, current().at, current().text);
      ++position_;
      node->operands.push_back(std::move(left));
      node->operands.push_back(binary(level));
      left = std::move(node);
    }
    return left;
  }

  ExprPtr unary() {
    if (at(Token::bang) || at(Token::minus)) {
      auto node = make(Expr::Kind::unary, current().at, current().text);
      ++position_;
      node->operands.push_back(unary());
      return node;
    }
    return postfix(primary());
  }

  ExprPtr postfix(ExprPtr target) {
    for (;;) {
      if (at(Token::dot)) {
        ++position_;
        auto node = make(Expr::Kind::member, current().at, expect(Token::identifier, "after '.'").text);
        node->operands.push_back(std::move(target));
        target = std::move(node);
      } else if (at(Token::lbracket)) {
        auto node = make(Expr::Kind::index, current().at);
        ++position_;
        node->operands.push_back(std::move(target));
        node->operands.push_back(expression());
        expect(Token::rbracket, "to close the index");
        target = std::move(node);
      } else if (at(Token::lparen)) {
        auto node = make(Expr::Kind::call, current().at);
        ++position_;
        node->operands.push_back(std::move(target));
        while (!at(Token::rparen)) {
          if (at(Token::identifier) && ahead(1).kind == Token::colon)
            throw Error(current().at, "calls take positional arguments only");
          node->operands.push_back(expression());
          if (!accept(Token::comma)) break;
        }
        expect(Token::rparen, "to close the call");
        target = std::move(node);
      } else {
        return target;
      }
    }
  }

  // `(a, b) =>` or `() =>`: a lambda, checked before reading a parenthesis.
  bool lambda_ahead() const {
    if (!at(Token::lparen)) return false;
    std::size_t offset = 1;
    while (ahead(offset).kind == Token::identifier || ahead(offset).kind == Token::comma) ++offset;
    return ahead(offset).kind == Token::rparen && ahead(offset + 1).kind == Token::arrow;
  }

  ExprPtr primary() {
    const Lexeme& token = current();
    switch (token.kind) {
      case Token::number: ++position_; return make(Expr::Kind::number, token.at, token.text);
      case Token::string: ++position_; return make(Expr::Kind::string, token.at, token.text);
      case Token::true_: case Token::false_:
        ++position_;
        return make(Expr::Kind::boolean, token.at, token.text);
      case Token::null: ++position_; return make(Expr::Kind::null, token.at);
      case Token::identifier: ++position_; return make(Expr::Kind::identifier, token.at, token.text);
      case Token::if_: {
        // if c then a else b
        auto node = make(Expr::Kind::conditional, token.at);
        ++position_;
        node->operands.push_back(expression());
        expect(Token::then, "in 'if … then … else'");
        node->operands.push_back(expression());
        expect(Token::else_, "in 'if … then … else'");
        node->operands.push_back(expression());
        return node;
      }
      case Token::lbrace: {
        auto node = make(Expr::Kind::object, token.at);
        ++position_;
        while (!at(Token::rbrace)) {
          const std::string name = expect(Token::identifier, "naming a field").text;
          expect(Token::colon, "after the field name");
          node->fields.emplace_back(name, expression());
          if (!accept(Token::comma)) break;
        }
        expect(Token::rbrace, "to close the object");
        return node;
      }
      case Token::lbracket: {
        auto node = make(Expr::Kind::array, token.at);
        ++position_;
        while (!at(Token::rbracket)) {
          node->operands.push_back(expression());
          if (!accept(Token::comma)) break;
        }
        expect(Token::rbracket, "to close the list");
        return node;
      }
      case Token::lparen: {
        if (lambda_ahead()) {
          auto node = make(Expr::Kind::lambda, token.at);
          ++position_;
          while (!at(Token::rparen)) {
            node->parameters.push_back(expect(Token::identifier, "naming a parameter").text);
            if (!accept(Token::comma)) break;
          }
          expect(Token::rparen, "to close the parameters");
          expect(Token::arrow, "after the parameters");
          if (at(Token::cpp)) {
            node->cpp_body = true;
            node->code = current().text;
            node->code_at = current().at;
            ++position_;
            return node;
          }
          if (accept(Token::lbrace)) {
            // { let x = …; actions.a(x); actions.b() }: statements, in order.
            node->block = true;
            while (!at(Token::rbrace)) {
              if (at(Token::end)) throw Error(current().at, "this lambda body never closes");
              if (accept(Token::let)) {
                const std::string name = expect(Token::identifier, "after 'let'").text;
                expect(Token::assign, "after the name");
                node->fields.emplace_back(name, expression());
              } else {
                node->fields.emplace_back("", expression());
              }
              (void)accept(Token::semicolon);
            }
            expect(Token::rbrace, "to close the lambda body");
            return node;
          }
          node->operands.push_back(expression());
          return node;
        }
        ++position_;
        auto inner = expression();
        expect(Token::rparen, "to close the parenthesis");
        return inner;
      }
      default:
        throw Error(token.at, "expected a value, found " +
                                  (token.text.empty() ? std::string(describe(token.kind)) : "'" + token.text + "'"));
    }
  }

  std::vector<Lexeme> tokens_;
  std::size_t position_ = 0;
};

}  // namespace

File parse(const std::string& source) { return Parser(lex(source)).file(); }

}  // namespace facet
