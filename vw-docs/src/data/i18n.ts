// The site in English (at /) and Russian (at /ru/). Pages live in
// content/docs and content/ru under the same ids; a page without a Russian
// version shows the English one under /ru/ with a note.
export type Locale = 'en' | 'ru';
export const locales: Locale[] = ['en', 'ru'];

export const localeNames: Record<Locale, string> = { en: 'English', ru: 'Русский' };

const strings = {
  en: {
    skip: 'Skip to content',
    home: 'vw home',
    homeTitle: 'vw — widgets for Wayland, drawn on the GPU',
    tagline: 'Wayland widgets on Vulkan',
    documentation: 'Documentation',
    sets: 'Documentation sets',
    search: 'Search',
    searchLabel: 'Search the documentation',
    searchPlaceholder: 'Search widgets, services, guides…',
    closeSearch: 'Close search',
    matches: (count: number) => `${count} match${count === 1 ? '' : 'es'}`,
    startHere: 'Good places to start',
    searchFailed: 'Search could not load.',
    pages: (tab: string) => `${tab} pages`,
    theme: 'Switch light or dark',
    closeMenu: 'Close menu',
    previousNext: 'Previous and next',
    previous: 'Previous',
    next: 'Next',
    onThisPage: 'On this page',
    quickActions: 'Quick actions',
    index: 'Index',
    language: 'Русский',
    languageLabel: 'Читать на русском',
    untranslated: 'This page has no translation yet.',
    notFoundTitle: 'Nothing here',
    notFoundText: 'That page moved or never existed. The index and search know where everything is.',
    notFoundBack: 'Back to the start',
    docs: 'Docs',
  },
  ru: {
    skip: 'К содержимому',
    home: 'vw: главная',
    homeTitle: 'vw — виджеты для Wayland, нарисованные на GPU',
    tagline: 'Виджеты Wayland на Vulkan',
    documentation: 'Документация',
    sets: 'Разделы документации',
    search: 'Поиск',
    searchLabel: 'Поиск по документации',
    searchPlaceholder: 'Виджеты, сервисы, руководства…',
    closeSearch: 'Закрыть поиск',
    matches: (count: number) => {
      const tens = count % 100;
      const ones = count % 10;
      const word = tens >= 11 && tens <= 14 ? 'совпадений' : ones === 1 ? 'совпадение' : ones >= 2 && ones <= 4 ? 'совпадения' : 'совпадений';
      return `${count} ${word}`;
    },
    startHere: 'С чего начать',
    searchFailed: 'Поиск не загрузился.',
    pages: (tab: string) => `Страницы: ${tab}`,
    theme: 'Светлая или тёмная тема',
    closeMenu: 'Закрыть меню',
    previousNext: 'Предыдущая и следующая',
    previous: 'Назад',
    next: 'Далее',
    onThisPage: 'На этой странице',
    quickActions: 'Быстрые действия',
    index: 'Оглавление',
    language: 'English',
    languageLabel: 'Read in English',
    untranslated: 'Эта страница ещё не переведена — ниже английский оригинал.',
    notFoundTitle: 'Здесь ничего нет',
    notFoundText: 'Страница переехала или её никогда не было. Оглавление и поиск знают, где что лежит.',
    notFoundBack: 'На главную',
    docs: 'Документация',
  },
} as const;

export function t(locale: Locale) {
  return strings[locale];
}

// Sidebar, tab and folder labels. Names of things (vw, zwwm, Facet, Lua…)
// stay as they are.
const labels: Record<string, string> = {
  // tabs and sections
  Compositor: 'Композитор',
  Start: 'Начало',
  Examples: 'Примеры',
  Guides: 'Руководства',
  'C++ reference': 'Справочник C++',
  Services: 'Сервисы',
  Help: 'Помощь',
  Compositors: 'Композиторы',
  Capabilities: 'Возможности',
  Language: 'Язык',
  Reference: 'Справочник',
  Integration: 'Интеграция',
  // folders
  Surfaces: 'Поверхности',
  Drawing: 'Рисование',
  Behaviour: 'Поведение',
  Core: 'Основа',
  Widgets: 'Виджеты',
  Interaction: 'Взаимодействие',
  Desktop: 'Рабочий стол',
  System: 'Система',
  Basics: 'Основы',
  'Layout and motion': 'Раскладка и движение',
  // pages
  'What vw is': 'Что такое vw',
  'Install and build': 'Установка и сборка',
  'Your first surface': 'Первая поверхность',
  'All examples': 'Все примеры',
  'Shells and layer surfaces': 'Оболочки и layer-поверхности',
  'Application windows': 'Окна приложений',
  'Lock screens': 'Экраны блокировки',
  Styling: 'Оформление',
  'Clipping, fading, masks': 'Обрезка, затухание, маски',
  'Custom shaders': 'Свои шейдеры',
  'Motion and gestures': 'Движение и жесты',
  'Services and events': 'Сервисы и события',
  'App and the loop': 'App и цикл',
  'Surfaces and roles': 'Поверхности и роли',
  'Drawing widgets': 'Виджеты рисования',
  Layout: 'Раскладка',
  Input: 'Ввод',
  Animation: 'Анимация',
  Notifications: 'Уведомления',
  'Apps and tray': 'Приложения и трей',
  'Media and audio': 'Медиа и звук',
  'Network, Bluetooth, power': 'Сеть, Bluetooth, питание',
  'Processes and IPC': 'Процессы и IPC',
  'Login (greetd)': 'Вход (greetd)',
  'Lua configs': 'Конфиги на Lua',
  'Module map': 'Карта модулей',
  Troubleshooting: 'Решение проблем',
  Contributing: 'Участие в разработке',
  'Compositors and protocols': 'Композиторы и протоколы',
  'Tags, snapshots, actions': 'Теги, снимки, действия',
  'Edge swipes': 'Свайпы от края',
  'Focus grabs and decorations': 'Захват фокуса и декорации',
  'Screen and window capture': 'Захват экрана и окон',
  'What Facet is': 'Что такое Facet',
  'Build and tooling': 'Сборка и инструменты',
  'Views and imports': 'View и импорты',
  'Expressions and handlers': 'Выражения и обработчики',
  'Loops, conditions, let': 'Циклы, условия, let',
  'Layout and sizing': 'Раскладка и размеры',
  Elements: 'Элементы',
  Controls: 'Элементы управления',
  'C++ in Facet': 'C++ в Facet',
};

export function label(text: string, locale: Locale) {
  return locale === 'ru' ? labels[text] ?? text : text;
}

// The locale a URL path belongs to.
export function localeOf(path: string): Locale {
  const base = import.meta.env.BASE_URL.replace(/\/$/, '');
  const rest = path.startsWith(base) ? path.slice(base.length) : path;
  return rest === '/ru' || rest.startsWith('/ru/') ? 'ru' : 'en';
}
