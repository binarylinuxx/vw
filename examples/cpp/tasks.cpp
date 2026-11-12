// A to-do list in an ordinary resizable window: add tasks with Enter, tick
// them off, filter, delete and clear finished ones. Tasks persist in
// $XDG_DATA_HOME/vw-tasks/tasks.txt (default ~/.local/share).
//
//   cmake --build build && ./build/vw-example-tasks
#include <vw/text.hpp>
#include <vw/vw.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr const char* input_id = "tasks-input";
constexpr const char* list_id = "tasks-list";
constexpr const char* font = "sans";

struct Task {
  std::string text;
  bool done = false;
};

enum class Filter { all, active, done };

struct State {
  std::vector<Task> tasks;
  std::string draft;
  Filter filter = Filter::all;
};

State state;

const vw::Color background = vw::rgb(0x1e1e2e);
const vw::Color panel = vw::rgb(0x181825);
const vw::Color raised = vw::rgb(0x313244);
const vw::Color hover = vw::rgb(0x45475a);
const vw::Color text = vw::rgb(0xcdd6f4);
const vw::Color muted = vw::rgb(0x7f849c);
const vw::Color accent = vw::rgb(0xa6e3a1);
const vw::Color danger = vw::rgb(0xf38ba8);

std::filesystem::path store_path() {
  const char* data = std::getenv("XDG_DATA_HOME");
  const char* home = std::getenv("HOME");
  const std::filesystem::path base =
      data != nullptr && *data != '\0' ? std::filesystem::path(data)
                                       : std::filesystem::path(home != nullptr ? home : ".") /
                                             ".local/share";
  return base / "vw-tasks" / "tasks.txt";
}

// One task per line: "x text" when done, "- text" otherwise.
void load() {
  std::ifstream input(store_path());
  std::string line;
  while (std::getline(input, line)) {
    if (line.size() < 3 || line[1] != ' ') continue;
    state.tasks.push_back({line.substr(2), line[0] == 'x'});
  }
}

void save() {
  const auto path = store_path();
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  std::ofstream output(path, std::ios::trunc);
  for (const Task& task : state.tasks) output << (task.done ? "x " : "- ") << task.text << '\n';
}

void add_draft() {
  const auto first = state.draft.find_first_not_of(' ');
  if (first == std::string::npos) return;
  const auto last = state.draft.find_last_not_of(' ');
  state.tasks.push_back({state.draft.substr(first, last - first + 1)});
  state.draft.clear();
  vw::reset_text_field(input_id);
  save();
}

bool shown(const Task& task) {
  switch (state.filter) {
    case Filter::active: return !task.done;
    case Filter::done: return task.done;
    case Filter::all: break;
  }
  return true;
}

// A rounded label button; returns its widget at the given position.
vw::Widget button(vw::Ui& ui, const std::string& label, float x, float y, bool active,
                  vw::ClickHandler on_click) {
  // Measure the caption so the button can be drawn underneath it.
  const float width = vw::caret_stops(label, font, 13, vw::weight::medium).back().x + 24;
  const vw::Widget body = ui.rectangle({
      .width = width, .height = 30, .radius = 15, .color = active ? accent : raised,
      .place = {.x = x, .y = y},
      .input = {.on_click = std::move(on_click),
                .hover = active ? std::nullopt : std::optional(vw::Hover{.color = hover})}});
  const vw::Widget caption = ui.text({.text = label, .font = font, .size = 13,
                                      .weight = vw::weight::medium,
                                      .color = active ? background : text});
  ui.move(caption, x + (width - caption.width) / 2, y + (30 - caption.height) / 2);
  return {body.begin, caption.end, width, 30};
}

vw::Widget task_row(vw::Ui& ui, std::size_t index, float width) {
  constexpr float height = 48;
  const Task& task = state.tasks[index];
  const vw::Widget row = ui.rectangle({.width = width, .height = height, .radius = 12,
                                       .color = panel,
                                       .input = {.hover = vw::Hover{.color = raised}}});
  ui.rectangle({
      .width = 22, .height = 22, .radius = 11, .color = task.done ? accent : panel,
      .border_width = 2, .border_color = task.done ? accent : muted,
      .place = {.x = 14, .y = (height - 22) / 2},
      .input = {.on_click = [index](const vw::ClickEvent&) {
                  state.tasks[index].done = !state.tasks[index].done;
                  save();
                },
                .hover = vw::Hover{.scale = 1.1F}}});
  if (task.done) {
    const vw::Widget mark = ui.text({.text = "✓", .font = font, .size = 13,
                                     .weight = vw::weight::bold, .color = background});
    ui.move(mark, 14 + (22 - mark.width) / 2, (height - mark.height) / 2);
  }
  const vw::Widget label = ui.text({.text = task.text, .font = font, .size = 15,
                                    .color = task.done ? muted : text,
                                    .max_width = width - 100, .max_lines = 1});
  ui.move(label, 50, (height - label.height) / 2);
  ui.rectangle({
      .width = 28, .height = 28, .radius = 14, .color = panel,
      .place = {.x = width - 40, .y = (height - 28) / 2},
      .input = {.on_click = [index](const vw::ClickEvent&) {
                  state.tasks.erase(state.tasks.begin() + static_cast<std::ptrdiff_t>(index));
                  save();
                },
                .hover = vw::Hover{.color = raised}}});
  const vw::Widget cross = ui.text({.text = "✕", .font = font, .size = 13, .color = danger});
  ui.move(cross, width - 40 + (28 - cross.width) / 2, (height - cross.height) / 2);
  return {row.begin, cross.end, width, height};
}

void build(vw::Ui& ui) {
  ui.toplevel({.width = 480, .height = 640, .title = "Tasks", .app_id = "vw-tasks"});
  const vw::Size size = vw::toplevel_size();
  const float width = std::max(320.0F, static_cast<float>(size.width));
  const float height = std::max(360.0F, static_cast<float>(size.height));
  constexpr float margin = 20;
  const float inner = width - margin * 2;

  ui.rectangle({.width = width, .height = height, .color = background});

  // Header: title and how much is left.
  const auto left = std::count_if(state.tasks.begin(), state.tasks.end(),
                                  [](const Task& task) { return !task.done; });
  const vw::Widget title = ui.text({.text = "Tasks", .font = font, .size = 26,
                                    .weight = vw::weight::bold, .color = text});
  ui.move(title, margin, margin);
  const std::string summary = state.tasks.empty() ? "Nothing to do"
                              : left == 0         ? "All done"
                                                  : std::to_string(left) + " left";
  const vw::Widget count = ui.text({.text = summary, .font = font, .size = 14, .color = muted});
  ui.move(count, width - margin - count.width, margin + (title.height - count.height) / 2);

  // New task field; Enter or the button adds it.
  const float field_y = margin + title.height + 16;
  constexpr float add_width = 72;
  ui.text_field({.id = input_id, .value = state.draft, .placeholder = "What needs doing?",
                 .width = inner - add_width - 8, .height = 44, .radius = 12,
                 .border_width = 1, .color = panel, .border_color = raised,
                 .focus_color = accent, .text_color = text, .placeholder_color = muted,
                 .font = font, .size = 15, .autofocus = true, .max_length = 200,
                 .on_change = [](const std::string& value) { state.draft = value; },
                 .on_submit = [](const std::string&) { add_draft(); },
                 .place = {.x = margin, .y = field_y}});
  const bool can_add = state.draft.find_first_not_of(' ') != std::string::npos;
  ui.rectangle({.width = add_width, .height = 44, .radius = 12,
                .color = can_add ? accent : raised,
                .place = {.x = width - margin - add_width, .y = field_y},
                .input = {.on_click = [](const vw::ClickEvent&) { add_draft(); },
                          .hover = vw::Hover{.scale = 1.03F}}});
  const vw::Widget add = ui.text({.text = "Add", .font = font, .size = 14,
                                  .weight = vw::weight::semibold,
                                  .color = can_add ? background : muted});
  ui.move(add, width - margin - add_width + (add_width - add.width) / 2,
          field_y + (44 - add.height) / 2);

  // Filters.
  const float filter_y = field_y + 44 + 14;
  float x = margin;
  const std::pair<const char*, Filter> filters[] = {
      {"All", Filter::all}, {"Active", Filter::active}, {"Done", Filter::done}};
  for (const auto& [label, filter] : filters) {
    const vw::Widget chip = button(ui, label, x, filter_y, state.filter == filter,
                                   [filter](const vw::ClickEvent&) {
                                     state.filter = filter;
                                   });
    x += chip.width + 8;
  }
  const bool any_done = std::any_of(state.tasks.begin(), state.tasks.end(),
                                    [](const Task& task) { return task.done; });
  if (any_done) {
    const vw::Widget clear = ui.text({.text = "Clear done", .font = font, .size = 13,
                                      .color = danger,
                                      .input = {.on_click = [](const vw::ClickEvent&) {
                                        std::erase_if(state.tasks,
                                                      [](const Task& task) { return task.done; });
                                        save();
                                      }}});
    ui.move(clear, width - margin - clear.width, filter_y + (30 - clear.height) / 2);
  }

  // The list, scrolled and virtualized; only rows in view are built.
  std::vector<std::size_t> visible;
  for (std::size_t index = 0; index < state.tasks.size(); ++index)
    if (shown(state.tasks[index])) visible.push_back(index);
  const float list_y = filter_y + 30 + 16;
  const float list_height = height - list_y - margin;
  if (visible.empty()) {
    const char* message = state.filter == Filter::done ? "Nothing finished yet"
                          : state.tasks.empty()        ? "Add your first task above"
                                                       : "Nothing left to do";
    const vw::Widget empty = ui.text({.text = message, .font = font, .size = 15, .color = muted});
    ui.move(empty, (width - empty.width) / 2, list_y + 40);
  } else {
    ui.listview({.id = list_id, .width = inner, .height = list_height, .item_height = 48,
                 .gap = 6, .count = visible.size(),
                 .render = [&ui, &visible, inner](std::size_t row) -> std::optional<vw::Widget> {
                   return task_row(ui, visible[row], inner);
                 },
                 .place = {.x = margin, .y = list_y}});
  }

  ui.keyboard([](const vw::KeyEvent& key) {
    if (!key.ctrl) return false;
    if (key.name == "1") state.filter = Filter::all;
    else if (key.name == "2") state.filter = Filter::active;
    else if (key.name == "3") state.filter = Filter::done;
    else return false;
    return true;
  });
}

}  // namespace

int main() {
  load();
  vw::App app(build);
  return app.run();
}
