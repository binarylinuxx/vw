import { getCollection } from 'astro:content';
import { allPages, sectionOf } from '../data/navigation';

export const prerender = true;
export async function GET() {
  const docs = await getCollection('docs');
  const entries = docs
    .map((entry) => ({
      title: entry.data.title,
      description: entry.data.description,
      slug: entry.id,
      section: sectionOf(entry.id)?.label ?? 'Docs',
      body: (entry.body || '').replace(/^import .*$/gm, '').replace(/<[^>]+>/g, ' ').replace(/\s+/g, ' ').slice(0, 20000),
    }))
    .sort((a, b) => allPages.findIndex((p) => p.slug === a.slug) - allPages.findIndex((p) => p.slug === b.slug));
  return Response.json(entries);
}
