import { useStore } from "../store";
import type { LobbySnap } from "../types";
import HexAvatar from "./HexAvatar";

export function memberName(l: LobbySnap, mid: string | null | undefined) {
  return l.members.find((m) => m.mid === mid)?.name ?? "?";
}

export function PhaseBadge({ phase }: { phase: string }) {
  return <span className={`pchip phase-${phase.toLowerCase()}`}>{phase}</span>;
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
  const shown = l.members.slice(0, 3);
  return (
    <a className="card" href={`#/lobby/${l.code}`}>
      <span className="card-jacket">
        {shown.length === 0 ? (
          <span className="card-jacket-empty" aria-hidden>
            ♪
          </span>
        ) : (
          <span className="card-hexes">
            {shown.map((m) => (
              <HexAvatar key={m.mid} name={m.name} size={30} />
            ))}
            {l.members.length > shown.length && (
              <span className="card-hexes-more">+{l.members.length - 3}</span>
            )}
          </span>
        )}
      </span>
      <span className="card-body">
        <span className="card-top">
          <span className="code">{l.code}</span>
          <PhaseBadge phase={l.phase} />
          {state.you.owns.includes(l.code) && (
            <span className="badge own">You own this</span>
          )}
        </span>
        <span className="song">{song}</span>
        <span className="card-sub">
          {l.members.length} member{l.members.length === 1 ? "" : "s"} · picker{" "}
          {memberName(l, l.leader)}
          {l.round ? ` · round ${l.round.id}` : ""}
        </span>
      </span>
    </a>
  );
}

export default function Overview() {
  const { state } = useStore();
  const snap = state.snapshot;
  if (!snap) return <main className="msg">Connecting…</main>;
  return (
    <main className="overview">
      <div className="ribbon-corner" aria-hidden>
        Spectate
      </div>
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
              <HexAvatar name={c.name} size={30} status="online" />
              <span className="console-name">
                <b>{c.name}</b>{" "}
                <span className="dim">
                  {c.lobby ? `in lobby ${c.lobby}` : "not in a lobby"}
                </span>
              </span>
            </li>
          ))}
        </ul>
      </aside>
    </main>
  );
}
