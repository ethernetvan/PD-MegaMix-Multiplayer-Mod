export function autoLabel(pct?: number | null): string {
  return pct == null || pct >= 100 ? "AUTO" : `AUTO ${Math.round(pct)}%`;
}
