// Builds inbox.facet in memory, clicks the first message through its C++
// handler, and builds again: the unread count must drop.
#include <algorithm>
#include <cstdio>
#include <vector>

#include "inbox.facet.hpp"

struct State { std::vector<Message> messages; };
struct Actions { int opened = 0; };

int main() {
  State state{{{"Ann", "Lunch?"}, {"Bob", "Build is green"}, {"Cy", "Notes", false}}};
  Actions actions;
  const auto unread_text = [&] {
    vw::Scene scene;
    vw::Ui ui(scene);
    views::Inbox(ui, state, actions);
    ui.finish();
    return scene;
  };
  vw::Scene before = unread_text();
  std::printf("before: %zu interactions\n", before.interactions.size());
  for (const auto& interaction : before.interactions)
    if (interaction.on_click) { interaction.on_click({}); break; }
  const int unread = static_cast<int>(std::count_if(state.messages.begin(), state.messages.end(),
                                                    [](const Message& m) { return m.unread; }));
  std::printf("after click: actions.opened = %d, unread = %d (was 2)\n", actions.opened, unread);
  return actions.opened == 1 && unread == 1 ? 0 : 1;
}
