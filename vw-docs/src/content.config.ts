import { defineCollection } from 'astro:content';
import { glob } from 'astro/loaders';
import { z } from 'astro/zod';

const schema = z.object({
  title: z.string(),
  description: z.string().default(''),
});

export const collections = {
  docs: defineCollection({ loader: glob({ pattern: '**/*.{md,mdx}', base: './src/content/docs' }), schema }),
  // Russian translations, under the same ids as docs.
  ru: defineCollection({ loader: glob({ pattern: '**/*.{md,mdx}', base: './src/content/ru' }), schema }),
};
