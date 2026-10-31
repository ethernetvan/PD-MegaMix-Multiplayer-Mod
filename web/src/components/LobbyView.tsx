import { useEffect, useReducer, useState } from "react";
import { feedText } from "../feedText";
import { fmtElapsed, fmtPct, fmtRel, fmtScore } from "../format";
import { canAdmin, serverNow } from "../state";
import { useStore } from "../store";
import type { LobbySnap, MemberSnap, PickMode } from "../types";
import HexAvatar from "./HexAvatar";
import { memberName, PhaseBadge } from "./Overview";

const PICK_LABEL: Record<PickMode, string> = {
  rotate: "Rotate",
  owner: "Owner picks",
  assigned: "Assigned",
};

function memberOf(l: LobbySnap, mid: string | null | undefined) {
  return l.members.find((m) => m.mid === mid);
}

function PhasePanel({ l, now }: { l: LobbySnap; now: number }) {
  if (l.phase === "SELECT") {
    const notReady = l.members.filter((m) => m.connected && !m.ready);
    return (
      <section className="panel status">
        <h3>Song select</h3>
        <p className="status-line">
          <b>{memberName(l, l.leader)}</b> is picking a song…
        </p>
        {l.sel && (
          <p className="sel-song">
            <span className="dim">Selected</span>
            <span className="sel-name">♪ {l.sel.name}</span>
            <span className="pv-chip">PV {l.sel.pv}</span>
          </p>
        )}
        {notReady.length > 0 && (
          <ul className="notready">
            {notReady.map((m) => (
              <li key={m.mid}>
                {m.name} — {m.reason || "not ready"}
              </li>
            ))}
          </ul>
        )}
      </section>
    );
  }
  if (l.phase === "LOADING") {
    return (
      <section className="panel">
        <h3>Loading…</h3>
        <ul className="checklist">
          {(l.round?.players ?? []).map((mid) => {
            const m = memberOf(l, mid);
            if (!m) return null;
            return (
              <li key={mid} className={m.loaded ? "ok" : ""}>
                <HexAvatar name={m.name} size={30} />
                <span className="chk-name">{m.name}</span>
                <span className="chk-mark">{m.loaded ? "✓" : "…"}</span>
              </li>
            );
          })}
        </ul>
      </section>
    );
  }
  if (l.phase === "PLAYING") {
    const rows = (l.round?.players ?? [])
      .map((mid) => memberOf(l, mid))
      .filter((m): m is MemberSnap => !!m)
      .sort((a, b) => b.score - a.score);
    return (
      <section className="panel">
        <h3>
          Live leaderboard{" "}
          <span className="dim">
            elapsed {fmtElapsed(l.round?.launch_at, now)}
          </span>
        </h3>
        <div className="board-wrap">
          <table className="board">
            <thead>
              <tr>
                <th>#</th>
                <th>Player</th>
                <th>Score</th>
                <th>%</th>
                <th>Combo</th>
                <th>Life</th>
              </tr>
            </thead>
            <tbody>
              {rows.map((m, i) => (
                <tr key={m.mid} className={m.connected ? "" : "off"}>
                  <td className="rank">{i + 1}</td>
                  <td>
                    <span className="p-cell">
                      <HexAvatar
                        name={m.name}
                        size={32}
                        status={m.connected ? "online" : "offline"}
                      />
                      <span>
                        {m.name}
                        {m.autoplay && (
                          <span className="badge auto">AUTO</span>
                        )}
                        {m.final && (
                          <span className="badge done">✓ finished</span>
                        )}
                      </span>
                    </span>
                  </td>
                  <td className="num score">{fmtScore(m.score)}</td>
                  <td className="pct">
                    <div className="bar gauge">
                      <i
                        style={{
                          width: `${100 - Math.min(100, m.pct)}%`,
                        }}
                      />
                      <span className="gauge-clear" aria-hidden />
                      <span className="gauge-border" aria-hidden />
                    </div>
                    <span className="pct-num">{fmtPct(m.pct)}</span>
                  </td>
                  <td className="num">{m.combo}x</td>
                  <td className="life-cell">
                    <div className="bar life">
                      <i style={{ width: `${(m.life / 255) * 100}%` }} />
                    </div>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </section>
    );
  }
  const st = l.standings;
  if (!st) return null;
  return (
    <section className="panel">
      <h3>
        Results{" "}
        {!st.complete && <span className="dim">(waiting for players…)</span>}
      </h3>
      <div className="board-wrap">
        <table className="board results">
          <thead>
            <tr>
              <th>#</th>
              <th>Player</th>
              <th>Score</th>
              <th>%</th>
              <th>Max combo</th>
              <th>Status</th>
            </tr>
          </thead>
          <tbody>
            {st.e.map((r) => (
              <tr key={r.mid} className={r.connected ? "" : "off"}>
                <td>
                  <span className={`rank-num r${r.rank}`}>{r.rank}</span>
                </td>
                <td>
                  <span className="p-cell">
                    <HexAvatar
                      name={r.name}
                      size={32}
                      status={r.connected ? "online" : "offline"}
                    />
                    <span>
                      {r.name}
                      {r.autoplay && <span className="badge auto">AUTO</span>}
                    </span>
                  </span>
                </td>
                <td className="num score">{fmtScore(r.score)}</td>
                <td className="num">{fmtPct(r.pct)}</td>
                <td className="num">{r.max_combo}x</td>
                <td>
                  <span className={`badge ${r.final ? "done" : "warn"}`}>
                    {r.final ? "final" : "pending"}
                  </span>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </section>
  );
}

function Members({ l }: { l: LobbySnap }) {
  const { state } = useStore();
  return (
    <section className="panel">
      <h3>Members</h3>
      <ul className="members">
        {l.members.map((m, i) => (
          <li key={m.mid} className={m.connected ? "" : "off"}>
            <span className="mem-hex">
              <HexAvatar
                name={m.name}
                size={56}
                status={m.connected ? "online" : "offline"}
                ring={
                  m.mid === l.leader ||
                  m.mid === l.owner ||
                  (state.you.name !== null && m.name === state.you.name)
                }
              />
              <span className="mem-num">{i + 1}</span>
            </span>
            <span className="mem-name">{m.name}</span>
            <span className="mem-badges">
              {m.mid === l.owner && <span className="badge">Owner</span>}
              {m.mid === l.leader && <span className="badge pick">Picker</span>}
              {m.role !== "player" && <span className="badge">{m.role}</span>}
              {m.autoplay && <span className="badge auto">AUTO</span>}
              {!m.connected && <span className="badge off">offline</span>}
              {m.connected && !m.ready && (
                <span className="badge warn">{m.reason || "not ready"}</span>
              )}
              {m.back_in_sel && <span className="badge">back in select</span>}
            </span>
          </li>
        ))}
      </ul>
    </section>
  );
}

function ActivityFeed({ l }: { l: LobbySnap }) {
  const { state } = useStore();
  const now = serverNow(state);
  const entries = [...(state.feeds[l.code] ?? [])].reverse();
  return (
    <section className="panel feed">
      <h3>Activity</h3>
      {entries.length === 0 && <p className="dim">No activity yet.</p>}
      <ul>
        {entries.map((e, i) => (
          <li key={i}>
            {feedText(e)} <span className="dim">· {fmtRel(e.ts, now)}</span>
          </li>
        ))}
      </ul>
    </section>
  );
}

function AdminPanel({ l }: { l: LobbySnap }) {
  const { socket, toast, state } = useStore();
  const [confirm, setConfirm] = useState<string | null>(null);
  const [assigning, setAssigning] = useState(false);
  const act = async (msg: Record<string, unknown>) => {
    try {
      const r = await socket.action({ lobby: l.code, ...msg });
      if (!r.ok) toast(`Action failed: ${r.error ?? "unknown"}`);
    } catch {
      toast("Action failed: not connected");
    }
  };
  const modes: PickMode[] = ["rotate", "owner", "assigned"];
  return (
    <section className="panel admin">
      <h3>Host controls</h3>
      <div className="seg">
        {modes.map((m) => (
          <button
            key={m}
            className={`seg-btn${
              l.pick_mode === m || (m === "assigned" && assigning) ? " on" : ""
            }`}
            onClick={() => {
              if (m === "assigned" && l.pick_mode !== "assigned") {
                setAssigning(true); // choose a member first; action on Assign
                return;
              }
              setAssigning(false);
              act({ op: "set_pick_mode", mode: m });
            }}
          >
            {PICK_LABEL[m]}
          </button>
        ))}
      </div>
      {assigning && l.pick_mode !== "assigned" && (
        <p className="dim">Pick a member to assign as the picker</p>
      )}
      {l.pending_pick_mode && (
        <p className="dim">
          {PICK_LABEL[l.pending_pick_mode.mode]}
          {l.pending_pick_mode.assigned
            ? ` (${memberName(l, l.pending_pick_mode.assigned)})`
            : ""}{" "}
          applies after this round
        </p>
      )}
      <ul className="member-admin">
        {l.members.map((m) => (
          <li key={m.mid}>
            <HexAvatar
              name={m.name}
              size={30}
              status={m.connected ? "online" : "offline"}
            />
            <span className={m.connected ? "" : "dim"}>{m.name}</span>
            <span className="spacer" />
            {((l.pick_mode === "assigned" && m.mid !== l.assigned) ||
              (assigning && l.pick_mode !== "assigned")) && (
              <button
                className="btn sm"
                onClick={() => {
                  setAssigning(false);
                  act({ op: "set_pick_mode", mode: "assigned", member: m.mid });
                }}
              >
                Assign
              </button>
            )}
            {l.pick_mode === "rotate" && m.mid !== l.leader && (
              <button
                className="btn sm"
                onClick={() => act({ op: "set_leader", member: m.mid })}
              >
                Make picker
              </button>
            )}
            {(state.you.admin || m.mid !== l.owner) &&
              (confirm === m.mid ? (
                <button
                  className="btn sm danger"
                  onClick={() => {
                    setConfirm(null);
                    act({ op: "kick", member: m.mid });
                  }}
                >
                  Sure?
                </button>
              ) : (
                <button
                  className="btn sm kick"
                  onClick={() => setConfirm(m.mid)}
                >
                  Kick
                </button>
              ))}
          </li>
        ))}
      </ul>
    </section>
  );
}

export default function LobbyView({ code }: { code: string }) {
  const { state } = useStore();
  const [, tick] = useReducer((n: number) => n + 1, 0);
  useEffect(() => {
    const t = setInterval(tick, 1000);
    return () => clearInterval(t);
  }, []);
  const snap = state.snapshot;
  const l = snap?.lobbies.find((x) => x.code === code);
  if (!snap) return <main className="msg">Connecting…</main>;
  if (!l)
    return (
      <main className="msg">
        <p>Lobby closed</p>
        <a href="#/">← Back to overview</a>
      </main>
    );
  const now = serverNow(state);
  const voteLeft = l.vote ? Math.max(0, Math.ceil(l.vote.ends_at - now)) : 0;
  const songInfo = l.round ?? l.sel;
  return (
    <main className="lobby">
      <div className="lobby-head">
        <div className="lobby-head-top">
          <h1>
            <span className="code">{l.code}</span>{" "}
            <PhaseBadge phase={l.phase} />
          </h1>
          <a href="#/" className="btn-back">
            ← lobbies
          </a>
        </div>
        {songInfo && (
          <div className="hud-song">
            ♪ <b>{songInfo.name}</b>{" "}
            <span className="pv-chip">PV {songInfo.pv}</span>
          </div>
        )}
        <div className="meta">
          {l.round && <span>round {l.round.id}</span>}
          <span className="mi">
            owner <HexAvatar name={memberName(l, l.owner)} size={20} />{" "}
            {memberName(l, l.owner)}
          </span>
          <span className="mi">
            picker <HexAvatar name={memberName(l, l.leader)} size={20} />{" "}
            {memberName(l, l.leader)}
          </span>
          <span>{PICK_LABEL[l.pick_mode]}</span>
        </div>
      </div>
      {l.group_paused && <div className="banner">Group paused</div>}
      {l.vote && (
        <div className="banner vote">
          {l.vote.kind === "pause" ? "Pause" : "Resume"} vote:{" "}
          {l.vote.agreed.length}/{l.vote.need} agreed · {voteLeft}s left
        </div>
      )}
      <PhasePanel l={l} now={now} />
      {canAdmin(state.you, l.code) && <AdminPanel l={l} />}
      <div className="cols">
        <Members l={l} />
        <ActivityFeed l={l} />
      </div>
    </main>
  );
}
