// Facet, interpreted: walks the syntax tree facetc compiles and builds the
// same widgets through the same helpers (vw/facet.hpp), so a view behaves the
// same either way. Differences: numbers are doubles (1 / 2 is 0.5), and a
// plugin reaches only what the host hands it, never C++.
#include "vw/facet_runtime.hpp"

#include <vw/facet.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <utility>

#include "syntax.hpp"

namespace facet {

// --- Value -------------------------------------------------------------------

namespace {

[[noreturn]] void wrong(const Value& value, std::string_view wanted) {
  throw std::runtime_error(std::format("expected {}, got {}", wanted, value.kind()));
}

std::string hex_color(const vw::Color& color) {
  std::string result = "#";
  for (const float channel : color)
    result += std::format("{:02x}", static_cast<int>(std::lround(std::clamp(channel, 0.0F, 1.0F) * 255.0F)));
  return result;
}

}  // namespace

double Value::number() const {
  if (const auto* found = std::get_if<double>(&data_)) return *found;
  wrong(*this, "a number");
}
bool Value::boolean() const {
  if (const auto* found = std::get_if<bool>(&data_)) return *found;
  wrong(*this, "true or false");
}
const std::string& Value::text() const {
  if (const auto* found = std::get_if<std::string>(&data_)) return *found;
  wrong(*this, "text");
}
const vw::Color& Value::color() const {
  if (const auto* found = std::get_if<vw::Color>(&data_)) return *found;
  wrong(*this, "a color");
}
const Value::List& Value::list() const {
  if (const auto* found = std::get_if<std::shared_ptr<const List>>(&data_)) return **found;
  wrong(*this, "a list");
}
const Value::Object& Value::object() const {
  if (const auto* found = std::get_if<std::shared_ptr<const Object>>(&data_)) return **found;
  wrong(*this, "an object");
}
const Function& Value::function() const {
  if (const auto* found = std::get_if<Function>(&data_)) return *found;
  wrong(*this, "a function");
}

const Value& Value::operator[](std::string_view field) const {
  static const Value none;
  if (!is_object()) return none;
  const auto& fields = object();
  const auto found = fields.find(field);
  return found == fields.end() ? none : found->second;
}

bool Value::truthy() const {
  if (is_null()) return false;
  if (is_number()) return number() != 0.0;
  if (is_bool()) return boolean();
  if (is_text()) return !text().empty();
  return true;
}

std::string Value::to_string() const {
  if (is_null()) return "null";
  if (is_number()) return std::format("{}", number());
  if (is_bool()) return boolean() ? "true" : "false";
  if (is_text()) return text();
  if (is_color()) return hex_color(color());
  if (is_list()) {
    std::string result = "[";
    for (const Value& item : list()) result += (result.size() > 1 ? ", " : "") + item.to_string();
    return result + "]";
  }
  if (is_object()) return "{…}";
  return "function";
}

std::string_view Value::kind() const {
  if (is_null()) return "null";
  if (is_number()) return "a number";
  if (is_bool()) return "a boolean";
  if (is_text()) return "text";
  if (is_color()) return "a color";
  if (is_list()) return "a list";
  if (is_object()) return "an object";
  return "a function";
}

bool Value::operator==(const Value& other) const {
  if (data_.index() != other.data_.index()) return false;
  if (is_null()) return true;
  if (is_number()) return number() == other.number();
  if (is_bool()) return boolean() == other.boolean();
  if (is_text()) return text() == other.text();
  if (is_color()) return color() == other.color();
  if (is_list()) return list() == other.list();
  if (is_object()) return object() == other.object();
  return false;  // functions
}

ScriptError::ScriptError(std::string file_name, int line_number, int column_number, std::string text)
    : std::runtime_error(std::format("{}:{}:{}: {}", file_name, line_number, column_number, text)),
      file(std::move(file_name)), line(line_number), column(column_number), message(std::move(text)) {}

// --- the program ---------------------------------------------------------------

namespace {

using Report = std::shared_ptr<std::function<void(const std::exception&)>>;

struct Program {
  File file;
  std::string name;
  std::filesystem::path path;
  std::filesystem::file_time_type modified{};
  std::map<std::string, const View*, std::less<>> views;
  std::map<std::string, std::string, std::less<>> modules;  // alias → module path
};

[[noreturn]] void fail(const Program& program, Location at, const std::string& message) {
  throw ScriptError(program.name, at.line, at.column, message);
}

constexpr std::string_view no_cpp =
    "plugins cannot run C++; compile this file with facetc instead, or move the C++ into the host";

void check_expression(const Program& program, const Expr& expr) {
  if (expr.cpp_body) fail(program, expr.code_at, "a cpp { } body: " + std::string(no_cpp));
  for (const auto& operand : expr.operands) check_expression(program, *operand);
  for (const auto& [name, field] : expr.fields) check_expression(program, *field);
}

void check_block(const Program& program, const Block& nodes) {
  for (const auto& node : nodes) {
    if (node->kind == Node::Kind::cpp) fail(program, node->at, "a cpp { } block: " + std::string(no_cpp));
    for (const auto& argument : node->arguments) check_expression(program, *argument.value);
    if (node->value) check_expression(program, *node->value);
    if (node->key) check_expression(program, *node->key);
    check_block(program, node->children);
    check_block(program, node->otherwise);
  }
}

std::shared_ptr<Program> compile_program(std::string source, std::string name) {
  auto program = std::make_shared<Program>();
  program->name = std::move(name);
  try {
    program->file = parse(source);
  } catch (const Error& error) {
    throw ScriptError(program->name, error.at.line, error.at.column, error.what());
  }
  const File& file = program->file;
  if (!file.includes.empty())
    fail(*program, file.includes.front().at, "include brings in C++ headers: " + std::string(no_cpp));
  if (!file.prelude.empty()) fail(*program, file.prelude.front().at, "a cpp { } block: " + std::string(no_cpp));
  for (const auto& constant : file.constants) check_expression(*program, *constant.value);
  for (const auto& view : file.views) {
    check_block(*program, view.body);
    if (!program->views.emplace(view.name, &view).second)
      fail(*program, view.at, "view '" + view.name + "' is declared twice");
  }
  for (const auto& import : file.imports) program->modules[import.alias] = import.path;
  return program;
}

// --- scopes ----------------------------------------------------------------------

struct Scope {
  Value::Object names;
  std::shared_ptr<const Scope> parent;

  const Value* find(std::string_view name) const {
    for (const Scope* scope = this; scope != nullptr; scope = scope->parent.get())
      if (const auto found = scope->names.find(name); found != scope->names.end()) return &found->second;
    return nullptr;
  }
};
using ScopePtr = std::shared_ptr<Scope>;

ScopePtr child_of(const ScopePtr& parent) {
  auto scope = std::make_shared<Scope>();
  scope->parent = parent;
  return scope;
}

enum class Seen : std::uint8_t { once, looped, repeated };

// What a view knows while it draws, shared with the handlers it leaves behind.
struct ViewContext {
  std::string view;
  std::map<std::string, Seen, std::less<>> ids;
};

void collect_ids(const Block& nodes, bool looped, std::map<std::string, Seen, std::less<>>& ids) {
  for (const auto& node : nodes) {
    if (node->kind == Node::Kind::element)
      for (const auto& argument : node->arguments)
        if (argument.name == "id" && argument.value->kind == Expr::Kind::identifier) {
          const auto [entry, added] = ids.emplace(argument.value->text, looped ? Seen::looped : Seen::once);
          if (!added) entry->second = Seen::repeated;
        }
    const bool inner = looped || node->kind == Node::Kind::for_ || node->kind == Node::Kind::render;
    collect_ids(node->children, inner, ids);
    collect_ids(node->otherwise, inner, ids);
  }
}

// --- expressions ---------------------------------------------------------------------

Value box_value(float x, float y, float width, float height, bool built, Value props = {}) {
  return Value::Object{{"x", x}, {"y", y}, {"width", width}, {"height", height}, {"built", built},
                       {"props", std::move(props)}};
}

Value screen_value() {
  const Screen screen = facet::screen();
  return Value::Object{{"name", screen.name},   {"description", screen.description},
                       {"x", screen.x},         {"y", screen.y},
                       {"width", screen.width}, {"height", screen.height},
                       {"scale", screen.scale}, {"refresh", screen.refresh}};
}

std::optional<vw::Color> parse_color(std::string_view text) {
  if (text == "transparent") return vw::rgba(0x00000000);
  if ((text.size() != 7 && text.size() != 9) || text.front() != '#') return std::nullopt;
  std::uint32_t value = 0;
  const auto digits = text.substr(1);
  const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value, 16);
  if (error != std::errc{} || end != digits.data() + digits.size()) return std::nullopt;
  return text.size() == 7 ? vw::rgb(value) : vw::rgba(value);
}

std::string event_key(const Program& program, Location at, const std::vector<std::string>& keys) {
  std::string result = std::format("{}:{}:{}", program.name, at.line, at.column);
  for (const auto& key : keys) result += '|' + key;
  return result;
}

const std::set<std::string, std::less<>>& easings() {
  static const std::set<std::string, std::less<>> names{"linear",   "in_out_cubic", "out_cubic", "out_quint",
                                                        "out_expo", "out_back",     "spring"};
  return names;
}

vw::Easing easing_named(std::string_view name) {
  if (name == "linear") return vw::Easing::linear;
  if (name == "in_out_cubic") return vw::Easing::in_out_cubic;
  if (name == "out_quint") return vw::Easing::out_quint;
  if (name == "out_expo") return vw::Easing::out_expo;
  if (name == "out_back") return vw::Easing::out_back;
  if (name == "spring") return vw::Easing::spring;
  return vw::Easing::out_cubic;
}

// Evaluates expressions. Copied into every handler and lambda, so it keeps
// the program (and with it the syntax tree) alive as long as they live.
struct Evaluator {
  std::shared_ptr<const Program> program;
  std::shared_ptr<const ViewContext> context;
  std::vector<std::string> keys;  // the loops around the expression

  [[noreturn]] void error(Location at, const std::string& message) const { fail(*program, at, message); }

  // Runs `work`, putting the expression's place on errors that lack one.
  template <class Work>
  auto at(Location where, Work&& work) const -> decltype(work()) {
    try {
      return work();
    } catch (const ScriptError&) {
      throw;
    } catch (const std::exception& failure) {
      error(where, failure.what());
    }
  }

  Value call(const Value& function, Arguments arguments, Location where) const {
    return at(where, [&] { return function.function()(arguments); });
  }

  Value identifier(const Expr& expr, const ScopePtr& scope) const {
    if (const Value* found = scope->find(expr.text)) return *found;
    if (expr.text == "screen") return screen_value();
    if (context) {
      if (const auto found = context->ids.find(expr.text); found != context->ids.end()) {
        if (found->second != Seen::once)
          error(expr.at, std::format("'{}' is {}, so it cannot be read before or outside it", expr.text,
                                     found->second == Seen::looped ? "named inside a loop" : "named more than once"));
        const Box box = previous(std::format("{}:{}.{}", program->name, context->view, expr.text));
        return box_value(box.x, box.y, box.width, box.height, box.built);
      }
    }
    error(expr.at, std::format("'{}' is not defined here", expr.text));
  }

  Value evaluate(const Expr& expr, const ScopePtr& scope) const {
    using K = Expr::Kind;
    switch (expr.kind) {
      case K::number: {
        // from_chars, not stod: the locale must not change what 1.5 means.
        double number = 0;
        const auto [end, failure] = std::from_chars(expr.text.data(), expr.text.data() + expr.text.size(), number);
        if (failure != std::errc{} || end != expr.text.data() + expr.text.size())
          error(expr.at, "'" + expr.text + "' is not a number");
        return Value(number);
      }
      case K::string: return Value(expr.text);
      case K::boolean: return Value(expr.text == "true");
      case K::null: return Value();
      case K::identifier: return identifier(expr, scope);
      case K::member: {
        const Value target = evaluate(*expr.operands[0], scope);
        if (expr.text == "length") {
          if (target.is_list()) return Value(target.list().size());
          if (target.is_text()) return Value(target.text().size());
        }
        if (!target.is_object()) error(expr.at, std::format("'.{}' of {}", expr.text, target.kind()));
        const auto& fields = target.object();
        const auto found = fields.find(expr.text);
        if (found == fields.end()) error(expr.at, std::format("no field '{}' here", expr.text));
        return found->second;
      }
      case K::index: {
        const Value target = evaluate(*expr.operands[0], scope);
        const Value index = evaluate(*expr.operands[1], scope);
        return at(expr.at, [&]() -> Value {
          if (target.is_object()) return target[index.to_string()];
          const double position = index.number();
          const auto size = target.is_text() ? target.text().size() : target.list().size();
          if (position < 0 || position >= static_cast<double>(size) || std::floor(position) != position)
            throw std::runtime_error(std::format("index {} is outside 0..{}", position, size));
          const auto offset = static_cast<std::size_t>(position);
          if (target.is_text()) return Value(target.text().substr(offset, 1));
          return target.list()[offset];
        });
      }
      case K::call: {
        const Expr& callee = *expr.operands[0];
        if (callee.kind == K::identifier && callee.text == "animate" && scope->find("animate") == nullptr)
          return animate(expr, scope);
        const Value function = evaluate(callee, scope);
        std::vector<Value> arguments;
        for (std::size_t index = 1; index < expr.operands.size(); ++index)
          arguments.push_back(evaluate(*expr.operands[index], scope));
        return call(function, arguments, expr.at);
      }
      case K::unary: {
        const Value operand = evaluate(*expr.operands[0], scope);
        if (expr.text == "!") return Value(!operand.truthy());
        if (expr.text == "-") return at(expr.at, [&] { return Value(-operand.number()); });
        return at(expr.at, [&] { return Value(operand.number()); });
      }
      case K::binary: return binary(expr, scope);
      case K::conditional:
        return evaluate(*expr.operands[evaluate(*expr.operands[0], scope).truthy() ? 1 : 2], scope);
      case K::lambda: return lambda(expr, scope);
      case K::array: {
        Value::List items;
        for (const auto& item : expr.operands) items.push_back(evaluate(*item, scope));
        return Value(std::move(items));
      }
      case K::object: {
        Value::Object fields;
        for (const auto& [name, field] : expr.fields) fields[name] = evaluate(*field, scope);
        return Value(std::move(fields));
      }
    }
    return Value();
  }

  Value binary(const Expr& expr, const ScopePtr& scope) const {
    const std::string& op = expr.text;
    if (op == "&&") return Value(evaluate(*expr.operands[0], scope).truthy() && evaluate(*expr.operands[1], scope).truthy());
    if (op == "||") return Value(evaluate(*expr.operands[0], scope).truthy() || evaluate(*expr.operands[1], scope).truthy());
    const Value left = evaluate(*expr.operands[0], scope);
    const Value right = evaluate(*expr.operands[1], scope);
    if (op == "==") return Value(left == right);
    if (op == "!=") return Value(!(left == right));
    return at(expr.at, [&]() -> Value {
      if (op == "+") {
        if (left.is_text() || right.is_text()) return Value(left.to_string() + right.to_string());
        return Value(left.number() + right.number());
      }
      if (left.is_text() && right.is_text()) {
        if (op == "<") return Value(left.text() < right.text());
        if (op == "<=") return Value(left.text() <= right.text());
        if (op == ">") return Value(left.text() > right.text());
        if (op == ">=") return Value(left.text() >= right.text());
      }
      const double a = left.number();
      const double b = right.number();
      if (op == "-") return Value(a - b);
      if (op == "*") return Value(a * b);
      if (op == "/") return Value(a / b);
      if (op == "%") return Value(std::fmod(a, b));
      if (op == "<") return Value(a < b);
      if (op == "<=") return Value(a <= b);
      if (op == ">") return Value(a > b);
      if (op == ">=") return Value(a >= b);
      throw std::runtime_error("unknown operator " + op);
    });
  }

  Value lambda(const Expr& expr, const ScopePtr& scope) const {
    const Evaluator self = *this;
    const Expr* body = &expr;
    return Value(Function([self, body, scope](Arguments arguments) -> Value {
      const ScopePtr inner = child_of(scope);
      for (std::size_t index = 0; index < body->parameters.size(); ++index)
        inner->names[body->parameters[index]] = index < arguments.size() ? arguments[index] : Value();
      if (body->block) {
        for (const auto& [name, item] : body->fields) {
          Value value = self.evaluate(*item, inner);
          if (!name.empty()) inner->names[name] = std::move(value);
        }
        return Value();
      }
      return self.evaluate(*body->operands[0], inner);
    }));
  }

  // animate(target, milliseconds[, easing]): a tween keyed by this place.
  Value animate(const Expr& call, const ScopePtr& scope) const {
    const std::size_t count = call.operands.size() - 1;
    if (count < 2 || count > 3) error(call.at, "animate(target, milliseconds[, easing])");
    vw::Easing easing = vw::Easing::out_cubic;
    if (count == 3) {
      const Expr& name = *call.operands[3];
      if (name.kind != Expr::Kind::identifier || !easings().contains(name.text))
        error(name.at, "easing is one of: linear, in_out_cubic, out_cubic, out_quint, out_expo, out_back, spring");
      easing = easing_named(name.text);
    }
    const double target = at(call.at, [&] { return evaluate(*call.operands[1], scope).number(); });
    const double duration = at(call.at, [&] { return evaluate(*call.operands[2], scope).number(); });
    return Value(tween(event_key(*program, call.at, keys), target, duration, easing));
  }

  // A handler runs when the event comes: a lambda gets the event, a call
  // (`on_click: actions.select(id)`) runs as written, a function reference
  // gets the event.
  Function handler(const Expr& expr, const ScopePtr& scope) const {
    const Evaluator self = *this;
    const Expr* body = &expr;
    if (expr.kind == Expr::Kind::lambda) {
      const Value function = lambda(expr, scope);
      return function.function();
    }
    if (expr.kind == Expr::Kind::call)
      return [self, body, scope](Arguments) -> Value { return self.evaluate(*body, scope); };
    return [self, body, scope](Arguments arguments) -> Value {
      return self.call(self.evaluate(*body, scope), arguments, body->at);
    };
  }
};

// --- properties ----------------------------------------------------------------------

enum class Type {
  number, integer, size, boolean, string, strings, color, handler, input, padding,
  anchor, layer, justify, align, weight, fit, text_align, gradient, color_depth,
};

enum class Kind {
  surface, rectangle, text, image, text_field, toggle, slider, dropdown, spinner,
  row, column, grid, listview,
};

struct Property {
  std::string_view name;
  Type type;
};

struct ElementSpec {
  std::string_view module;
  std::string_view name;
  Kind kind;
  std::vector<Property> properties;
  std::string_view positional;
};

// The same elements and properties as facetc (generator.cpp).
const std::vector<ElementSpec>& elements() {
  using enum Type;
  static const std::vector<Property> layout{{"gap", number},  {"padding", padding}, {"width", number},
                                            {"height", number}, {"justify", justify}, {"align", align},
                                            {"radius", number}};
  static const std::vector<ElementSpec> table{
      {"vw.ui", "Surface", Kind::surface,
       {{"width", number}, {"height", number}, {"anchor", anchor}, {"layer", layer},
        {"exclusive_zone", integer}, {"namespace", string}, {"refresh", number}, {"output", string},
        {"color_depth", color_depth}}, ""},
      {"vw.ui", "Rectangle", Kind::rectangle,
       {{"width", number}, {"height", number}, {"radius", number}, {"color", color}, {"bg", color},
        {"color2", color}, {"gradient", gradient}, {"border_width", number}, {"border_color", color},
        {"input", input}}, ""},
      {"vw.ui", "Text", Kind::text,
       {{"text", string}, {"font", string}, {"size", number}, {"weight", weight}, {"color", color},
        {"max_width", number}, {"max_lines", integer}, {"align", text_align}, {"padding", padding},
        {"pixel_snap", boolean}, {"input", input}}, "text"},
      {"vw.ui", "Image", Kind::image,
       {{"path", string}, {"icon", string}, {"width", number}, {"height", number}, {"fit", fit},
        {"radius", number}, {"padding", padding}, {"input", input}}, "path"},
      {"vw.ui", "TextField", Kind::text_field,
       {{"id", string}, {"value", string}, {"placeholder", string}, {"width", number}, {"height", number},
        {"radius", number}, {"color", color}, {"border_color", color}, {"focus_color", color},
        {"text_color", color}, {"placeholder_color", color}, {"font", string}, {"size", number},
        {"password", boolean}, {"autofocus", boolean}, {"on_change", handler}, {"on_submit", handler}}, ""},
      {"vw.ui", "Toggle", Kind::toggle,
       {{"id", string}, {"value", boolean}, {"width", number}, {"height", number}, {"color", color},
        {"off_color", color}, {"thumb_color", color}, {"on_change", handler}}, ""},
      {"vw.ui", "Slider", Kind::slider,
       {{"id", string}, {"value", number}, {"min", number}, {"max", number}, {"step", number},
        {"width", number}, {"height", number}, {"color", color}, {"track_color", color},
        {"thumb_color", color}, {"on_change", handler}}, ""},
      {"vw.ui", "Dropdown", Kind::dropdown,
       {{"id", string}, {"options", strings}, {"current", string}, {"value", string}, {"width", number},
        {"height", number}, {"color", color}, {"hover_color", color}, {"text_color", color},
        {"on_change", handler}}, ""},
      {"vw.ui", "Spinner", Kind::spinner, {{"id", string}, {"size", number}, {"color", color}}, ""},
      {"vw.layout", "Row", Kind::row, layout, ""},
      {"vw.layout", "Column", Kind::column, layout, ""},
      {"vw.layout", "Grid", Kind::grid,
       {{"columns", integer}, {"gap", number}, {"row_gap", number}, {"padding", padding}, {"width", number},
        {"height", number}, {"align", align}, {"radius", number}}, ""},
      {"vw.layout", "ListView", Kind::listview,
       {{"id", string}, {"width", number}, {"height", number}, {"item_height", number}, {"gap", number},
        {"radius", number}, {"scroll_step", number}, {"count", size}}, ""},
  };
  return table;
}

const std::map<std::string, std::uint32_t, std::less<>>& anchors() {
  static const std::map<std::string, std::uint32_t, std::less<>> table{
      {"center", vw::anchor::center},     {"top", vw::anchor::top},
      {"bottom", vw::anchor::bottom},     {"left", vw::anchor::left},
      {"right", vw::anchor::right},       {"top_left", vw::anchor::top_left},
      {"top_right", vw::anchor::top_right}, {"bottom_left", vw::anchor::bottom_left},
      {"bottom_right", vw::anchor::bottom_right}, {"fill", vw::anchor::fill}};
  return table;
}

// align: on a Text naming a place in its parent rather than a line alignment.
const std::map<std::string, std::uint32_t, std::less<>>& placements() {
  static const std::map<std::string, std::uint32_t, std::less<>> table{
      {"left", vw::anchor::left},          {"right", vw::anchor::right},
      {"top", vw::anchor::top},            {"bottom", vw::anchor::bottom},
      {"center_left", vw::anchor::left},   {"center_right", vw::anchor::right},
      {"top_center", vw::anchor::top},     {"bottom_center", vw::anchor::bottom},
      {"top_left", vw::anchor::top_left},  {"top_right", vw::anchor::top_right},
      {"bottom_left", vw::anchor::bottom_left}, {"bottom_right", vw::anchor::bottom_right}};
  return table;
}

const std::vector<std::string_view>& choices(Type type) {
  static const std::map<Type, std::vector<std::string_view>> table{
      {Type::anchor, {"center", "top", "bottom", "left", "right", "top_left", "top_right", "bottom_left",
                      "bottom_right", "fill"}},
      {Type::layer, {"background", "bottom", "top", "overlay"}},
      {Type::justify, {"start", "center", "end", "space-between", "space-around", "space-evenly"}},
      {Type::align, {"start", "center", "end"}},
      {Type::weight, {"thin", "light", "regular", "medium", "semibold", "bold", "black"}},
      {Type::fit, {"stretch", "contain", "cover", "aspect"}},
      {Type::text_align, {"start", "center", "end"}},
      {Type::gradient, {"none", "horizontal", "vertical"}},
      {Type::color_depth, {"bits8", "bits10", "bits16"}},
  };
  return table.at(type);
}

std::string joined(const std::vector<std::string_view>& names) {
  std::string result;
  for (const auto name : names) result += (result.empty() ? "" : ", ") + std::string(name);
  return result;
}

// The space an element's children lay out in.
struct Space {
  std::optional<float> width;
  std::optional<float> height;
};

// A flex child waiting for the row or column to know its leftover space.
struct FlexSlot {
  const Node* node;
  ScopePtr scope;
  std::size_t slot;
  float weight;
};

bool event_name(std::string_view name) {
  return name.size() > 2 && name.starts_with("on") &&
         (name[2] == '_' || std::isupper(static_cast<unsigned char>(name[2])) != 0);
}

const Argument* argument(const Node& node, std::string_view name) {
  for (const auto& item : node.arguments)
    if (item.name == name) return &item;
  return nullptr;
}

}  // namespace

// --- the plugin -----------------------------------------------------------------------

struct Plugin::State {
  std::shared_ptr<const Program> program;
  Value::Object globals;
  std::map<std::string, Component, std::less<>> components;
  Report report = std::make_shared<std::function<void(const std::exception&)>>(
      [](const std::exception& error) { std::cerr << "facet: " << error.what() << '\n'; });
};

namespace {

// Runs a handler from vw's event loop: a plugin's mistake is reported, not
// thrown into the loop.
template <class Work>
void guarded(const Report& report, Work&& work) {
  try {
    work();
  } catch (const std::exception& error) {
    if (report && *report) (*report)(error);
  }
}

class Builder {
 public:
  Builder(const Plugin::State& state, std::shared_ptr<const ViewContext> context)
      : state_(state), program_(*state.program) {
    evaluator_.program = state.program;
    evaluator_.context = std::move(context);
  }

  vw::Widget run(vw::Ui& ui, const View& view, Arguments arguments) {
    auto globals = std::make_shared<Scope>();
    globals->names = state_.globals;
    // File-level constants see the host's names and each other, in order.
    for (const auto& constant : program_.file.constants)
      globals->names[constant.name] = evaluator_.evaluate(*constant.value, globals);
    const ScopePtr scope = child_of(globals);
    for (std::size_t index = 0; index < view.parameters.size(); ++index)
      scope->names[view.parameters[index]] = index < arguments.size() ? arguments[index] : Value();
    spaces_ = {Space{}};
    const bool surface = view.body.size() == 1 && view.body.front()->kind == Node::Kind::element &&
                         resolve(*view.body.front()) != nullptr &&
                         resolve(*view.body.front())->kind == Kind::surface;
    if (surface) {
      top_level_ = view.body.front().get();
      (void)element(ui, *view.body.front(), scope);
      return vw::Widget{};
    }
    return group(ui, block(ui, view.body, scope));
  }

 private:
  // --- statements ------------------------------------------------------------
  std::vector<vw::Widget> block(vw::Ui& ui, const Block& nodes, const ScopePtr& parent,
                                std::vector<FlexSlot>* flex = nullptr) {
    // Names given inside the block stay in it.
    const ScopePtr scope = child_of(parent);
    std::vector<vw::Widget> list;
    for (const auto& node : nodes) statement(ui, *node, scope, list, flex);
    return list;
  }

  void statement(vw::Ui& ui, const Node& node, const ScopePtr& scope, std::vector<vw::Widget>& list,
                 std::vector<FlexSlot>* flex) {
    switch (node.kind) {
      case Node::Kind::let:
        scope->names[node.name] = evaluator_.evaluate(*node.value, scope);
        return;
      case Node::Kind::for_: {
        const Value items = evaluator_.evaluate(*node.value, scope);
        if (!items.is_list()) evaluator_.error(node.value->at, std::format("for needs a list, got {}", items.kind()));
        std::size_t counter = 0;
        for (const Value& item : items.list()) {
          const ScopePtr inner = child_of(scope);
          inner->names[node.name] = item;
          evaluator_.keys.push_back(node.key ? evaluator_.evaluate(*node.key, inner).to_string()
                                             : std::to_string(counter));
          ++counter;
          for (const auto& child : node.children) statement(ui, *child, inner, list, nullptr);
          evaluator_.keys.pop_back();
        }
        return;
      }
      case Node::Kind::if_: {
        const bool taken = evaluator_.evaluate(*node.value, scope).truthy();
        const ScopePtr inner = child_of(scope);
        for (const auto& child : taken ? node.children : node.otherwise) statement(ui, *child, inner, list, nullptr);
        return;
      }
      case Node::Kind::render:
        evaluator_.error(node.at, "a (index) => { … } row belongs inside a ListView");
      case Node::Kind::cpp:
        evaluator_.error(node.at, "a cpp { } block: plugins cannot run C++");
      case Node::Kind::element: {
        const Argument* weight = argument(node, "flex");
        const ElementSpec* spec = resolve(node);
        if (weight != nullptr && flex == nullptr && spec == nullptr) weight = nullptr;
        if (weight != nullptr) {
          if (flex == nullptr) evaluator_.error(weight->at, "flex works on elements written directly inside a Row or Column");
          const double share = evaluator_.at(weight->at, [&] { return evaluator_.evaluate(*weight->value, scope).number(); });
          flex->push_back({&node, scope, list.size(), static_cast<float>(share)});
          list.emplace_back();
          return;
        }
        if (const auto widget = named(ui, node, scope, {})) list.push_back(*widget);
        return;
      }
    }
  }

  // An element, and if it has `id: name`, its measured box for the elements
  // after it. Empty for a surface, which ends the view.
  std::optional<vw::Widget> named(vw::Ui& ui, const Node& node, const ScopePtr& scope,
                                  std::optional<std::pair<std::string, float>> main) {
    const std::string* id = element_id(node, scope);
    if (id != nullptr && scope->names.contains(*id))
      evaluator_.error(argument(node, "id")->at, "id '" + *id + "' is already used here");
    const auto widget = element(ui, node, scope, std::move(main));
    if (id != nullptr && widget) {
      Value& box = scope->names[*id];
      float x = 0;
      float y = 0;
      bool found = false;
      const auto& rectangles = ui.scene().rectangles;
      for (std::size_t index = widget->begin; index < widget->end && index < rectangles.size(); ++index) {
        x = found ? std::min(x, rectangles[index].x) : rectangles[index].x;
        y = found ? std::min(y, rectangles[index].y) : rectangles[index].y;
        found = true;
      }
      box = box_value(x, y, widget->width, widget->height, true, box["props"]);
      remember(*id, x, y, widget->width, widget->height);
    }
    return widget;
  }

  // id: name with a bare name not in use names the element; anything else is
  // a control's state key.
  const std::string* element_id(const Node& node, const ScopePtr& scope) const {
    const Argument* id = argument(node, "id");
    if (id == nullptr || id->value->kind != Expr::Kind::identifier) return nullptr;
    const Value* existing = scope->find(id->value->text);
    if (existing != nullptr && !names_.contains(id->value->text)) return nullptr;
    return &id->value->text;
  }

  void remember(const std::string& name, float x, float y, float width, float height) {
    const auto& ids = evaluator_.context->ids;
    if (const auto found = ids.find(name); found == ids.end() || found->second != Seen::once) return;
    const Box next{x, y, width, height, true};
    Box& kept = remembered_boxes()[std::format("{}:{}.{}", program_.name, evaluator_.context->view, name)];
    if (kept == next) return;
    kept = next;
    vw::request_animation_continuation();
  }

  // Binds `id: name` to the element's declared box while its children build.
  void declare(const Node& node, const ScopePtr& scope, float x, float y, float width, float height,
               Value props) {
    const std::string* id = element_id(node, scope);
    if (id == nullptr) return;
    scope->names[*id] = box_value(x, y, width, height, false, std::move(props));
    names_.insert(*id);
  }

  // --- elements ----------------------------------------------------------------
  const ElementSpec* resolve(const Node& node) const {
    if (node.module.empty() && node.name == "Native") return nullptr;
    if (node.module.empty())
      evaluator_.error(node.at, "'" + node.name + "' needs its module, e.g. ui." + node.name);
    const auto module = program_.modules.find(node.module);
    if (module == program_.modules.end())
      evaluator_.error(node.at, "'" + node.module + "' is not imported; add it to let [ … ] in");
    if (module->second != "vw.ui" && module->second != "vw.layout") return nullptr;
    for (const auto& spec : elements())
      if (spec.module == module->second && spec.name == node.name) return &spec;
    if (node.name == "Native") return nullptr;
    evaluator_.error(node.at, module->second + " has no element " + node.name);
  }

  std::string key_here(Location at) const { return event_key(program_, at, evaluator_.keys); }

  // A property's value, checked against its type. Enums stay as their names.
  Value convert(Type type, const Expr& expr, const ScopePtr& scope, std::string_view name) {
    if (expr.kind == Expr::Kind::conditional && type != Type::handler)
      return convert(type, *expr.operands[evaluator_.evaluate(*expr.operands[0], scope).truthy() ? 1 : 2], scope,
                     name);
    if (expr.kind == Expr::Kind::string && expr.text.size() > 1 && expr.text.back() == '%') {
      if (type != Type::number) evaluator_.error(expr.at, "only sizes can be relative");
      return Value(percent(expr, name));
    }
    const auto evaluated = [&] { return evaluator_.evaluate(expr, scope); };
    switch (type) {
      case Type::number: case Type::integer: case Type::size: case Type::padding:
        return evaluator_.at(expr.at, [&] { return Value(evaluated().number()); });
      case Type::boolean: return Value(evaluated().truthy());
      case Type::string: return Value(evaluated().to_string());
      case Type::strings: {
        const Value items = evaluated();
        if (!items.is_list()) evaluator_.error(expr.at, std::format("expected a list, got {}", items.kind()));
        Value::List texts;
        for (const Value& item : items.list()) texts.emplace_back(item.to_string());
        return Value(std::move(texts));
      }
      case Type::color: {
        const Value value = evaluated();
        if (value.is_color()) return value;
        if (value.is_text())
          if (const auto parsed = parse_color(value.text())) return Value(*parsed);
        evaluator_.error(expr.at, "colors are \"#rrggbb\", \"#rrggbbaa\", \"transparent\" or a color value");
      }
      case Type::handler: return Value(evaluator_.handler(expr, scope));
      case Type::input: return input(expr, scope);
      default: {
        if (expr.kind != Expr::Kind::identifier && expr.kind != Expr::Kind::string)
          evaluator_.error(expr.at, "expected one of: " + joined(choices(type)));
        const auto& names = choices(type);
        if (std::ranges::find(names, expr.text) == names.end())
          evaluator_.error(expr.at, std::format("'{}' is not one of: {}", expr.text, joined(names)));
        return Value(expr.text);
      }
    }
  }

  float percent(const Expr& expr, std::string_view name) {
    const std::string number = expr.text.substr(0, expr.text.size() - 1);
    double share = 0;
    const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), share);
    if (error != std::errc{} || end != number.data() + number.size())
      evaluator_.error(expr.at, "a relative size is a number and %: \"100%\"");
    const bool horizontal = name == "width" || name == "max_width";
    const bool vertical = name == "height" || name == "item_height";
    if (!horizontal && !vertical) evaluator_.error(expr.at, "only widths and heights can be relative");
    const auto& span = horizontal ? spaces_.back().width : spaces_.back().height;
    if (!span)
      evaluator_.error(expr.at, std::format("\"{}\" of what? Give a parent a {}", expr.text,
                                            horizontal ? "width" : "height"));
    return static_cast<float>(*span * share / 100.0);
  }

  Value input(const Expr& expr, const ScopePtr& scope) {
    if (expr.kind != Expr::Kind::object) evaluator_.error(expr.at, "input is an object: { on_click: …, hover: { … } }");
    Value::Object fields;
    for (const auto& [field_name, field] : expr.fields) {
      if (field_name == "on_click" || field_name == "on_right_click" || field_name == "on_middle_click" ||
          field_name == "on_hover" || field_name == "on_scroll" ||
          field_name == "on_scroll_x" || field_name == "on_touch" || field_name == "on_drag" ||
          field_name == "on_drag_start" || field_name == "on_drag_end") {
        fields[field_name] = Value(evaluator_.handler(*field, scope));
      } else if (field_name == "hover") {
        if (field->kind != Expr::Kind::object) evaluator_.error(field->at, "hover is an object: { color: …, scale: … }");
        Value::Object hover;
        for (const auto& [hover_name, hover_value] : field->fields) {
          if (hover_name == "color") hover["color"] = convert(Type::color, *hover_value, scope, hover_name);
          else if (hover_name == "scale") hover["scale"] = convert(Type::number, *hover_value, scope, hover_name);
          else evaluator_.error(hover_value->at, "hover has color and scale, not '" + hover_name + "'");
        }
        fields["hover"] = Value(std::move(hover));
      } else {
        evaluator_.error(field->at, "input has no '" + field_name + "'");
      }
    }
    return Value(std::move(fields));
  }

  // Event adapters: vw's handlers calling a plugin's functions.
  template <class Event, class Convert>
  std::function<void(const Event&)> adapt(const Value& function, Convert convert) const {
    const Function call = function.function();
    const Report report = state_.report;
    return [call, report, convert](const Event& event) {
      guarded(report, [&] {
        const Value argument = convert(event);
        call(Arguments(&argument, 1));
      });
    };
  }

  vw::Input make_input(const Value& value) const {
    vw::Input result;
    const auto click = [](const vw::ClickEvent& e) {
      return Value(Value::Object{{"x", e.x}, {"y", e.y}, {"button", e.button}, {"left", e.left()},
                                 {"right", e.right()}, {"middle", e.middle()}});
    };
    const auto scroll = [](const vw::ScrollEvent& e) {
      return Value(Value::Object{{"x", e.x}, {"y", e.y}, {"delta", e.delta}});
    };
    const auto drag = [](const vw::DragEvent& e) {
      return Value(Value::Object{{"x", e.x}, {"y", e.y}, {"start_x", e.start_x}, {"start_y", e.start_y},
                                 {"dx", e.dx}, {"dy", e.dy}, {"delta_x", e.delta_x}, {"delta_y", e.delta_y},
                                 {"button", e.button}, {"cancelled", e.cancelled}});
    };
    const auto touch = [](const vw::TouchEvent& e) {
      static constexpr std::array<std::string_view, 4> phases{"down", "motion", "up", "cancel"};
      return Value(Value::Object{{"phase", phases[static_cast<std::size_t>(e.phase)]}, {"id", e.id},
                                 {"x", e.x}, {"y", e.y}, {"start_x", e.start_x}, {"start_y", e.start_y},
                                 {"fingers", e.fingers}});
    };
    if (const Value& f = value["on_click"]; f.is_function()) result.on_click = adapt<vw::ClickEvent>(f, click);
    if (const Value& f = value["on_right_click"]; f.is_function()) result.on_right_click = adapt<vw::ClickEvent>(f, click);
    if (const Value& f = value["on_middle_click"]; f.is_function()) result.on_middle_click = adapt<vw::ClickEvent>(f, click);
    if (const Value& f = value["on_scroll"]; f.is_function()) result.on_scroll = adapt<vw::ScrollEvent>(f, scroll);
    if (const Value& f = value["on_scroll_x"]; f.is_function()) result.on_scroll_x = adapt<vw::ScrollEvent>(f, scroll);
    if (const Value& f = value["on_touch"]; f.is_function()) result.on_touch = adapt<vw::TouchEvent>(f, touch);
    if (const Value& f = value["on_drag"]; f.is_function()) result.on_drag = adapt<vw::DragEvent>(f, drag);
    if (const Value& f = value["on_drag_start"]; f.is_function()) result.on_drag_start = adapt<vw::DragEvent>(f, drag);
    if (const Value& f = value["on_drag_end"]; f.is_function()) result.on_drag_end = adapt<vw::DragEvent>(f, drag);
    if (const Value& f = value["on_hover"]; f.is_function()) {
      const Function call = f.function();
      const Report report = state_.report;
      result.on_hover = [call, report](bool inside) {
        guarded(report, [&] {
          const Value argument(inside);
          call(Arguments(&argument, 1));
        });
      };
    }
    if (const Value& hover = value["hover"]; hover.is_object()) {
      result.hover = vw::Hover{};
      if (hover["color"].is_color()) result.hover->color = hover["color"].color();
      if (hover["scale"].is_number()) result.hover->scale = static_cast<float>(hover["scale"].number());
    }
    return result;
  }

  template <class Argument>
  std::function<void(Argument)> value_handler(const Value& function) const {
    const Function call = function.function();
    const Report report = state_.report;
    return [call, report](Argument argument) {
      guarded(report, [&] {
        const Value value(argument);
        call(Arguments(&value, 1));
      });
    };
  }

  static vw::Margin margin_of(const Value& value) { return facet::margin(value.number()); }

  // Sets one property on the props struct of an element.
  void apply(Kind kind, void* props, std::string_view name, const Value& value, Location at) {
    const auto f = [&] { return static_cast<float>(value.number()); };
    const auto anchor_of = [&] { return anchors().at(value.text()); };
    const auto align_of = [&] {
      const auto& text = value.text();
      return text == "start" ? vw::Align::start : text == "end" ? vw::Align::end : vw::Align::center;
    };
    switch (kind) {
      case Kind::surface: {
        auto& p = *static_cast<vw::SurfaceProps*>(props);
        if (name == "width") p.width = f();
        else if (name == "height") p.height = f();
        else if (name == "anchor") p.anchor = anchor_of();
        else if (name == "layer") {
          const auto& text = value.text();
          p.layer = text == "background" ? vw::Layer::background : text == "bottom" ? vw::Layer::bottom
                  : text == "overlay" ? vw::Layer::overlay : vw::Layer::top;
        } else if (name == "exclusive_zone") p.exclusive_zone = static_cast<std::int32_t>(value.number());
        else if (name == "namespace") p.surface_namespace = value.text();
        else if (name == "refresh") p.refresh_interval = value.number();
        else if (name == "output") p.output = value.text();
        else if (name == "color_depth") {
          const auto& text = value.text();
          p.color_depth = text == "bits16" ? vw::ColorDepth::bits16
                        : text == "bits10" ? vw::ColorDepth::bits10 : vw::ColorDepth::bits8;
        }
        return;
      }
      case Kind::rectangle: {
        auto& p = *static_cast<vw::RectangleProps*>(props);
        if (name == "width") p.width = f();
        else if (name == "height") p.height = f();
        else if (name == "radius") p.radius = f();
        else if (name == "color" || name == "bg") p.color = value.color();
        else if (name == "color2") p.color2 = value.color();
        else if (name == "gradient") {
          const auto& text = value.text();
          p.gradient = text == "horizontal" ? vw::Gradient::horizontal
                     : text == "vertical" ? vw::Gradient::vertical : vw::Gradient::none;
        } else if (name == "border_width") p.border_width = f();
        else if (name == "border_color") p.border_color = value.color();
        else if (name == "input") p.input = make_input(value);
        return;
      }
      case Kind::text: {
        auto& p = *static_cast<vw::TextProps*>(props);
        if (name == "text") p.text = value.text();
        else if (name == "font") p.font = value.text();
        else if (name == "size") p.size = f();
        else if (name == "weight") {
          static const std::map<std::string, int, std::less<>> weights{
              {"thin", vw::weight::thin},     {"light", vw::weight::light},       {"regular", vw::weight::regular},
              {"medium", vw::weight::medium}, {"semibold", vw::weight::semibold}, {"bold", vw::weight::bold},
              {"black", vw::weight::black}};
          p.weight = weights.at(value.text());
        } else if (name == "color") p.color = value.color();
        else if (name == "max_width") p.max_width = f();
        else if (name == "max_lines") p.max_lines = static_cast<int>(value.number());
        else if (name == "align") p.align = value.text();
        else if (name == "pixel_snap") p.pixel_snap = value.boolean();
        else if (name == "padding") p.place.margin = margin_of(value);
        else if (name == "input") p.input = make_input(value);
        return;
      }
      case Kind::image: {
        auto& p = *static_cast<vw::ImageProps*>(props);
        if (name == "path") p.path = value.text();
        else if (name == "icon") p.icon = value.text();
        else if (name == "width") p.width = f();
        else if (name == "height") p.height = f();
        else if (name == "fit") {
          const auto& text = value.text();
          p.fit = text == "contain" ? vw::ImageFit::contain : text == "cover" ? vw::ImageFit::cover
                                                                              : text == "aspect" ? vw::ImageFit::aspect : vw::ImageFit::stretch;
        } else if (name == "radius") p.radius = f();
        else if (name == "padding") p.place.margin = margin_of(value);
        else if (name == "input") p.input = make_input(value);
        return;
      }
      case Kind::text_field: {
        auto& p = *static_cast<vw::TextFieldProps*>(props);
        if (name == "id") p.id = value.text();
        else if (name == "value") p.value = value.text();
        else if (name == "placeholder") p.placeholder = value.text();
        else if (name == "width") p.width = f();
        else if (name == "height") p.height = f();
        else if (name == "radius") p.radius = f();
        else if (name == "color") p.color = value.color();
        else if (name == "border_color") p.border_color = value.color();
        else if (name == "focus_color") p.focus_color = value.color();
        else if (name == "text_color") p.text_color = value.color();
        else if (name == "placeholder_color") p.placeholder_color = value.color();
        else if (name == "font") p.font = value.text();
        else if (name == "size") p.size = f();
        else if (name == "password") p.password = value.boolean();
        else if (name == "autofocus") p.autofocus = value.boolean();
        else if (name == "on_change") p.on_change = value_handler<const std::string&>(value);
        else if (name == "on_submit") p.on_submit = value_handler<const std::string&>(value);
        return;
      }
      case Kind::toggle: {
        auto& p = *static_cast<ToggleProps*>(props);
        if (name == "id") p.id = value.text();
        else if (name == "value") p.value = value.boolean();
        else if (name == "width") p.width = f();
        else if (name == "height") p.height = f();
        else if (name == "color") p.color = value.color();
        else if (name == "off_color") p.off_color = value.color();
        else if (name == "thumb_color") p.thumb_color = value.color();
        else if (name == "on_change") p.on_change = value_handler<bool>(value);
        return;
      }
      case Kind::slider: {
        auto& p = *static_cast<SliderProps*>(props);
        if (name == "id") p.id = value.text();
        else if (name == "value") p.value = f();
        else if (name == "min") p.min = f();
        else if (name == "max") p.max = f();
        else if (name == "step") p.step = f();
        else if (name == "width") p.width = f();
        else if (name == "height") p.height = f();
        else if (name == "color") p.color = value.color();
        else if (name == "track_color") p.track_color = value.color();
        else if (name == "thumb_color") p.thumb_color = value.color();
        else if (name == "on_change") p.on_change = value_handler<float>(value);
        return;
      }
      case Kind::dropdown: {
        auto& p = *static_cast<DropdownProps*>(props);
        if (name == "id") p.id = value.text();
        else if (name == "options") {
          p.options.clear();
          for (const Value& item : value.list()) p.options.push_back(item.text());
        } else if (name == "current" || name == "value") p.current = value.text();
        else if (name == "width") p.width = f();
        else if (name == "height") p.height = f();
        else if (name == "color") p.color = value.color();
        else if (name == "hover_color") p.hover_color = value.color();
        else if (name == "text_color") p.text_color = value.color();
        else if (name == "on_change") p.on_change = value_handler<const std::string&>(value);
        return;
      }
      case Kind::spinner: {
        auto& p = *static_cast<SpinnerProps*>(props);
        if (name == "id") p.id = value.text();
        else if (name == "size") p.size = f();
        else if (name == "color") p.color = value.color();
        return;
      }
      case Kind::row: case Kind::column: {
        auto& p = *static_cast<vw::LayoutProps*>(props);
        if (name == "gap") p.gap = f();
        else if (name == "padding") p.padding = f();
        else if (name == "width") p.width = f();
        else if (name == "height") p.height = f();
        else if (name == "justify") {
          const auto& text = value.text();
          p.justify = text == "center" ? vw::Justify::center : text == "end" ? vw::Justify::end
                    : text == "space-between" ? vw::Justify::space_between
                    : text == "space-around" ? vw::Justify::space_around
                    : text == "space-evenly" ? vw::Justify::space_evenly : vw::Justify::start;
        } else if (name == "align") p.align = align_of();
        else if (name == "radius") p.radius = f();
        return;
      }
      case Kind::grid: {
        auto& p = *static_cast<vw::GridProps*>(props);
        if (name == "columns") p.columns = static_cast<int>(value.number());
        else if (name == "gap") p.gap = f();
        else if (name == "row_gap") p.row_gap = f();
        else if (name == "padding") p.padding = f();
        else if (name == "width") p.width = f();
        else if (name == "height") p.height = f();
        else if (name == "align") p.align = align_of();
        else if (name == "radius") p.radius = f();
        return;
      }
      case Kind::listview: {
        auto& p = *static_cast<vw::ListviewProps*>(props);
        if (name == "id") p.id = value.text();
        else if (name == "width") p.width = f();
        else if (name == "height") p.height = f();
        else if (name == "item_height") p.item_height = f();
        else if (name == "gap") p.gap = f();
        else if (name == "radius") p.radius = f();
        else if (name == "scroll_step") p.scroll_step = f();
        else if (name == "count") p.count = static_cast<std::size_t>(std::max(0.0, value.number()));
        return;
      }
    }
    evaluator_.error(at, "cannot set " + std::string(name));
  }

  static std::string hint(const std::string& name) {
    if (name == "padding") return " (padding belongs to a layout around it)";
    if (name == "bg") return " (backgrounds go on Rectangle, Row, Column or Grid)";
    return "";
  }

  // Sets every argument of `node` on `props`. Returns the converted values,
  // which `id: name` exposes as `name.props`.
  Value configure(const ElementSpec& spec, const Node& node, const ScopePtr& scope, void* props,
                  const std::optional<std::pair<std::string, float>>& main) {
    const bool layout = spec.kind == Kind::row || spec.kind == Kind::column || spec.kind == Kind::grid;
    // animate: { property: "width", duration: 200 } tweens that property.
    std::string animated;
    double duration = 280;
    if (const Argument* tween_argument = argument(node, "animate")) {
      if (tween_argument->value->kind != Expr::Kind::object)
        evaluator_.error(tween_argument->at, "animate is { property: \"width\", duration: 200 }, or animate(value, 200) on the value");
      for (const auto& [field_name, field] : tween_argument->value->fields) {
        if (field_name == "property" && field->kind == Expr::Kind::string) animated = field->text;
        else if (field_name == "duration")
          duration = evaluator_.at(field->at, [&] { return evaluator_.evaluate(*field, scope).number(); });
        else evaluator_.error(field->at, "animate has property and duration");
      }
      if (animated.empty()) evaluator_.error(tween_argument->at, "animate needs property: the name of a size or number");
    }
    Value::Object values;
    const std::string* id = element_id(node, scope);
    for (std::size_t index = 0; index < node.arguments.size(); ++index) {
      const Argument& item = node.arguments[index];
      std::string name = item.name;
      if (layout && (name == "bg" || name == "color" || name == "border_width" || name == "border_color")) continue;
      if (name == "flex" || name == "animate") continue;
      if (name == "id" && id != nullptr) continue;
      if (name.empty()) {
        if (index != 0 || spec.positional.empty())
          evaluator_.error(item.at, std::string(spec.name) + " takes only named arguments here");
        name = std::string(spec.positional);
      }
      const auto property = std::ranges::find(spec.properties, name, &Property::name);
      if (property == spec.properties.end())
        evaluator_.error(item.at, std::format("{} has no property '{}'{}", spec.name, name, hint(name)));
      if (property->type == Type::text_align && item.value->kind == Expr::Kind::identifier &&
          placements().contains(item.value->text)) {
        static_cast<vw::TextProps*>(props)->place.anchor = placements().at(item.value->text);
        continue;
      }
      Value value = convert(property->type, *item.value, scope, name);
      if (name == animated) {
        if (!value.is_number()) evaluator_.error(item.at, "only numbers and sizes animate");
        value = Value(tween(key_here(item.at), value.number(), duration));
        animated.clear();
      }
      evaluator_.at(item.at, [&] { apply(spec.kind, props, name, value, item.at); });
      values[name] = std::move(value);
    }
    if (!animated.empty())
      evaluator_.error(argument(node, "animate")->at, std::format("animate names '{}', which is not set here", animated));
    if (main) {
      if (std::ranges::find(spec.properties, main->first, &Property::name) == spec.properties.end())
        evaluator_.error(node.at, std::format("flex sets {}, which {} does not have", main->first, spec.name));
      apply(spec.kind, props, main->first, Value(main->second), node.at);
      values[main->first] = Value(main->second);
    }
    return Value(std::move(values));
  }

  // A layout's background: drawn first, so it lands behind the children.
  std::optional<vw::Widget> background(vw::Ui& ui, const Node& node, const ScopePtr& scope) {
    if (argument(node, "bg") == nullptr && argument(node, "color") == nullptr) return std::nullopt;
    vw::RectangleProps back;
    back.width = back.height = 1;
    for (const auto& item : node.arguments) {
      if (item.name == "bg" || item.name == "color") back.color = convert(Type::color, *item.value, scope, item.name).color();
      else if (item.name == "border_width")
        back.border_width = static_cast<float>(convert(Type::number, *item.value, scope, item.name).number());
      else if (item.name == "border_color")
        back.border_color = convert(Type::color, *item.value, scope, item.name).color();
      else if (item.name == "radius")
        back.radius = static_cast<float>(convert(Type::number, *item.value, scope, item.name).number());
    }
    return ui.rectangle(back);
  }

  std::optional<vw::Widget> element(vw::Ui& ui, const Node& node, const ScopePtr& scope,
                                    std::optional<std::pair<std::string, float>> main = {}) {
    const ElementSpec* spec = resolve(node);
    if (spec == nullptr) return component(ui, node, scope, main);
    if (spec->kind == Kind::surface && &node != top_level_)
      evaluator_.error(node.at, "a Surface must be the whole view");
    switch (spec->kind) {
      case Kind::surface: {
        vw::SurfaceProps props;
        Value values = configure(*spec, node, scope, &props, main);
        if (node.children.empty()) evaluator_.error(node.at, "a Surface needs content");
        facet::use_output(props.output);
        {
          const Named<vw::SurfaceProps> declared(props);
          declare(node, scope, declared.x, declared.y, declared.width, declared.height, values);
        }
        spaces_.push_back({props.width, props.height});
        const auto children = block(ui, node.children, scope);
        spaces_.pop_back();
        (void)group(ui, children);
        ui.surface(props);
        return std::nullopt;
      }
      case Kind::rectangle: {
        vw::RectangleProps props;
        Value values = configure(*spec, node, scope, &props, main);
        declare(node, scope, 0, 0, props.width, props.height, values);
        vw::Widget widget = ui.rectangle(props);
        if (!node.children.empty()) {
          spaces_.push_back({props.width, props.height});
          const auto children = block(ui, node.children, scope);
          spaces_.pop_back();
          widget = on(ui, widget, children, props.radius);
        }
        return widget;
      }
      case Kind::text: return simple<vw::TextProps>(ui, *spec, node, scope, main, [&](const vw::TextProps& p) { return ui.text(p); });
      case Kind::image: return simple<vw::ImageProps>(ui, *spec, node, scope, main, [&](const vw::ImageProps& p) { return image(ui, p); });
      case Kind::text_field:
        return simple<vw::TextFieldProps>(ui, *spec, node, scope, main, [&](const vw::TextFieldProps& p) { return ui.text_field(p); });
      case Kind::toggle: return control<ToggleProps>(ui, *spec, node, scope, main, [&](const ToggleProps& p) { return toggle(ui, p); });
      case Kind::slider: return control<SliderProps>(ui, *spec, node, scope, main, [&](const SliderProps& p) { return slider(ui, p); });
      case Kind::dropdown: return control<DropdownProps>(ui, *spec, node, scope, main, [&](const DropdownProps& p) { return dropdown(ui, p); });
      case Kind::spinner: return control<SpinnerProps>(ui, *spec, node, scope, main, [&](const SpinnerProps& p) { return spinner(ui, p); });
      case Kind::listview: return listview(ui, *spec, node, scope, main);
      case Kind::row: case Kind::column: return linear(ui, *spec, node, scope, main);
      case Kind::grid: return grid(ui, *spec, node, scope, main);
    }
    return std::nullopt;
  }

  template <class Props, class Build>
  vw::Widget simple(vw::Ui&, const ElementSpec& spec, const Node& node, const ScopePtr& scope,
                    const std::optional<std::pair<std::string, float>>& main, Build build) {
    if (!node.children.empty()) evaluator_.error(node.at, std::string(spec.name) + " takes no children");
    Props props;
    Value values = configure(spec, node, scope, &props, main);
    const Named<Props> declared(props);
    declare(node, scope, declared.x, declared.y, declared.width, declared.height, values);
    return build(props);
  }

  template <class Props, class Build>
  vw::Widget control(vw::Ui&, const ElementSpec& spec, const Node& node, const ScopePtr& scope,
                     const std::optional<std::pair<std::string, float>>& main, Build build) {
    if (!node.children.empty()) evaluator_.error(node.at, std::string(spec.name) + " takes no children");
    Props props;
    Value values = configure(spec, node, scope, &props, main);
    // Without an id of its own (or with id: name), the place keys its state.
    if (argument(node, "id") == nullptr || element_id(node, scope) != nullptr) props.id = key_here(node.at);
    const Named<Props> declared(props);
    declare(node, scope, declared.x, declared.y, declared.width, declared.height, values);
    return build(props);
  }

  vw::Widget linear(vw::Ui& ui, const ElementSpec& spec, const Node& node, const ScopePtr& scope,
                    const std::optional<std::pair<std::string, float>>& main) {
    const bool row = spec.kind == Kind::row;
    const auto back = background(ui, node, scope);
    vw::LayoutProps props;
    Value values = configure(spec, node, scope, &props, main);
    declare(node, scope, 0, 0, props.width.value_or(0), props.height.value_or(0), values);
    const Space parent = spaces_.back();
    const auto inner = [&](const std::optional<float>& own, const std::optional<float>& around) -> std::optional<float> {
      if (own) return *own - 2 * props.padding;
      if (around) return *around - 2 * props.padding;
      return std::nullopt;
    };
    spaces_.push_back({inner(props.width, parent.width), inner(props.height, parent.height)});
    std::vector<FlexSlot> flex;
    std::vector<vw::Widget> children = block(ui, node.children, scope, &flex);
    if (!flex.empty()) {
      // A row or column with flex children spans its parent's axis.
      auto& axis = row ? props.width : props.height;
      if (!axis) {
        const auto& span = row ? parent.width : parent.height;
        if (!span)
          evaluator_.error(node.at, std::format("flex needs the {}'s {}: set {} here or on a parent", spec.name,
                                                row ? "width" : "height", row ? "width" : "height"));
        axis = *span;
        spaces_.back() = {inner(props.width, parent.width), inner(props.height, parent.height)};
      }
      float total = 0;
      for (const auto& slot : flex) total += slot.weight;
      std::vector<std::size_t> slots;
      for (const auto& slot : flex) slots.push_back(slot.slot);
      const float spare = flex_space(children, slots, *axis, props.padding, props.gap, row);
      for (const auto& slot : flex) {
        const float share = total > 0 ? spare * slot.weight / total : 0;
        const auto built = named(ui, *slot.node, slot.scope, std::pair{std::string(row ? "width" : "height"), share});
        children[slot.slot] = built.value_or(nothing(ui));
      }
    }
    spaces_.pop_back();
    vw::Widget widget = row ? facet::row(ui, children, props) : column(ui, children, props);
    if (back) widget = behind(ui, *back, widget);
    return widget;
  }

  vw::Widget grid(vw::Ui& ui, const ElementSpec& spec, const Node& node, const ScopePtr& scope,
                  const std::optional<std::pair<std::string, float>>& main) {
    const auto back = background(ui, node, scope);
    vw::GridProps props;
    Value values = configure(spec, node, scope, &props, main);
    declare(node, scope, 0, 0, props.width.value_or(0), props.height.value_or(0), values);
    const Space parent = spaces_.back();
    const auto inner = [&](const std::optional<float>& own, const std::optional<float>& around) -> std::optional<float> {
      if (own) return *own - 2 * props.padding;
      if (around) return *around - 2 * props.padding;
      return std::nullopt;
    };
    Space inside{inner(props.width, parent.width), inner(props.height, parent.height)};
    if (inside.width && props.columns > 0)
      inside.width = (*inside.width - props.gap * static_cast<float>(props.columns - 1)) / static_cast<float>(props.columns);
    spaces_.push_back(inside);
    const auto children = block(ui, node.children, scope);
    spaces_.pop_back();
    vw::Widget widget = facet::grid(ui, children, props);
    if (back) widget = behind(ui, *back, widget);
    return widget;
  }

  vw::Widget listview(vw::Ui& ui, const ElementSpec& spec, const Node& node, const ScopePtr& scope,
                      const std::optional<std::pair<std::string, float>>& main) {
    if (node.children.size() != 1 || node.children.front()->kind != Node::Kind::render)
      evaluator_.error(node.at, "a ListView holds one (index) => { … } row");
    if (argument(node, "count") == nullptr) evaluator_.error(node.at, "a ListView needs count: how many rows");
    vw::ListviewProps props;
    if (argument(node, "width") == nullptr && spaces_.back().width) props.width = *spaces_.back().width;
    Value values = configure(spec, node, scope, &props, main);
    if (argument(node, "id") == nullptr || element_id(node, scope) != nullptr) props.id = key_here(node.at);
    declare(node, scope, 0, 0, props.width, props.height, values);
    const Node& render = *node.children.front();
    if (render.parameters.size() != 1) evaluator_.error(render.at, "a row takes one parameter: its index");
    const auto rows = [&](vw::Ui& row_ui, std::size_t index) -> vw::Widget {
      const ScopePtr inner = child_of(scope);
      inner->names[render.parameters.front()] = Value(index);
      evaluator_.keys.push_back(std::to_string(index));
      spaces_.push_back({props.width, props.item_height});
      const auto children = block(row_ui, render.children, inner);
      spaces_.pop_back();
      evaluator_.keys.pop_back();
      return group(row_ui, children);
    };
    return facet::listview(ui, props, rows);
  }

  // S.Name(…) { … } and Native(name: Name, …): a component the host registered.
  vw::Widget component(vw::Ui& ui, const Node& node, const ScopePtr& scope,
                       const std::optional<std::pair<std::string, float>>& main) {
    std::string name;
    Value::Object props;
    const std::string* id = element_id(node, scope);
    const bool native = node.name == "Native" &&
                        (node.module.empty() || program_.modules.at(node.module).starts_with("vw."));
    if (native) {
      for (const auto& item : node.arguments) {
        if (item.name == "name" || item.name == "class") {
          if (item.value->kind != Expr::Kind::identifier && item.value->kind != Expr::Kind::string)
            evaluator_.error(item.at, "Native's name is the component to draw");
          name = item.value->text;
        } else if (item.name == "props") {
          if (item.value->kind != Expr::Kind::object) evaluator_.error(item.at, "props is an object: { … }");
          for (const auto& [field_name, field] : item.value->fields) props[field_name] = component_value(*field, scope, field_name);
        } else if (item.name == "id" && id != nullptr) {
          continue;
        } else if (item.name.empty()) {
          evaluator_.error(item.at, "Native takes only named arguments");
        } else {
          props[item.name] = component_value(*item.value, scope, item.name);
        }
      }
      if (name.empty()) evaluator_.error(node.at, "Native needs name: the component to draw");
    } else {
      name = program_.modules.at(node.module) + "." + node.name;
      for (std::size_t index = 0; index < node.arguments.size(); ++index) {
        const Argument& item = node.arguments[index];
        if (item.name == "flex" && main) continue;
        if (item.name == "id" && id != nullptr) continue;
        if (item.name.empty() && index != 0) evaluator_.error(item.at, "only the first argument may be unnamed");
        const std::string field = item.name.empty() ? "value" : item.name;
        props[field] = component_value(*item.value, scope, field);
      }
    }
    if (main) props[main->first] = Value(main->second);
    const auto found = state_.components.find(name);
    if (found == state_.components.end())
      evaluator_.error(node.at, std::format("no component '{}': the host registers components with Plugin::component", name));
    const Value value(std::move(props));
    const auto number = [&](std::string_view field) {
      const Value& item = value[field];
      return item.is_number() ? static_cast<float>(item.number()) : 0.0F;
    };
    declare(node, scope, 0, 0, number("width"), number("height"), value);
    const Children children(
        [this, &node, scope](vw::Ui& child_ui) {
          return node.children.empty() ? std::vector<vw::Widget>{} : block(child_ui, node.children, scope);
        },
        ui);
    const Component& draw = found->second;
    return evaluator_.at(node.at, [&] { return draw(ui, value, children); });
  }

  // A component's argument: event names take handlers, a bare name that is
  // not a variable is its own name (role: heading), "#rrggbb" is a color,
  // "50%" a share of the space around it, an object its fields.
  Value component_value(const Expr& expr, const ScopePtr& scope, const std::string& name) {
    if (event_name(name)) return Value(evaluator_.handler(expr, scope));
    if (expr.kind == Expr::Kind::conditional)
      return component_value(*expr.operands[evaluator_.evaluate(*expr.operands[0], scope).truthy() ? 1 : 2], scope, name);
    if (expr.kind == Expr::Kind::identifier && scope->find(expr.text) == nullptr && expr.text != "screen")
      return Value(expr.text);
    if (expr.kind == Expr::Kind::string)
      if (const auto color = parse_color(expr.text); color && expr.text != "transparent") return Value(*color);
    if (expr.kind == Expr::Kind::string && expr.text.size() > 1 && expr.text.back() == '%')
      return Value(percent(expr, name));
    if (expr.kind == Expr::Kind::object) {
      Value::Object fields;
      for (const auto& [field_name, field] : expr.fields) fields[field_name] = component_value(*field, scope, field_name);
      return Value(std::move(fields));
    }
    return evaluator_.evaluate(expr, scope);
  }

  const Plugin::State& state_;
  const Program& program_;
  Evaluator evaluator_;
  std::vector<Space> spaces_;
  std::set<std::string> names_;  // every id: name given so far
  const Node* top_level_ = nullptr;
};

std::filesystem::file_time_type modified_time(const std::filesystem::path& path) {
  std::error_code error;
  const auto time = std::filesystem::last_write_time(path, error);
  return error ? std::filesystem::file_time_type{} : time;
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw ScriptError(path.string(), 0, 0, "cannot read the file");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

}  // namespace

Plugin::Plugin(std::unique_ptr<State> state) : state_(std::move(state)) {}
Plugin::Plugin(Plugin&&) noexcept = default;
Plugin& Plugin::operator=(Plugin&&) noexcept = default;
Plugin::~Plugin() = default;

Plugin Plugin::parse(std::string source, std::string name) {
  auto state = std::make_unique<State>();
  state->program = compile_program(std::move(source), std::move(name));
  return Plugin(std::move(state));
}

Plugin Plugin::load(const std::filesystem::path& path) {
  const auto modified = modified_time(path);
  auto state = std::make_unique<State>();
  auto program = compile_program(read_text(path), path.string());
  program->path = path;
  program->modified = modified;
  state->program = std::move(program);
  return Plugin(std::move(state));
}

std::vector<std::string> Plugin::views() const {
  std::vector<std::string> names;
  for (const auto& view : state_->program->file.views) names.push_back(view.name);
  return names;
}

Plugin& Plugin::set(std::string name, Value value) {
  state_->globals[std::move(name)] = std::move(value);
  return *this;
}

Plugin& Plugin::component(std::string name, Component component) {
  state_->components[std::move(name)] = std::move(component);
  return *this;
}

Plugin& Plugin::on_error(std::function<void(const std::exception&)> report) {
  *state_->report = std::move(report);
  return *this;
}

vw::Widget Plugin::view(vw::Ui& ui, std::string_view name, Arguments arguments) const {
  const auto& program = state_->program;
  const auto found = program->views.find(name);
  if (found == program->views.end())
    throw ScriptError(program->name, 0, 0, std::format("there is no view '{}'", name));
  const View& view = *found->second;
  auto context = std::make_shared<ViewContext>();
  context->view = view.name;
  collect_ids(view.body, false, context->ids);
  Builder builder(*state_, std::move(context));
  return builder.run(ui, view, arguments);
}

bool Plugin::reload() {
  const auto& program = state_->program;
  if (program->path.empty()) return false;
  const auto modified = modified_time(program->path);
  if (modified == program->modified) return false;
  auto next = compile_program(read_text(program->path), program->path.string());
  next->path = program->path;
  next->modified = modified;
  state_->program = std::move(next);
  return true;
}

}  // namespace facet
