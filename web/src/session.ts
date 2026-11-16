import type { MemberSnap, StandingRow } from "./types";

export type ClearStatus = "clear" | "fail" | "dnf" | "unknown";

/* STAGE CLEAR / NOT CLEAR / DROP×OUT for a results row. The verdict comes
   from the console (it knows its song's border and the final life);
   without one (old client) the console's reported border decides, and with
   no border either there is nothing to show. */
export function clearStatus(
  r: Pick<StandingRow, "pct" | "border" | "clear" | "drop">,
  hasFinal: boolean,
): ClearStatus {
  if (!hasFinal || r.drop) return "dnf";
  if (r.clear === true) return "clear";
  if (r.clear === false) return "fail";
  if (r.border > 0) return r.pct >= r.border ? "clear" : "fail";
  return "unknown";
}

/* members in room session order (server position, join order on ties) */
export function sessionOrder(members: MemberSnap[]): MemberSnap[] {
  return members
    .map((m, i) => ({ m, i }))
    .sort(
      (a, b) =>
        (a.m.session?.pos || 99) - (b.m.session?.pos || 99) || a.i - b.i,
    )
    .map((x) => x.m);
}

/* true once a round has counted for anyone in the room */
export function hasSessionResults(members: MemberSnap[]): boolean {
  return members.some(
    (m) => !!m.session && (m.session.score > 0 || m.session.places.some((p) => p > 0)),
  );
}
