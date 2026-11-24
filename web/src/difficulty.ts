/* A member's chart: d 0..3 = EASY NORMAL HARD EXTREME, e 1 = the EXTRA
 * edition (EXTREME only). d < 0 = the console didn't report one (older
 * mod build). Mirrors server/lobby.py _diff_of. */
const NAMES = ["EASY", "NORMAL", "HARD", "EXTREME"] as const;
const SHORT = ["E", "N", "H", "EX"] as const;
const CLASSES = ["easy", "normal", "hard", "extreme"] as const;

const known = (d: number | undefined | null): d is number =>
  typeof d === "number" && Number.isInteger(d) && d >= 0 && d <= 3;

const extra = (d: number, e: number | undefined | null): boolean =>
  d === 3 && e === 1;

/* "EXTRA EXTREME" etc.; null when unknown */
export function diffLabel(
  d: number | undefined | null,
  e?: number | null,
): string | null {
  if (!known(d)) return null;
  return extra(d, e) ? "EXTRA EXTREME" : NAMES[d];
}

/* compact form for dense rows: E / N / H / EX / EXEX */
export function diffShort(
  d: number | undefined | null,
  e?: number | null,
): string | null {
  if (!known(d)) return null;
  return extra(d, e) ? "EXEX" : SHORT[d];
}

/* css modifier: easy | normal | hard | extreme | exextreme */
export function diffClass(
  d: number | undefined | null,
  e?: number | null,
): string | null {
  if (!known(d)) return null;
  return extra(d, e) ? "exextreme" : CLASSES[d];
}

/* true when at least two players with a known chart played different ones
 * (worth calling out on a results screen) */
export function mixedDifficulty(
  rows: { diff?: number | null; ed?: number | null }[],
): boolean {
  const seen = new Set(
    rows.map((r) => diffClass(r.diff, r.ed)).filter((c) => c !== null),
  );
  return seen.size > 1;
}

export function platLabel(p: string | undefined | null): string | null {
  if (p === "pc") return "PC · Mega Mix+";
  if (p === "switch") return "Switch · Mega Mix";
  return null;
}
