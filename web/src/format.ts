export const fmtScore = (n: number): string =>
  Math.round(n).toLocaleString("en-US");

export const fmtPct = (p: number): string => `${p.toFixed(2)}%`;

export function fmtRel(ts: number, now: number): string {
  const d = Math.max(0, now - ts);
  if (d < 5) return "just now";
  if (d < 60) return `${Math.floor(d)}s ago`;
  if (d < 3600) return `${Math.floor(d / 60)}m ago`;
  if (d < 86400) return `${Math.floor(d / 3600)}h ago`;
  return `${Math.floor(d / 86400)}d ago`;
}

export function fmtElapsed(
  start: number | null | undefined,
  now: number,
): string {
  if (start == null) return "—";
  const s = Math.max(0, Math.floor(now - start));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const ss = s % 60;
  const mm = String(m).padStart(2, "0");
  const sss = String(ss).padStart(2, "0");
  return h > 0 ? `${h}:${mm}:${sss}` : `${m}:${sss}`;
}
