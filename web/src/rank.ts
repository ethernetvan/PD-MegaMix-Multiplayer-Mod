/* 1-2-2-4 competition ranking on an already-sorted row list.
   Rows tie when their key is equal: [Math.round(pct * 100) / 100, score]
   (matches the server's _competition_ranks in lobby.py). */

export interface Ranked {
  pct: number;
  score: number;
}

export function competitionRanks<T extends Ranked>(rows: T[]): number[] {
  let rank = 0;
  let prevKey: string | null = null;
  return rows.map((r, i) => {
    const key = `${Math.round(r.pct * 100) / 100}|${r.score}`;
    if (key !== prevKey) {
      rank = i + 1;
      prevKey = key;
    }
    return rank;
  });
}
