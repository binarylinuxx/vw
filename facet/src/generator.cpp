// Facet to C++: each view becomes a function template over its parameters'
// types, so clang checks every use of state and actions. #line directives
// send C++ errors back to the .facet line they came from.
#include "syntax.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>

namespace facet {
namespace {

enum class Type {
  number, integer, size, boolean, string, strings, color, handler, input, padding,
  anchor, layer, justify, align, weight, fit, text_align, gradient,
};

struct Property {
  std::string_view name;  // in Facet
  std::string_view field; // in the props struct
  Type type;
};

enum class Kind {
  surface, rectangle, text, image, text_field, row, column, grid, listview,
  control, native, external,
};

struct Element {
  std::string_view module;
  std::string_view name;
  Kind kind;
  std::string_view props_type;
  std::vector<Property> properties;
  std::string_view positional;  // what a positional first argument sets
  std::string_view call;        // the C++ that builds it, for controls
};

const std::vector<Element>& elements() {
  using enum Type;
  static const std::vector<Property> layout{
      {"gap", "gap", number}, {"padding", "padding", number}, {"width", "width", number},
      {"height", "height", number}, {"justify", "justify", Type::justify},
      {"align", "align", Type::align}, {"radius", "radius", number}};
  static const std::vector<Element> table{
      {"vw.ui", "Surface", Kind::surface, "vw::SurfaceProps",
       {{"width", "width", number}, {"height", "height", number}, {"anchor", "anchor", Type::anchor},
        {"layer", "layer", Type::layer}, {"exclusive_zone", "exclusive_zone", integer},
        {"namespace", "surface_namespace", string}, {"refresh", "refresh_interval", number},
        {"output", "output", string}}, "", ""},
      {"vw.ui", "Rectangle", Kind::rectangle, "vw::RectangleProps",
       {{"width", "width", number}, {"height", "height", number}, {"radius", "radius", number},
        {"color", "color", color}, {"bg", "color", color}, {"color2", "color2", color},
        {"gradient", "gradient", Type::gradient}, {"border_width", "border_width", number},
        {"border_color", "border_color", color}, {"input", "input", input}}, "", ""},
      {"vw.ui", "Text", Kind::text, "vw::TextProps",
       {{"text", "text", string}, {"font", "font", string}, {"size", "size", number},
        {"weight", "weight", Type::weight}, {"color", "color", color}, {"max_width", "max_width", number},
        {"max_lines", "max_lines", integer}, {"align", "align", text_align}, {"padding", "place.margin", Type::padding},
        {"input", "input", input}},
       "text", ""},
      {"vw.ui", "Image", Kind::image, "vw::ImageProps",
       {{"path", "path", string}, {"icon", "icon", string}, {"width", "width", number},
        {"height", "height", number}, {"fit", "fit", Type::fit}, {"radius", "radius", number},
        {"padding", "place.margin", Type::padding}, {"input", "input", input}}, "path", ""},
      {"vw.ui", "TextField", Kind::text_field, "vw::TextFieldProps",
       {{"id", "id", string}, {"value", "value", string}, {"placeholder", "placeholder", string},
        {"width", "width", number}, {"height", "height", number}, {"radius", "radius", number},
        {"color", "color", color}, {"border_color", "border_color", color},
        {"focus_color", "focus_color", color}, {"text_color", "text_color", color},
        {"placeholder_color", "placeholder_color", color}, {"font", "font", string},
        {"size", "size", number}, {"password", "password", boolean}, {"autofocus", "autofocus", boolean},
        {"on_change", "on_change", handler}, {"on_submit", "on_submit", handler}}, "", ""},
      {"vw.ui", "Toggle", Kind::control, "facet::ToggleProps",
       {{"id", "id", string}, {"value", "value", boolean}, {"width", "width", number},
        {"height", "height", number}, {"color", "color", color}, {"off_color", "off_color", color},
        {"thumb_color", "thumb_color", color}, {"on_change", "on_change", handler}}, "", "facet::toggle"},
      {"vw.ui", "Slider", Kind::control, "facet::SliderProps",
       {{"id", "id", string}, {"value", "value", number}, {"min", "min", number}, {"max", "max", number},
        {"step", "step", number}, {"width", "width", number}, {"height", "height", number},
        {"color", "color", color}, {"track_color", "track_color", color},
        {"thumb_color", "thumb_color", color}, {"on_change", "on_change", handler}}, "", "facet::slider"},
      {"vw.ui", "Dropdown", Kind::control, "facet::DropdownProps",
       {{"id", "id", string}, {"options", "options", strings}, {"current", "current", string},
        {"value", "current", string}, {"width", "width", number}, {"height", "height", number},
        {"color", "color", color}, {"hover_color", "hover_color", color},
        {"text_color", "text_color", color}, {"on_change", "on_change", handler}}, "", "facet::dropdown"},
      {"vw.ui", "Spinner", Kind::control, "facet::SpinnerProps",
       {{"id", "id", string}, {"size", "size", number}, {"color", "color", color}}, "", "facet::spinner"},
      {"vw.layout", "Row", Kind::row, "vw::LayoutProps", layout, "", ""},
      {"vw.layout", "Column", Kind::column, "vw::LayoutProps", layout, "", ""},
      {"vw.layout", "Grid", Kind::grid, "vw::GridProps",
       {{"columns", "columns", integer}, {"gap", "gap", number}, {"row_gap", "row_gap", number},
        {"padding", "padding", number}, {"width", "width", number}, {"height", "height", number},
        {"align", "align", Type::align}, {"radius", "radius", number}}, "", ""},
      {"vw.layout", "ListView", Kind::listview, "vw::ListviewProps",
       {{"id", "id", string}, {"width", "width", number}, {"height", "height", number},
        {"item_height", "item_height", number}, {"gap", "gap", number}, {"radius", "radius", number},
        {"scroll_step", "scroll_step", number}, {"count", "count", size}}, "", ""},
  };
  return table;
}

// Names Facet accepts for each enum, and the C++ they mean.
const std::map<std::string, std::string>& choices(Type type) {
  static const std::map<Type, std::map<std::string, std::string>> table{
      {Type::anchor, {{"center", "vw::anchor::center"}, {"top", "vw::anchor::top"},
                      {"bottom", "vw::anchor::bottom"}, {"left", "vw::anchor::left"},
                      {"right", "vw::anchor::right"}, {"top_left", "vw::anchor::top_left"},
                      {"top_right", "vw::anchor::top_right"}, {"bottom_left", "vw::anchor::bottom_left"},
                      {"bottom_right", "vw::anchor::bottom_right"}, {"fill", "vw::anchor::fill"}}},
      {Type::layer, {{"background", "vw::Layer::background"}, {"bottom", "vw::Layer::bottom"},
                     {"top", "vw::Layer::top"}, {"overlay", "vw::Layer::overlay"}}},
      {Type::justify, {{"start", "vw::Justify::start"}, {"center", "vw::Justify::center"},
                       {"end", "vw::Justify::end"}, {"space-between", "vw::Justify::space_between"},
                       {"space-around", "vw::Justify::space_around"},
                       {"space-evenly", "vw::Justify::space_evenly"}}},
      {Type::align, {{"start", "vw::Align::start"}, {"center", "vw::Align::center"}, {"end", "vw::Align::end"}}},
      {Type::weight, {{"thin", "vw::weight::thin"}, {"light", "vw::weight::light"},
                      {"regular", "vw::weight::regular"}, {"medium", "vw::weight::medium"},
                      {"semibold", "vw::weight::semibold"}, {"bold", "vw::weight::bold"},
                      {"black", "vw::weight::black"}}},
      {Type::fit, {{"stretch", "vw::ImageFit::stretch"}, {"contain", "vw::ImageFit::contain"},
                   {"cover", "vw::ImageFit::cover"}}},
      {Type::text_align, {{"start", "\"start\""}, {"center", "\"center\""}, {"end", "\"end\""}}},
      {Type::gradient, {{"none", "vw::Gradient::none"}, {"horizontal", "vw::Gradient::horizontal"},
                        {"vertical", "vw::Gradient::vertical"}}},
  };
  return table.at(type);
}

// align: on a Text that names a place in its parent rather than a line
// alignment.
const std::map<std::string, std::string>& placements() {
  static const std::map<std::string, std::string> table{
      {"left", "vw::anchor::left"}, {"right", "vw::anchor::right"}, {"top", "vw::anchor::top"},
      {"bottom", "vw::anchor::bottom"}, {"center_left", "vw::anchor::left"},
      {"center_right", "vw::anchor::right"}, {"top_center", "vw::anchor::top"},
      {"bottom_center", "vw::anchor::bottom"}, {"top_left", "vw::anchor::top_left"},
      {"top_right", "vw::anchor::top_right"}, {"bottom_left", "vw::anchor::bottom_left"},
      {"bottom_right", "vw::anchor::bottom_right"}};
  return table;
}

const std::set<std::string>& easings() {
  static const std::set<std::string> names{"linear", "in_out_cubic", "out_cubic", "out_quint",
                                           "out_expo", "out_back", "spring"};
  return names;
}

std::string quote(const std::string& text) {
  std::string result = "\"";
  for (const char c : text) {
    switch (c) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n"; break;
      case '\t': result += "\\t"; break;
      default: result += c;
    }
  }
  return result + '"';
}

bool is_hex_color(const std::string& text) {
  return (text.size() == 7 || text.size() == 9) && text.front() == '#' &&
         std::all_of(text.begin() + 1, text.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

bool is_percent(const Expr& value) {
  return value.kind == Expr::Kind::string && value.text.size() > 1 && value.text.back() == '%';
}

// The space an element's children lay out in, as C++ float expressions.
struct Box {
  std::optional<std::string> width;
  std::optional<std::string> height;
};

// A flex child waiting for the row or column to know its leftover space.
struct FlexSlot {
  const Node* node;
  std::string slot;    // C++ variable holding its index in the children
  std::string weight;  // its flex, as a C++ float
};

class Generator {
 public:
  Generator(const File& file, std::string source, std::string namespace_name, std::string default_module)
      : file_(file), source_(std::move(source)), namespace_(std::move(namespace_name)),
        default_module_(std::move(default_module)) {
    for (const auto& import : file.imports) modules_[import.alias] = import.path;
  }

  std::string run() {
    out_ << "// Generated by facetc from " << source_ << ". Do not edit.\n"
         << "#pragma once\n\n#include <vw/facet.hpp>\n";
    // include "x.hpp" → #include "x.hpp"; include "<format>" → #include <format>
    for (const auto& header : file_.includes) {
      if (header.text.empty()) throw Error(header.at, "include needs a header name");
      out_ << (header.text.front() == '<' ? "#include " + header.text : "#include " + quote(header.text)) << "\n";
    }
    out_ << "\n";
    if (!namespace_.empty()) out_ << "namespace " << namespace_ << " {\n\n";
    for (const auto& code : file_.prelude) verbatim(code.at, code.text);
    {
      Scope scope(*this);
      for (const auto& constant : file_.constants) {
        if (local(constant.name)) throw Error(constant.at, "'" + constant.name + "' is already defined");
        line(constant.at, std::format("inline const auto {} = {};", constant.name, expression(*constant.value)));
        declare(constant.name);
        constants_.insert(constant.name);
      }
    }
    if (!file_.constants.empty()) out_ << "\n";
    for (const auto& view : file_.views) generate(view);
    if (!namespace_.empty()) out_ << "}  // namespace " << namespace_ << "\n";
    return out_.str();
  }

 private:
  // --- output and scopes ----------------------------------------------------
  void line(Location at, const std::string& text) {
    out_ << "#line " << at.line << " " << quote(source_) << "\n"
         << std::string(static_cast<std::size_t>(indent_ * 2), ' ') << text << "\n";
  }
  std::string fresh(const char* stem) { return std::format("{}{}", stem, ++counter_); }

  // C++ from a cpp { } block, with #line so its errors point into the .facet.
  void verbatim(Location at, const std::string& code) {
    out_ << "#line " << at.line << " " << quote(source_) << "\n" << code << "\n";
  }

  // Names a cpp { } block seems to declare or assign (`auto total = …`,
  // `count = …`), so the Facet after it treats them as variables. facetc does
  // not parse C++; this only has to tell variables from enum names.
  static std::vector<std::string> assigned_names(const std::string& code) {
    std::vector<std::string> names;
    for (std::size_t at = 0; at < code.size(); ++at) {
      const auto letter = [&](std::size_t index) {
        return std::isalnum(static_cast<unsigned char>(code[index])) != 0 || code[index] == '_';
      };
      if (!letter(at) || (at > 0 && letter(at - 1))) continue;
      std::size_t end = at;
      while (end < code.size() && letter(end)) ++end;
      const bool member = at > 0 && (code[at - 1] == '.' || (at > 1 && code[at - 2] == '-' && code[at - 1] == '>'));
      std::size_t next = end;
      while (next < code.size() && (code[next] == ' ' || code[next] == '\t')) ++next;
      const bool assigned = next < code.size() && (code[next] == '=' || code[next] == '{') &&
                            (next + 1 >= code.size() || code[next + 1] != '=');
      if (assigned && !member && std::isdigit(static_cast<unsigned char>(code[at])) == 0)
        names.push_back(code.substr(at, end - at));
      at = end;
    }
    return names;
  }

  struct Scope {
    Generator& generator;
    explicit Scope(Generator& owner, const std::vector<std::string>& names = {}) : generator(owner) {
      generator.scopes_.emplace_back(names.begin(), names.end());
    }
    ~Scope() { generator.scopes_.pop_back(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
  };
  void declare(const std::string& name) { scopes_.back().insert(name); }
  // A lambda parameter or local named like a view parameter hides it.
  bool shadowed(const std::string& name) const {
    for (std::size_t index = 1; index < scopes_.size(); ++index)
      if (scopes_[index].contains(name)) return true;
    return false;
  }
  bool local(const std::string& name) const {
    return constants_.contains(name) ||
           std::ranges::any_of(scopes_, [&](const auto& scope) { return scope.contains(name); });
  }

  // id: name, with a bare name not already in use, names the element: its
  // props are `name` to its children and to the elements after it. A string
  // (or a variable) is a control's state key, as before.
  const Argument* element_id(const Node& node) const {
    const Argument* id = argument(node, "id");
    if (id == nullptr || id->value->kind != Expr::Kind::identifier) return nullptr;
    // Another element's name is still a name (a repeat is reported), not a key.
    if (local(id->value->text) && !element_names_.contains(id->value->text)) return nullptr;
    return id;
  }

  // Binds the element's props to its id, if it has one.
  void name_element(const Node& node, const std::string& props) {
    const Argument* id = element_id(node);
    if (id == nullptr) return;
    const std::string& name = id->value->text;
    line(id->at, std::format("facet::Named {}{{{}}};", name, props));
    declare(name);
    element_names_.insert(name);
  }

  // The key of state belonging to the element at `at`, in its loops.
  std::string key(Location at) const {
    std::string result = std::format("facet::key({}", quote(std::format("{}:{}:{}", source_, at.line, at.column)));
    for (const auto& loop : keys_) result += ", " + loop;
    return result + ")";
  }

  const Box& box() const { return boxes_.back(); }

  // --- views ----------------------------------------------------------------
  void generate(const View& view) {
    parameters_ = view.parameters;
    view_ = view.name;
    view_ids_.clear();
    collect_ids(view.body, false);
    Scope scope(*this, view.parameters);
    boxes_ = {Box{}};
    const bool surface = view.body.size() == 1 && view.body.front()->kind == Node::Kind::element &&
                         resolve(*view.body.front()).kind == Kind::surface;
    std::string templates;
    std::string signature = "vw::Ui& ui";
    for (std::size_t index = 0; index < view.parameters.size(); ++index) {
      templates += std::format("{}class T{}", index == 0 ? "" : ", ", index);
      signature += std::format(", T{}& {}", index, view.parameters[index]);
    }
    if (!templates.empty()) out_ << "template <" << templates << ">\n";
    out_ << (surface ? "void " : "vw::Widget ") << view.name << "(" << signature << ") {\n";
    ++indent_;
    // Handlers reach the view's parameters through these, so they see the
    // live objects without capturing them by name.
    for (const auto& name : view.parameters)
      line(view.at, std::format("[[maybe_unused]] auto* const {} = &{};", reference(name), name));
    if (surface) {
      element(*view.body.front());
    } else {
      const std::string children = block(view.body);
      line(view.at, std::format("return facet::group(ui, {});", children));
    }
    --indent_;
    out_ << "}\n\n";
  }

  // Emits the nodes into a fresh vector of widgets and returns its name.
  // With `flex`, flex children are left as slots for the caller to fill.
  std::string block(const Block& nodes, std::vector<FlexSlot>* flex = nullptr) {
    // Names given inside the block stay in it; below it they are read ahead.
    Scope scope(*this);
    const std::string list = fresh("children");
    line(nodes.empty() ? Location{} : nodes.front()->at, std::format("std::vector<vw::Widget> {};", list));
    for (const auto& node : nodes) statement(*node, list, flex);
    return list;
  }

  void statement(const Node& node, const std::string& list, std::vector<FlexSlot>* flex) {
    switch (node.kind) {
      case Node::Kind::let:
        line(node.at, std::format("const auto {} = {};", node.name, expression(*node.value)));
        declare(node.name);
        return;
      case Node::Kind::for_: {
        // A counter keys each item's state unless the loop gives a key.
        const std::string counter = fresh("item");
        line(node.at, std::format("for (std::size_t {}_next = 0; const auto& {} : {}) {{", counter, node.name,
                                  expression(*node.value)));
        ++indent_;
        line(node.at, std::format("const std::size_t {0} = {0}_next++;", counter));
        line(node.at, std::format("(void){};", counter));
        Scope scope(*this, {node.name});
        keys_.push_back(node.key ? expression(*node.key) : counter);
        for (const auto& child : node.children) statement(*child, list, nullptr);
        keys_.pop_back();
        --indent_;
        line(node.at, "}");
        return;
      }
      case Node::Kind::if_: {
        line(node.at, std::format("if ({}) {{", expression(*node.value)));
        ++indent_;
        {
          Scope scope(*this);
          for (const auto& child : node.children) statement(*child, list, nullptr);
        }
        --indent_;
        if (node.has_else) {
          line(node.at, "} else {");
          ++indent_;
          Scope scope(*this);
          for (const auto& child : node.otherwise) statement(*child, list, nullptr);
          --indent_;
        }
        line(node.at, "}");
        return;
      }
      case Node::Kind::render:
        throw Error(node.at, "a (index) => { … } row belongs inside a ListView");
      case Node::Kind::cpp:
        // Plain C++ statements where they stand: ui, the view's parameters
        // and loop variables are in scope, and what they declare is usable
        // by the Facet after them.
        verbatim(node.at, node.code);
        for (const auto& name : assigned_names(node.code)) declare(name);
        return;
      case Node::Kind::element: {
        const Argument* weight = argument(node, "flex");
        // In a component's own layout, flex is just a field it reads.
        if (weight != nullptr && flex == nullptr && resolve(node).kind == Kind::external) weight = nullptr;
        if (weight != nullptr) {
          if (flex == nullptr)
            throw Error(weight->at, "flex works on elements written directly inside a Row or Column");
          FlexSlot slot{&node, fresh("slot"), std::format("static_cast<float>({})", expression(*weight->value))};
          line(node.at, std::format("const std::size_t {} = {}.size();", slot.slot, list));
          line(node.at, std::format("{}.emplace_back();", list));
          flex->push_back(std::move(slot));
          return;
        }
        if (const Argument* id = element_id(node)) {
          // No braces: the elements after this one may use its name.
          if (scopes_.back().contains(id->value->text))
            throw Error(id->at, "id '" + id->value->text + "' is already used here");
          const std::string widget = element(node);
          if (!widget.empty()) {
            line(node.at, std::format("facet::measure(ui, {}, {});", id->value->text, widget));
            remember(*id);
            line(node.at, std::format("{}.push_back({});", list, widget));
          }
          return;
        }
        line(node.at, "{");
        ++indent_;
        line(node.at, std::format("{}.push_back({});", list, element(node)));
        --indent_;
        line(node.at, "}");
        return;
      }
    }
  }

  static const Argument* argument(const Node& node, std::string_view name) {
    for (const auto& item : node.arguments)
      if (item.name == name) return &item;
    return nullptr;
  }

  // --- elements -------------------------------------------------------------
  const Element& resolve(const Node& node) const {
    if (node.module.empty() && node.name == "Native") return native_;
    if (node.module.empty()) {
      if (default_module_ == "vw.ui" || default_module_ == "vw.layout") {
        for (const auto& element : elements())
          if (element.module == default_module_ && element.name == node.name) return element;
        throw Error(node.at, default_module_ + " has no element " + node.name);
      }
      if (!default_module_.empty()) return external_;
      throw Error(node.at, "'" + node.name + "' needs its module, e.g. ui." + node.name +
                               " (or run facetc with --module)");
    }
    const auto module = modules_.find(node.module);
    if (module == modules_.end())
      throw Error(node.at, "'" + node.module + "' is not imported; add it to let [ … ] in");
    if (module->second != "vw.ui" && module->second != "vw.layout") return external_;
    for (const auto& element : elements())
      if (element.module == module->second && element.name == node.name) return element;
    if (node.name == "Native") return native_;
    throw Error(node.at, module->second + " has no element " + node.name);
  }

  // Emits an element and returns the variable holding its widget (empty for
  // a surface, which ends the view). `main` sets width or height for a flex
  // child: {property, value}.
  std::string element(const Node& node, std::optional<std::pair<std::string, std::string>> main = {}) {
    const Element& spec = resolve(node);
    if (spec.kind == Kind::native) return native(node);
    if (spec.kind == Kind::external) return external(node, main);
    if (spec.kind == Kind::surface && indent_ != 1)
      throw Error(node.at, "a Surface must be the whole view");
    const bool layout = spec.kind == Kind::row || spec.kind == Kind::column || spec.kind == Kind::grid;
    const std::string props = fresh("props");
    line(node.at, std::format("{} {}{{}};", spec.props_type, props));

    // A layout's background is drawn first, so it lands behind the children.
    std::string back;
    if (layout && (argument(node, "bg") != nullptr || argument(node, "color") != nullptr)) {
      const std::string back_props = fresh("back_props");
      back = fresh("back");
      line(node.at, std::format("vw::RectangleProps {}{{}};", back_props));
      line(node.at, std::format("{0}.width = {0}.height = 1;", back_props));
      for (const auto& item : node.arguments) {
        if (item.name == "bg" || item.name == "color")
          assign(item.at, back_props + ".color", Type::color, *item.value);
        else if (item.name == "border_width")
          assign(item.at, back_props + ".border_width", Type::number, *item.value);
        else if (item.name == "border_color")
          assign(item.at, back_props + ".border_color", Type::color, *item.value);
        else if (item.name == "radius")
          assign(item.at, back_props + ".radius", Type::number, *item.value);
      }
      line(node.at, std::format("const vw::Widget {} = ui.rectangle({});", back, back_props));
    }

    // animate: { property: "width", duration: 200 } tweens that property.
    std::string animated;
    std::string duration = "280";
    if (const Argument* tween = argument(node, "animate")) {
      if (tween->value->kind != Expr::Kind::object)
        throw Error(tween->at, "animate is { property: \"width\", duration: 200 }, or animate(value, 200) on the value");
      for (const auto& [field_name, field] : tween->value->fields) {
        if (field_name == "property" && field->kind == Expr::Kind::string) animated = field->text;
        else if (field_name == "duration") duration = expression(*field);
        else throw Error(field->at, "animate has property and duration");
      }
      if (animated.empty()) throw Error(tween->at, "animate needs property: the name of a size or number");
    }

    for (std::size_t index = 0; index < node.arguments.size(); ++index) {
      const Argument& item = node.arguments[index];
      std::string name = item.name;
      if (layout && (name == "bg" || name == "color" || name == "border_width" || name == "border_color")) continue;
      if (name == "flex" || name == "animate") continue;
      if (name == "id" && &item == element_id(node)) continue;
      if (name.empty()) {
        if (index != 0 || spec.positional.empty())
          throw Error(item.at, std::string(spec.name) + " takes only named arguments here");
        name = std::string(spec.positional);
      }
      const auto property = std::ranges::find(spec.properties, name, &Property::name);
      if (property == spec.properties.end())
        throw Error(item.at, std::format("{} has no property '{}'{}", spec.name, name, hint(name)));
      const std::string target = props + "." + std::string(property->field);
      if (property->type == Type::text_align && item.value->kind == Expr::Kind::identifier &&
          placements().contains(item.value->text)) {
        // center_left, top_right, …: where the text sits in its parent.
        line(item.at, std::format("{}.place.anchor = {};", props, placements().at(item.value->text)));
        continue;
      }
      assign(item.at, target, property->type, *item.value, name);
      if (name == animated) {
        if (property->type != Type::number) throw Error(item.at, "only numbers and sizes animate");
        line(item.at, std::format("{0} = facet::tween({1}, static_cast<double>({0}), static_cast<double>({2}));",
                                  target, key(item.at), duration));
        animated.clear();
      }
    }
    if (!animated.empty())
      throw Error(argument(node, "animate")->at, std::format("animate names '{}', which is not set here", animated));
    if (main) {
      if (std::ranges::find(spec.properties, main->first, &Property::name) == spec.properties.end())
        throw Error(node.at, std::format("flex sets {}, which {} does not have", main->first, spec.name));
      line(node.at, std::format("{}.{} = {};", props, main->first, main->second));
    }
    if ((spec.kind == Kind::control || spec.kind == Kind::listview) &&
        (argument(node, "id") == nullptr || element_id(node) != nullptr))
      line(node.at, std::format("{}.id = {};", props, key(node.at)));
    name_element(node, props);

    const std::string widget = fresh("widget");
    switch (spec.kind) {
      case Kind::surface: {
        if (node.children.empty()) throw Error(node.at, "a Surface needs content");
        boxes_.push_back({argument(node, "width") ? std::optional(std::format("*{}.width", props)) : std::nullopt,
                          argument(node, "height") ? std::optional(std::format("*{}.height", props)) : std::nullopt});
        const std::string children = block(node.children);
        boxes_.pop_back();
        line(node.at, std::format("(void)facet::group(ui, {});", children));
        line(node.at, std::format("ui.surface({});", props));
        return {};
      }
      case Kind::rectangle: {
        line(node.at, std::format("vw::Widget {} = ui.rectangle({});", widget, props));
        if (!node.children.empty()) {
          boxes_.push_back({props + ".width", props + ".height"});
          const std::string children = block(node.children);
          boxes_.pop_back();
          line(node.at, std::format("{0} = facet::on(ui, {0}, {1}, {2}.radius);", widget, children, props));
        }
        return widget;
      }
      case Kind::text: case Kind::image: case Kind::text_field: case Kind::control: {
        if (!node.children.empty()) throw Error(node.at, std::string(spec.name) + " takes no children");
        static const std::map<Kind, std::string_view> calls{
            {Kind::text, "ui.text"}, {Kind::image, "facet::image"}, {Kind::text_field, "ui.text_field"}};
        const std::string call = spec.kind == Kind::control ? std::string(spec.call)
                                                            : std::string(calls.at(spec.kind));
        const bool helper = spec.kind == Kind::control || spec.kind == Kind::image;
        line(node.at, std::format("const vw::Widget {} = {}({}{});", widget, call, helper ? "ui, " : "", props));
        return widget;
      }
      case Kind::listview: return listview(node, props, widget);
      case Kind::row: case Kind::column: case Kind::grid: {
        const bool row = spec.kind == Kind::row;
        // The content box: its own width or height when set, else the
        // parent's, less the padding.
        const auto inner = [&](std::string_view axis, const std::optional<std::string>& parent)
            -> std::optional<std::string> {
          if (argument(node, axis)) return std::format("(*{0}.{1} - 2 * {0}.padding)", props, axis);
          if (parent) return std::format("({} - 2 * {}.padding)", *parent, props);
          return std::nullopt;
        };
        const Box parent = box();
        Box inside{inner("width", parent.width), inner("height", parent.height)};
        if (spec.kind == Kind::grid && inside.width)
          inside.width = std::format("(({0} - {1}.gap * ({1}.columns - 1)) / {1}.columns)", *inside.width, props);
        boxes_.push_back(inside);
        std::vector<FlexSlot> flex;
        const std::string children = block(node.children, spec.kind == Kind::grid ? nullptr : &flex);
        if (!flex.empty()) {
          // A row or column with flex children spans its parent's axis.
          const std::string axis = row ? "width" : "height";
          if (!argument(node, axis)) {
            const auto& span = row ? parent.width : parent.height;
            if (!span)
              throw Error(node.at, std::format("flex needs the {}'s {}: set {} here or on a parent",
                                               spec.name, axis, axis));
            line(node.at, std::format("{}.{} = {};", props, axis, *span));
          }
          std::string total = "0.0F";
          for (const auto& slot : flex) total += " + " + slot.weight;
          const std::string spare = fresh("spare");
          std::string slots;
          for (const auto& slot : flex) slots += (slots.empty() ? "" : ", ") + slot.slot;
          line(node.at, std::format("const std::array<std::size_t, {}> {}_slots{{{}}};", flex.size(), spare, slots));
          line(node.at, std::format("const float {0} = facet::flex_space({1}, {0}_slots, *{2}.{3}, {2}.padding, {2}.gap, {4});",
                                    spare, children, props, axis, row ? "true" : "false"));
          for (const auto& slot : flex) {
            line(slot.node->at, "{");
            ++indent_;
            const std::string share = std::format("{} * {} / ({})", spare, slot.weight, total);
            const std::string built = element(*slot.node, std::pair{axis, share});
            if (const Argument* id = element_id(*slot.node))
            {
              line(slot.node->at, std::format("facet::measure(ui, {}, {});", id->value->text, built));
              remember(*id);
            }
            line(slot.node->at, std::format("{}[{}] = {};", children, slot.slot, built));
            --indent_;
            line(slot.node->at, "}");
          }
        }
        boxes_.pop_back();
        static const std::map<Kind, std::string_view> calls{
            {Kind::row, "row"}, {Kind::column, "column"}, {Kind::grid, "grid"}};
        line(node.at, std::format("vw::Widget {} = facet::{}(ui, {}, {});", widget, calls.at(spec.kind),
                                  children, props));
        if (!back.empty()) line(node.at, std::format("{0} = facet::behind(ui, {1}, {0});", widget, back));
        return widget;
      }
      case Kind::native: case Kind::external: break;
    }
    return widget;
  }

  // ListView(…) { (index) => { … } }: the row for each visible index.
  std::string listview(const Node& node, const std::string& props, const std::string& widget) {
    if (node.children.size() != 1 || node.children.front()->kind != Node::Kind::render)
      throw Error(node.at, "a ListView holds one (index) => { … } row");
    // Width defaults to the space around it. Without item_height the first
    // row is measured; without height the list shows every row.
    if (argument(node, "width") == nullptr && box().width)
      line(node.at, std::format("{}.width = {};", props, *box().width));
    if (argument(node, "count") == nullptr) throw Error(node.at, "a ListView needs count: how many rows");
    const Node& render = *node.children.front();
    if (render.parameters.size() != 1) throw Error(render.at, "a row takes one parameter: its index");
    // Rows render while the list is built, so they may refer to everything
    // around them; `ui` is whichever Ui the row is drawn into.
    const std::string rows = fresh("rows");
    line(render.at, std::format("const auto {} = [&](vw::Ui& ui, std::size_t {}) -> vw::Widget {{", rows,
                                render.parameters.front()));
    ++indent_;
    {
      Scope scope(*this, render.parameters);
      keys_.push_back(render.parameters.front());
      boxes_.push_back({std::format("{}.width", props), std::format("{}.item_height", props)});
      const std::string children = block(render.children);
      boxes_.pop_back();
      keys_.pop_back();
      line(render.at, std::format("return facet::group(ui, {});", children));
    }
    --indent_;
    line(render.at, "};");
    line(node.at, std::format("const vw::Widget {} = facet::listview(ui, {}, {});", widget, props, rows));
    return widget;
  }

  // Native(name: Graph, …) calls the C++ function Graph(ui, GraphProps) with
  // the other arguments, and the fields of props: { … }, set by name.
  std::string native(const Node& node) {
    std::string function;
    std::vector<std::pair<std::string, const Expr*>> fields;
    for (const auto& item : node.arguments) {
      if (item.name == "name" || item.name == "class") {
        if (item.value->kind != Expr::Kind::identifier && item.value->kind != Expr::Kind::string)
          throw Error(item.at, "Native's name is the C++ function to call");
        function = item.value->text;
      } else if (item.name == "props") {
        if (item.value->kind != Expr::Kind::object) throw Error(item.at, "props is an object: { … }");
        for (const auto& [name, value] : item.value->fields) fields.emplace_back(name, value.get());
      } else if (item.name == "id" && &item == element_id(node)) {
        continue;
      } else if (item.name.empty()) {
        throw Error(item.at, "Native takes only named arguments");
      } else {
        fields.emplace_back(item.name, item.value.get());
      }
    }
    if (function.empty()) throw Error(node.at, "Native needs name: the C++ function to call");
    return call_component(node, function, fields);
  }

  // S.Text(role: heading, …) from `let [ JES.spectralium as S ]` calls
  // JES::spectralium::Text(ui, TextProps), plus the children when it has any.
  // A positional first argument sets the field `value`: Text("Settings").
  std::string external(const Node& node, const std::optional<std::pair<std::string, std::string>>& main) {
    std::string function;
    const std::string& path = node.module.empty() ? default_module_ : modules_.at(node.module);
    for (const char c : path) function += c == '.' ? std::string("::") : std::string(1, c);
    function += "::" + node.name;
    std::vector<std::pair<std::string, const Expr*>> fields;
    for (std::size_t index = 0; index < node.arguments.size(); ++index) {
      const Argument& item = node.arguments[index];
      if (item.name == "flex" && main) continue;  // a vw Row or Column sized it
      if (item.name == "id" && &item == element_id(node)) continue;
      if (item.name.empty() && index != 0) throw Error(item.at, "only the first argument may be unnamed");
      fields.emplace_back(item.name.empty() ? "value" : item.name, item.value.get());
    }
    return call_component(node, function, fields, main);
  }

  std::string call_component(const Node& node, const std::string& function,
                             const std::vector<std::pair<std::string, const Expr*>>& fields,
                             const std::optional<std::pair<std::string, std::string>>& main = {}) {
    const std::string props = fresh("props");
    const std::string widget = fresh("widget");
    line(node.at, std::format("{}Props {}{{}};", function, props));
    for (const auto& [name, value] : fields) component_field(props + "." + name, name, *value);
    if (main) line(node.at, std::format("{}.{} = {};", props, main->first, main->second));
    name_element(node, props);
    if (node.children.empty()) {
      line(node.at, std::format("const vw::Widget {} = {}(ui, {});", widget, function, props));
    } else {
      // The children are built when the component asks for them, so it can
      // draw its background first and have them land on top.
      const std::string children = fresh("children_of");
      line(node.at, std::format("const facet::Children {}([&](vw::Ui& ui) {{", children));
      ++indent_;
      const std::string list = block(node.children);
      line(node.at, std::format("return {};", list));
      --indent_;
      line(node.at, "}, ui);");
      line(node.at, std::format("const vw::Widget {} = {}(ui, {}, {});", widget, function, props, children));
    }
    return widget;
  }

  // Values for C++ components: a bare name that is not a variable here is an
  // enum value of the field's type (role: heading), "#rrggbb" is a color,
  // "50%" is a share of the space around it, a lambda is a handler.
  // One field of a component's props; an object sets its fields one by one.
  void component_field(const std::string& target, const std::string& name, const Expr& value) {
    if (value.kind == Expr::Kind::object) {
      for (const auto& [field_name, field] : value.fields) component_field(target + "." + field_name, field_name, *field);
      return;
    }
    line(value.at, std::format("{} = {};", target, component_value(value, target, name)));
  }

  // onPress, on_click, onChange: fields named like events take handlers.
  static bool event_name(const std::string& name) {
    return name.size() > 2 && name.starts_with("on") &&
           (name[2] == '_' || std::isupper(static_cast<unsigned char>(name[2])) != 0);
  }

  std::string component_value(const Expr& value, const std::string& target, const std::string& name) {
    if (event_name(name)) return handler(value);
    if (value.kind == Expr::Kind::conditional)
      return std::format("({} ? {} : {})", expression(*value.operands[0]),
                         component_value(*value.operands[1], target, name),
                         component_value(*value.operands[2], target, name));
    if (value.kind == Expr::Kind::identifier && !local(value.text))
      return std::format("decltype({})::{}", target, enum_name(value.text));
    if (value.kind == Expr::Kind::string && is_hex_color(value.text)) return color(value);
    if (is_percent(value)) return percent(value, name);
    if (value.kind == Expr::Kind::lambda) return lambda(value, true);
    return expression(value);
  }

  static std::string enum_name(std::string name) {
    std::ranges::replace(name, '-', '_');
    return name;
  }

  // --- properties -----------------------------------------------------------
  void assign(Location at, const std::string& target, Type type, const Expr& value, const std::string& name = {}) {
    if (type == Type::input) {
      if (value.kind != Expr::Kind::object) throw Error(value.at, "input is an object: { on_click: …, hover: { … } }");
      for (const auto& [field_name, field] : value.fields) {
        if (field_name == "on_click" || field_name == "on_hover" || field_name == "on_scroll" ||
            field_name == "on_scroll_x" || field_name == "on_touch" || field_name == "on_drag" ||
            field_name == "on_drag_start" || field_name == "on_drag_end") {
          line(field->at, std::format("{}.{} = {};", target, field_name, handler(*field)));
        } else if (field_name == "hover") {
          if (field->kind != Expr::Kind::object) throw Error(field->at, "hover is an object: { color: …, scale: … }");
          line(field->at, std::format("{}.hover = vw::Hover{{}};", target));
          for (const auto& [hover_name, hover_value] : field->fields) {
            if (hover_name == "color") line(hover_value->at, std::format("{}.hover->color = {};", target, convert(Type::color, *hover_value)));
            else if (hover_name == "scale") line(hover_value->at, std::format("{}.hover->scale = {};", target, convert(Type::number, *hover_value)));
            else throw Error(hover_value->at, "hover has color and scale, not '" + hover_name + "'");
          }
        } else {
          throw Error(field->at, "input has no '" + field_name + "'");
        }
      }
      return;
    }
    line(at, std::format("{} = {};", target, convert(type, value, name)));
  }

  // "50%" of the content box around this element, along the property's axis.
  std::string percent(const Expr& value, const std::string& name) {
    const std::string number = value.text.substr(0, value.text.size() - 1);
    if (number.empty() || !std::ranges::all_of(number, [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '.'; }))
      throw Error(value.at, "a relative size is a number and %: \"100%\"");
    const bool horizontal = name == "width" || name == "max_width";
    const bool vertical = name == "height" || name == "item_height";
    if (!horizontal && !vertical) throw Error(value.at, "only widths and heights can be relative");
    const auto& span = horizontal ? box().width : box().height;
    if (!span)
      throw Error(value.at, std::format("\"{}\" of what? Give a parent a {}", value.text,
                                        horizontal ? "width" : "height"));
    return std::format("static_cast<float>({} * {} / 100.0F)", *span, number);
  }

  std::string convert(Type type, const Expr& value, const std::string& name = {}) {
    if (value.kind == Expr::Kind::conditional && type != Type::handler)
      return std::format("({} ? {} : {})", expression(*value.operands[0]), convert(type, *value.operands[1], name),
                         convert(type, *value.operands[2], name));
    if (is_percent(value)) {
      if (type != Type::number) throw Error(value.at, "only sizes can be relative");
      return percent(value, name);
    }
    switch (type) {
      case Type::number: return std::format("static_cast<float>({})", expression(value));
      case Type::integer: return std::format("static_cast<int>({})", expression(value));
      case Type::size: return std::format("static_cast<std::size_t>({})", expression(value));
      case Type::boolean: return std::format("static_cast<bool>({})", expression(value));
      case Type::string:
        return value.kind == Expr::Kind::string ? quote(value.text)
                                                : std::format("facet::to_string({})", expression(value));
      case Type::strings:
        if (value.kind == Expr::Kind::array) {
          std::string items;
          for (const auto& item : value.operands)
            items += (items.empty() ? "" : ", ") + convert(Type::string, *item);
          return std::format("std::vector<std::string>{{{}}}", items);
        }
        return std::format("facet::strings({})", expression(value));
      case Type::color: return color(value);
      case Type::handler: return handler(value);
      case Type::padding: return std::format("facet::margin({})", expression(value));
      case Type::input: throw Error(value.at, "input is an object");
      default: {
        if (value.kind != Expr::Kind::identifier && value.kind != Expr::Kind::string)
          throw Error(value.at, "expected one of: " + options(type));
        const auto& names = choices(type);
        const auto found = names.find(value.text);
        if (found == names.end()) {
          std::string allowed = options(type);
          if (type == Type::text_align)
            for (const auto& [place, anchor] : placements()) allowed += ", " + place;
          throw Error(value.at, std::format("'{}' is not one of: {}", value.text, allowed));
        }
        return found->second;
      }
    }
  }

  std::string color(const Expr& value) {
    if (value.kind == Expr::Kind::string) {
      if (value.text == "transparent") return "vw::rgba(0x00000000)";
      if (!is_hex_color(value.text))
        throw Error(value.at, "colors are \"#rrggbb\", \"#rrggbbaa\" or \"transparent\"");
      return value.text.size() == 7 ? std::format("vw::rgb(0x{})", value.text.substr(1))
                                    : std::format("vw::rgba(0x{})", value.text.substr(1));
    }
    return expression(value);  // a C++ vw::Color in scope
  }

  std::string handler(const Expr& value) {
    if (value.kind == Expr::Kind::lambda) return lambda(value, true);
    if (value.kind == Expr::Kind::call) {
      // onPress: actions.select(tab.id) runs the call on the event, not now.
      ++lambdas_;
      const std::string call = expression(value);
      --lambdas_;
      return std::format("[{}](const auto&...) {{ {}; }}", captures(), call);
    }
    // A function reference: forward whatever vw passes.
    ++lambdas_;
    const std::string target = expression(value);
    --lambdas_;
    return std::format("[{}](auto&&... arguments) {{ {}(std::forward<decltype(arguments)>(arguments)...); }}",
                       captures(), target);
  }

  std::string lambda(const Expr& value, bool statement) {
    std::string parameters;
    if (value.parameters.empty()) {
      parameters = "const auto&...";
    } else {
      for (const auto& name : value.parameters)
        parameters += std::format("{}const auto& {}", parameters.empty() ? "" : ", ", name);
    }
    Scope scope(*this, value.parameters);
    if (value.cpp_body) {
      // () => cpp { … }: the block is the body. The view's parameters are the
      // live objects (references), not copies.
      std::string aliases;
      for (const auto& name : parameters_)
        aliases += std::format(" [[maybe_unused]] auto& {} = *{};", name, reference(name));
      return std::format("[{}]({}) {{{}\n#line {} {}\n{}\n}}", captures(), parameters, aliases, value.code_at.line,
                         quote(source_), value.code);
    }
    ++lambdas_;
    struct Leave { int& depth; ~Leave() { --depth; } } leave{lambdas_};
    if (value.block) {
      std::string body;
      for (const auto& [name, item] : value.fields) {
        if (name.empty()) {
          body += std::format(" {};", expression(*item));
        } else {
          body += std::format(" const auto {} = {};", name, expression(*item));
          declare(name);
        }
      }
      return std::format("[{}]({}) {{{} }}", captures(), parameters, body);
    }
    const std::string body = expression(*value.operands[0]);
    return statement ? std::format("[{}]({}) {{ {}; }}", captures(), parameters, body)
                     : std::format("[{}]({}) {{ return {}; }}", captures(), parameters, body);
  }

  // Handlers outlive the build: copy locals, but keep the view's parameters
  // (state, actions) by reference so handlers see the live objects.
  std::string captures() const { return "="; }
  static std::string reference(const std::string& name) { return "facet_" + name; }
  bool parameter(const std::string& name) const { return std::ranges::find(parameters_, name) != parameters_.end(); }

  std::string options(Type type) const {
    std::string result;
    for (const auto& [name, cpp] : choices(type)) result += (result.empty() ? "" : ", ") + name;
    return result;
  }

  static std::string hint(const std::string& name) {
    if (name == "padding") return " (padding belongs to a layout around it)";
    if (name == "bg") return " (backgrounds go on Rectangle, Row, Column or Grid)";
    return "";
  }

  // animate(target, milliseconds[, easing]): a tween keyed by this place in
  // the file and the loops around it.
  std::string animate(const Expr& call) {
    const std::size_t count = call.operands.size() - 1;
    if (count < 2 || count > 3) throw Error(call.at, "animate(target, milliseconds[, easing])");
    std::string easing;
    if (count == 3) {
      const Expr& name = *call.operands[3];
      if (name.kind != Expr::Kind::identifier || !easings().contains(name.text))
        throw Error(name.at, "easing is one of: linear, in_out_cubic, out_cubic, out_quint, out_expo, out_back, spring");
      easing = ", vw::Easing::" + name.text;
    }
    return std::format("facet::tween({}, static_cast<double>({}), static_cast<double>({}){})", key(call.at),
                       expression(*call.operands[1]), expression(*call.operands[2]), easing);
  }

  // --- expressions ----------------------------------------------------------
  std::string expression(const Expr& value) {
    using K = Expr::Kind;
    switch (value.kind) {
      case K::number: return value.text;
      case K::string: return quote(value.text);
      case K::boolean: return value.text;
      case K::null: return "std::nullopt";
      case K::identifier:
        // Inside a handler a view parameter is reached through its pointer.
        if (lambdas_ > 0 && parameter(value.text) && !shadowed(value.text)) return "(*" + reference(value.text) + ")";
        // The monitor being drawn on, unless the view names something screen.
        if (value.text == "screen" && !local(value.text) && !parameter(value.text)) return "facet::screen()";
        // An element named further down (or inside another block): its box
        // from the previous frame.
        if (!local(value.text) && !parameter(value.text))
          if (const auto found = view_ids_.find(value.text); found != view_ids_.end()) {
            if (found->second != Seen::once)
              throw Error(value.at, std::format("'{}' is {}, so it cannot be read before or outside it", value.text,
                                                found->second == Seen::looped ? "named inside a loop" : "named more than once"));
            return std::format("facet::previous({})", quote(id_key(value.text)));
          }
        return value.text;
      case K::member:
        if (value.text == "length") return std::format("std::size({})", expression(*value.operands[0]));
        return std::format("{}.{}", expression(*value.operands[0]), value.text);
      case K::index:
        return std::format("{}[{}]", expression(*value.operands[0]), expression(*value.operands[1]));
      case K::call: {
        const Expr& callee = *value.operands[0];
        if (callee.kind == K::identifier && callee.text == "animate" && !local("animate")) return animate(value);
        std::string arguments;
        for (std::size_t index = 1; index < value.operands.size(); ++index)
          arguments += (index == 1 ? "" : ", ") + expression(*value.operands[index]);
        return std::format("{}({})", expression(callee), arguments);
      }
      case K::unary: return std::format("({}{})", value.text, expression(*value.operands[0]));
      case K::binary:
        if (value.text == "+")
          return std::format("facet::add({}, {})", expression(*value.operands[0]), expression(*value.operands[1]));
        return std::format("({} {} {})", expression(*value.operands[0]), value.text, expression(*value.operands[1]));
      case K::conditional:
        return std::format("({} ? {} : {})", expression(*value.operands[0]), expression(*value.operands[1]),
                           expression(*value.operands[2]));
      case K::lambda: return lambda(value, false);
      case K::array: {
        std::string items;
        for (const auto& item : value.operands) items += (items.empty() ? "" : ", ") + expression(*item);
        return std::format("std::vector{{{}}}", items);
      }
      case K::object: throw Error(value.at, "objects go in input:, Native props and component props only");
    }
    return {};
  }

  const File& file_;
  std::string source_;
  std::string namespace_;
  std::string default_module_;
  std::map<std::string, std::string> modules_;
  std::vector<std::string> parameters_;
  std::vector<std::set<std::string>> scopes_;
  std::set<std::string> constants_;      // file-level let
  std::set<std::string> element_names_;  // every id: name so far
  // Every element name in the view being generated, found before generating
  // it, for reading names ahead of where they are given.
  enum class Seen : std::uint8_t { once, looped, repeated };
  std::map<std::string, Seen> view_ids_;
  std::string view_;

  void collect_ids(const Block& nodes, bool looped) {
    for (const auto& node : nodes) {
      if (node->kind == Node::Kind::element)
        if (const Argument* id = argument(*node, "id"); id != nullptr && id->value->kind == Expr::Kind::identifier) {
          const auto [entry, added] = view_ids_.emplace(id->value->text, looped ? Seen::looped : Seen::once);
          if (!added) entry->second = Seen::repeated;
        }
      const bool inner = looped || node->kind == Node::Kind::for_ || node->kind == Node::Kind::render;
      collect_ids(node->children, inner);
      collect_ids(node->otherwise, inner);
    }
  }

  std::string id_key(const std::string& name) const { return std::format("{}:{}.{}", source_, view_, name); }

  // Keeps an element's measured box for the names read ahead of it.
  void remember(const Argument& id) {
    const auto found = view_ids_.find(id.value->text);
    if (found == view_ids_.end() || found->second != Seen::once) return;
    line(id.at, std::format("facet::remember({}, {});", quote(id_key(id.value->text)), id.value->text));
  }
  std::vector<std::string> keys_;
  std::vector<Box> boxes_;
  std::ostringstream out_;
  int indent_ = 0;
  int counter_ = 0;
  int lambdas_ = 0;  // inside how many lambdas the current expression is
  Element native_{"", "Native", Kind::native, "", {}, "", ""};
  Element external_{"", "", Kind::external, "", {}, "", ""};
};

}  // namespace

std::string generate(const File& file, const std::string& source_name, const std::string& namespace_name,
                     const std::string& default_module) {
  return Generator(file, source_name, namespace_name, default_module).run();
}

}  // namespace facet
