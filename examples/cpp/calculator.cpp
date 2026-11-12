// A calculator floating in the middle of the screen, at a fixed size. Click
// the keys or type: digits, + - * /, Enter or =, Backspace, and Escape to
// clear (Escape again closes it).
//
//   cmake --build build && ./build/vw-example-calculator
#include <vw/vw.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>

namespace {

struct State {
  double stored = 0;
  char pending = 0;      // the operator waiting for its right-hand side
  std::string entry = "0";
  bool fresh = true;     // the next digit starts a new entry
  std::string history;   // "12 ×" above the display
};

State state;

std::string format(double value) {
  if (!std::isfinite(value)) return "Error";
  return std::format("{}", value);
}

std::string_view symbol(char op) {
  switch (op) {
    case '+': return "+";
    case '-': return "−";
    case '*': return "×";
    case '/': return "÷";
    default: return "";
  }
}

double apply(double left, char op, double right) {
  switch (op) {
    case '+': return left + right;
    case '-': return left - right;
    case '*': return left * right;
    case '/': return left / right;
    default: return right;
  }
}

double entry_value() { return state.entry == "Error" ? 0 : std::stod(state.entry); }

void press(std::string_view key) {
  if (key.size() == 1 && ((key[0] >= '0' && key[0] <= '9') || key[0] == '.')) {
    if (state.fresh) state.entry = key[0] == '.' ? "0" : "";
    state.fresh = false;
    if (key[0] == '.' && state.entry.find('.') != std::string::npos) return;
    if (state.entry == "0" && key[0] != '.') state.entry.clear();
    state.entry += key;
  } else if (key == "+" || key == "-" || key == "*" || key == "/") {
    const double value = entry_value();
    state.stored = state.pending != 0 && !state.fresh ? apply(state.stored, state.pending, value) : value;
    state.pending = key[0];
    state.entry = format(state.stored);
    state.history = std::format("{} {}", state.entry, symbol(state.pending));
    state.fresh = true;
  } else if (key == "=") {
    if (state.pending == 0) return;
    const double value = entry_value();
    state.history = std::format("{} {} {} =", format(state.stored), symbol(state.pending), state.entry);
    state.entry = format(apply(state.stored, state.pending, value));
    state.pending = 0;
    state.fresh = true;
  } else if (key == "C") {
    state = {};
  } else if (key == "⌫") {
    if (state.fresh || state.entry == "Error") return;
    state.entry.pop_back();
    if (state.entry.empty() || state.entry == "-") state.entry = "0";
  } else if (key == "±") {
    if (state.entry != "0" && state.entry != "Error")
      state.entry = state.entry.front() == '-' ? state.entry.substr(1) : "-" + state.entry;
  } else if (key == "%") {
    state.entry = format(entry_value() / 100);
  }
}

struct Key {
  const char* label;
  const char* press;
  int kind;  // 0 digit, 1 function, 2 operator, 3 equals
};

constexpr Key keys[5][4] = {
    {{"C", "C", 1}, {"±", "±", 1}, {"%", "%", 1}, {"÷", "/", 2}},
    {{"7", "7", 0}, {"8", "8", 0}, {"9", "9", 0}, {"×", "*", 2}},
    {{"4", "4", 0}, {"5", "5", 0}, {"6", "6", 0}, {"−", "-", 2}},
    {{"1", "1", 0}, {"2", "2", 0}, {"3", "3", 0}, {"+", "+", 2}},
    {{"0", "0", 0}, {".", ".", 0}, {"⌫", "⌫", 1}, {"=", "=", 3}},
};

void build(vw::Ui& ui) {
  constexpr float width = 340;
  constexpr float height = 520;
  constexpr float pad = 16;
  constexpr float gap = 10;

  ui.rectangle({.width = width, .height = height, .radius = 32, .color = vw::rgb(0x151a1e),
                .border_width = 1, .border_color = vw::rgba(0xffffff14)});

  // Display, right-aligned, shrinking long numbers to fit.
  const float display_height = height * 0.28F;
  const vw::Widget history = ui.text({.text = state.history, .font = "sans", .size = 16,
                                      .color = vw::rgb(0x6f767d)});
  ui.move(history, width - pad - history.width, display_height - 76);
  const float digits = static_cast<float>(state.entry.size());
  const vw::Widget entry = ui.text({.text = state.entry, .font = "sans",
                                    .size = std::min(52.0F, (width - pad * 2) / (digits * 0.62F)),
                                    .weight = vw::weight::light, .color = vw::rgb(0xdfe6ee)});
  ui.move(entry, width - pad - entry.width, display_height - entry.height);

  // Keypad.
  const float key_width = (width - pad * 2 - gap * 3) / 4;
  const float key_height = (height - display_height - pad * 2 - gap * 4) / 5;
  for (int row = 0; row < 5; ++row) {
    for (int column = 0; column < 4; ++column) {
      const Key& key = keys[row][column];
      const vw::Color colors[] = {vw::rgb(0x20272c), vw::rgb(0x262d33), vw::rgb(0xa7cbe7),
                                  vw::rgb(0xb7c9d8)};
      const vw::Color hovers[] = {vw::rgb(0x262d33), vw::rgb(0x42494f), vw::rgb(0xbde1fe),
                                  vw::rgb(0xd3e5f5)};
      const bool pending = key.kind == 2 && state.pending == key.press[0] && state.fresh;
      const float x = pad + static_cast<float>(column) * (key_width + gap);
      const float y = display_height + pad + static_cast<float>(row) * (key_height + gap);
      const vw::Widget body = ui.rectangle({
          .width = key_width, .height = key_height, .radius = std::min(key_width, key_height) / 2,
          .color = pending ? vw::rgb(0xdfe6ee) : colors[key.kind],
          .input = {.on_click = [&key](const vw::ClickEvent&) { press(key.press); },
                    .hover = vw::Hover{.color = hovers[key.kind]}}});
      ui.move(body, x, y);
      const vw::Widget label = ui.text({
          .text = key.label, .font = "sans", .size = 22,
          .weight = key.kind >= 2 ? vw::weight::semibold : vw::weight::regular,
          .color = key.kind >= 2 ? vw::rgb(0x1f445b) : vw::rgb(0xdfe6ee)});
      ui.move(label, x + (key_width - label.width) / 2, y + (key_height - label.height) / 2);
    }
  }

  ui.keyboard([](const vw::KeyEvent& key) {
    if (key.name == "Return" || key.name == "KP_Enter" || key.text == "=") press("=");
    else if (key.name == "BackSpace") press("⌫");
    else if (key.name == "Escape" && state.entry == "0" && state.pending == 0) std::exit(EXIT_SUCCESS);
    else if (key.name == "Escape" || key.name == "Delete") press("C");
    else if (key.text == "," ) press(".");
    else if (key.text.size() == 1 && std::string_view("0123456789.+-*/%").contains(key.text[0]))
      press(key.text);
    else return false;
    return true;
  }, vw::Keyboard::exclusive);
  // A layer surface keeps its exact size: no tiling stretches the keypad.
  ui.surface({.anchor = vw::anchor::center, .layer = vw::Layer::top,
              .surface_namespace = "vw-calculator", .refresh_interval = 3600});
}

}  // namespace

int main() {
  vw::App app(build);
  return app.run();
}
