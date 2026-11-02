/* Ranks for an already-sorted row list where tied rows share the LOWEST
   position of their group (1-3-3-4: two players tied for first are both
   2nd). Rows tie when their key is equal: [Math.round(pct * 100) / 100,
   score] (matches the server's _competition_ranks in lobby.py). */

export interface Ranked {
  pct: number;
  score: number;
}

const key = (r: Ranked) => `${Math.round(r.pct * 100) / 100}|${r.score}`;

export function competitionRanks<T extends Ranked>(rows: T[]): number[] {
  const out = new Array<number>(rows.length);
  let i = 0;
  while (i < rows.length) {
    let j = i;
    while (j + 1 < rows.length && key(rows[j + 1]) === key(rows[i])) j++;
    for (let k = i; k <= j; k++) out[k] = j + 1;
    i = j + 1;
  }
  return out;
}
