export type Page = { label: string; slug: string };
export type Section = { label: string; icon: string; items: Page[] };

export const navigation: Section[] = [
  {label: 'Start', icon: 'start', items: [
    {label: 'What vw is', slug: 'start/introduction'},
    {label: 'Install and build', slug: 'start/installation'},
    {label: 'Your first surface', slug: 'start/first-surface'},
  ]},
  {label: 'Examples', icon: 'bolt', items: [
    {label: 'All examples', slug: 'examples'},
  ]},
  {label: 'Guides', icon: 'guides', items: [
    {label: 'Shells and layer surfaces', slug: 'guides/shells'},
    {label: 'Application windows', slug: 'guides/application'},
    {label: 'Motion and gestures', slug: 'guides/motion'},
    {label: 'Clipping, fading, masks', slug: 'guides/composition'},
    {label: 'Services and events', slug: 'guides/services'},
    {label: 'Custom shaders', slug: 'guides/shaders'},
    {label: 'Lock screens', slug: 'guides/locking'},
  ]},
  {label: 'C++ reference', icon: 'reference', items: [
    {label: 'App and the loop', slug: 'reference/app'},
    {label: 'Drawing widgets', slug: 'reference/widgets'},
    {label: 'Layout', slug: 'reference/layout'},
    {label: 'Input', slug: 'reference/input'},
    {label: 'Surfaces and roles', slug: 'reference/surfaces'},
    {label: 'Animation', slug: 'reference/animation'},
  ]},
  {label: 'Services', icon: 'services', items: [
    {label: 'Notifications', slug: 'services/notifications'},
    {label: 'Media and audio', slug: 'services/media'},
    {label: 'Network, Bluetooth, power', slug: 'services/system'},
    {label: 'Apps and tray', slug: 'services/desktop'},
    {label: 'Compositors', slug: 'services/compositors'},
    {label: 'Processes and IPC', slug: 'services/processes'},
  ]},
  {label: 'Lua', icon: 'lua', items: [
    {label: 'Lua configs', slug: 'lua/overview'},
    {label: 'Module map', slug: 'lua/modules'},
  ]},
  {label: 'Help', icon: 'help', items: [
    {label: 'Troubleshooting', slug: 'help/troubleshooting'},
    {label: 'Contributing', slug: 'help/contributing'},
  ]},
];

export const allPages = navigation.flatMap((section) => section.items);

export function pageUrl(slug: string) {
  const base = import.meta.env.BASE_URL.endsWith('/') ? import.meta.env.BASE_URL : `${import.meta.env.BASE_URL}/`;
  return slug ? `${base}${slug}/` : base;
}

export function sectionOf(slug: string) {
  return navigation.find((section) => section.items.some((page) => page.slug === slug));
}
