// QuickJS runs this once; draw() is called on compositor presentation frames.
let frames = 0;

function draw(ctx, canvas, time) {
  const w = canvas.width, h = canvas.height;
  ctx.clearRect(0, 0, w, h);
  ctx.fillStyle = '#101a24';
  ctx.fillRect(0, 0, w, h);

  ctx.strokeStyle = 'rgba(150, 200, 215, 0.10)';
  ctx.lineWidth = 1;
  ctx.beginPath();
  for (let x = 0; x < w; x += 32) { ctx.moveTo(x, 0); ctx.lineTo(x, h); }
  for (let y = 0; y < h; y += 32) { ctx.moveTo(0, y); ctx.lineTo(w, y); }
  ctx.stroke();

  ctx.save();
  ctx.translate(w / 2, h / 2 + 10);
  ctx.rotate(time * 0.22);
  const orbit = Math.min(w, h) * 0.28;
  ctx.strokeStyle = '#294655';
  ctx.lineWidth = 2;
  ctx.beginPath(); ctx.arc(0, 0, orbit, 0, Math.PI * 2); ctx.stroke();
  const colors = ['#83dec9', '#9ccaff', '#efad91', '#d4c3f5'];
  for (let i = 0; i < colors.length; ++i) {
    const angle = time * (0.4 + i * 0.08) + i * Math.PI / 2;
    const radius = 14 + 6 * Math.sin(time * 1.5 + i);
    ctx.fillStyle = colors[i];
    ctx.beginPath();
    ctx.arc(Math.cos(angle) * orbit, Math.sin(angle) * orbit, radius, 0, Math.PI * 2);
    ctx.fill();
  }
  ctx.fillStyle = '#83dec9';
  ctx.beginPath();
  ctx.moveTo(-18, 0);
  ctx.bezierCurveTo(-18, -26, 18, -26, 18, 0);
  ctx.bezierCurveTo(18, 26, -18, 26, -18, 0);
  ctx.fill();
  ctx.restore();

  ctx.fillStyle = '#eaf1f6';
  ctx.font = 'bold 21px sans-serif';
  ctx.textBaseline = 'top';
  ctx.fillText('JavaScript → Canvas 2D → Vulkan', 22, 20);
  ctx.fillStyle = '#93a9b9';
  ctx.font = '12px sans-serif';
  ctx.fillText('Persistent JS state · presentation-driven drawing', 22, 52);
  ctx.fillText('Frames drawn: ' + (++frames), 22, h - 26);
}
