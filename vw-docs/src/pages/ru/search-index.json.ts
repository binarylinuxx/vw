import { getCollection } from 'astro:content';
import { allPages, tabOf, trailOf } from '../../data/navigation';
import { label } from '../../data/i18n';

export const prerender = true;
// The Russian pages, with English ones standing in for the untranslated.
export async function GET() {
  const russian = new Map((await getCollection('ru')).map((entry) => [entry.id, entry]));
  const docs = (await getCollection('docs')).map((entry) => russian.get(entry.id) ?? entry);
  const entries = docs
    .map((entry) => ({
      title: entry.data.title,
      description: entry.data.description,
      slug: entry.id,
      section: [tabOf(entry.id).label, ...trailOf(entry.id)].map((item) => label(item, 'ru')).join(' · ') || 'Документация',
      body: (entry.body || '').replace(/^import .*$/gm, '').replace(/<[^>]+>/g, ' ').replace(/\s+/g, ' ').slice(0, 20000),
    }))
    .sort((a, b) => allPages.findIndex((p) => p.slug === a.slug) - allPages.findIndex((p) => p.slug === b.slug));
  return Response.json(entries);
}
