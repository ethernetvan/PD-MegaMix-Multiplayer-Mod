import { useStore } from "../store";
import type { LobbySnap } from "../types";

export function memberName(l: LobbySnap, mid: string | null | undefined) {
  return l.members.find((m) => m.mid === mid)?.name ?? "?";
}

export function PhaseBadge({ phase }: { phase: string }) {
  return <span className={`badge phase-${phase.toLowerCase()}`}>{phase}</span>;
}

function LobbyCard({ l }: { l: LobbySnap }) {
  const { state } = useStore();
  const playing = l.phase !== "SELECT";
  const song = playing
    ? l.round
      ? `${l.round.name} · PV ${l.round.pv}`
      : "—"
    : l.sel
      ? `${l.sel.name} · PV ${l.sel.pv}`
      : "picking…";
  return (
    <a className="card" href={`#/lobby/${l.code}`}>
      <div className="card-top">
        <span className="code">{l.code}</span>
        <PhaseBadge phase={l.phase} />
        {state.you.owns.includes(l.code) && (
          <span className="badge own">You own this</span>
        )}
      </div>
      <div className="song">{song}</div>
      <div className="dim">
        {l.members.length} member{l.members.length === 1 ? "" : "s"} · picker{" "}
        {memberName(l, l.leader)}
        {l.round ? ` · round ${l.round.id}` : ""}
      </div>
    </a>
  );
}

export default function Overview() {
  const { state } = useStore();
  const snap = state.snapshot;
  if (!snap) return <main className="msg">Connecting…</main>;
  return (
    <main className="overview">
      <section className="lobby-list">
        {snap.lobbies.length === 0 && (
          <p className="empty">
            No lobbies yet — create one from the Multiplayer button in song
            select.
          </p>
        )}
        {snap.lobbies.map((l) => (
          <LobbyCard key={l.code} l={l} />
        ))}
      </section>
      <aside className="panel">
        <h3>Online consoles</h3>
        <ul className="console-list">
          {snap.consoles.length === 0 && <li className="dim">none connected</li>}
          {snap.consoles.map((c) => (
            <li key={c.mid}>
              <b>{c.name}</b>{" "}
              <span className="dim">
                {c.lobby ? `in lobby ${c.lobby}` : "not in a lobby"}
              </span>
            </li>
          ))}
        </ul>
      </aside>
    </main>
  );
}
