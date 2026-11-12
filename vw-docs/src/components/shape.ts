// Material 3 Expressive-style shapes as SVG paths: a circle with `lobes`
// soft bumps (a "cookie" / "scallop"), sampled smoothly. Used as accents.
export function lobed(lobes: number, depth = 0.08, size = 100, rotation = 0): string {
  const points: string[] = [];
  const steps = Math.max(96, lobes * 16);
  const radius = size / 2 / (1 + depth);
  for (let i = 0; i <= steps; i++) {
    const t = (i / steps) * Math.PI * 2;
    const r = radius * (1 + depth * Math.cos(lobes * t));
    const x = size / 2 + r * Math.cos(t + rotation);
    const y = size / 2 + r * Math.sin(t + rotation);
    points.push(`${i === 0 ? 'M' : 'L'}${x.toFixed(2)} ${y.toFixed(2)}`);
  }
  return `${points.join('')}Z`;
}
