export async function update(ctx) {
  const cpu = ctx.systemMetrics.cpu || {};
  const usage = Number(cpu.totalPercent);

  return {
    usage: Number.isFinite(usage) ? Math.max(0, Math.min(100, usage)) : 0
  };
}
