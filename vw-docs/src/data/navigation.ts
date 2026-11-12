// The site is three tabs, each with its own sidebar: sections hold pages and
// groups, and groups nest (a group is a folder of pages and further groups).
export type Page = { label: string; slug: string };
export type Group = { label: string; items: Entry[] };
export type Entry = Page | Group;
export type Section = { label: string; icon: string; items: Entry[] };
export type Tab = { id: string; label: string; icon: string; home: string; sections: Section[] };

export const isGroup = (entry: Entry): entry is Group => 'items' in entry;

export const tabs: Tab[] = [
  {id: 'vw', label: 'vw', icon: 'widgets', home: '', sections: [
    {label: 'Start', icon: 'start', items: [
      {label: 'What vw is', slug: 'start/introduction'},
      {label: 'Install and build', slug: 'start/installation'},
      {label: 'Your first surface', slug: 'start/first-surface'},
    ]},
    {label: 'Examples', icon: 'bolt', items: [
      {label: 'All examples', slug: 'examples'},
    ]},
    {label: 'Guides', icon: 'guides', items: [
      {label: 'Surfaces', items: [
        {label: 'Shells and layer surfaces', slug: 'guides/shells'},
        {label: 'Application windows', slug: 'guides/application'},
        {label: 'Lock screens', slug: 'guides/locking'},
      ]},
      {label: 'Drawing', items: [
        {label: 'Styling', slug: 'guides/styling'},
        {label: 'Clipping, fading, masks', slug: 'guides/composition'},
        {label: 'Custom shaders', slug: 'guides/shaders'},
      ]},
      {label: 'Behaviour', items: [
        {label: 'Motion and gestures', slug: 'guides/motion'},
        {label: 'Services and events', slug: 'guides/services'},
      ]},
    ]},
    {label: 'C++ reference', icon: 'reference', items: [
      {label: 'Core', items: [
        {label: 'App and the loop', slug: 'reference/app'},
        {label: 'Surfaces and roles', slug: 'reference/surfaces'},
      ]},
      {label: 'Widgets', items: [
        {label: 'Drawing widgets', slug: 'reference/widgets'},
        {label: 'Layout', slug: 'reference/layout'},
      ]},
      {label: 'Interaction', items: [
        {label: 'Input', slug: 'reference/input'},
        {label: 'Animation', slug: 'reference/animation'},
      ]},
    ]},
    {label: 'Services', icon: 'services', items: [
      {label: 'Desktop', items: [
        {label: 'Notifications', slug: 'services/notifications'},
        {label: 'Apps and tray', slug: 'services/desktop'},
        {label: 'Media and audio', slug: 'services/media'},
      ]},
      {label: 'System', items: [
        {label: 'Network, Bluetooth, power', slug: 'services/system'},
        {label: 'Processes and IPC', slug: 'services/processes'},
        {label: 'Login (greetd)', slug: 'services/greetd'},
      ]},
    ]},
    {label: 'Lua', icon: 'lua', items: [
      {label: 'Lua configs', slug: 'lua/overview'},
      {label: 'Module map', slug: 'lua/modules'},
    ]},
    {label: 'Help', icon: 'help', items: [
      {label: 'Troubleshooting', slug: 'help/troubleshooting'},
      {label: 'Contributing', slug: 'help/contributing'},
    ]},
  ]},
  {id: 'compositor', label: 'Compositor', icon: 'layers', home: 'compositor/overview', sections: [
    {label: 'Start', icon: 'start', items: [
      {label: 'Compositors and protocols', slug: 'compositor/overview'},
    ]},
    {label: 'Compositors', icon: 'layers', items: [
      {label: 'zwwm', items: [
        {label: 'Tags, snapshots, actions', slug: 'compositor/zwwm'},
        {label: 'Edge swipes', slug: 'compositor/edge-swipes'},
        {label: 'Focus grabs and decorations', slug: 'compositor/zwwm-surfaces'},
      ]},
      {label: 'Hyprland', slug: 'compositor/hyprland'},
    ]},
    {label: 'Capabilities', icon: 'bolt', items: [
      {label: 'Screen and window capture', slug: 'compositor/capture'},
    ]},
  ]},
  {id: 'facet', label: 'Facet', icon: 'code', home: 'facet/introduction', sections: [
    {label: 'Start', icon: 'start', items: [
      {label: 'What Facet is', slug: 'facet/introduction'},
      {label: 'Build and tooling', slug: 'facet/setup'},
    ]},
    {label: 'Language', icon: 'guides', items: [
      {label: 'Basics', items: [
        {label: 'Views and imports', slug: 'facet/views'},
        {label: 'Expressions and handlers', slug: 'facet/expressions'},
        {label: 'Loops, conditions, let', slug: 'facet/flow'},
      ]},
      {label: 'Layout and motion', items: [
        {label: 'Layout and sizing', slug: 'facet/layout'},
        {label: 'Animation', slug: 'facet/animation'},
      ]},
    ]},
    {label: 'Reference', icon: 'reference', items: [
      {label: 'Elements', slug: 'facet/elements'},
      {label: 'Controls', slug: 'facet/controls'},
    ]},
    {label: 'Integration', icon: 'services', items: [
      {label: 'C++ in Facet', slug: 'facet/cpp'},
    ]},
  ]},
];

export function pagesIn(entries: Entry[]): Page[] {
  return entries.flatMap((entry) => (isGroup(entry) ? pagesIn(entry.items) : [entry]));
}

export const pagesOf = (tab: Tab) => tab.sections.flatMap((section) => pagesIn(section.items));
export const allPages = tabs.flatMap(pagesOf);

// A page's URL; Russian pages live under ru/.
export function pageUrl(slug: string, locale: 'en' | 'ru' = 'en') {
  const base = import.meta.env.BASE_URL.endsWith('/') ? import.meta.env.BASE_URL : `${import.meta.env.BASE_URL}/`;
  const root = locale === 'ru' ? `${base}ru/` : base;
  return slug ? `${root}${slug}/` : root;
}

// The tab a page belongs to; the home page belongs to vw.
export function tabOf(slug: string): Tab {
  return tabs.find((tab) => pagesOf(tab).some((page) => page.slug === slug)) ?? tabs[0];
}

export function sectionOf(slug: string) {
  for (const tab of tabs)
    for (const section of tab.sections)
      if (pagesIn(section.items).some((page) => page.slug === slug)) return section;
  return undefined;
}

// Labels from the section down to the page's folder: ['Guides', 'Surfaces'].
export function trailOf(slug: string): string[] {
  const walk = (entries: Entry[], path: string[]): string[] | undefined => {
    for (const entry of entries) {
      if (isGroup(entry)) {
        const found = walk(entry.items, [...path, entry.label]);
        if (found) return found;
      } else if (entry.slug === slug) {
        return path;
      }
    }
    return undefined;
  };
  for (const section of tabOf(slug).sections) {
    const found = walk(section.items, [section.label]);
    if (found) return found;
  }
  return [];
}
