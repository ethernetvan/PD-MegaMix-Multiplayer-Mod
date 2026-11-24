import { useEffect, useLayoutEffect, useReducer, useRef, useState } from "react";
import type { CSSProperties } from "react";
import { feedText } from "../feedText";
import { fmtElapsed, fmtPct, fmtRel, fmtScore } from "../format";
import { competitionRanks } from "../rank";
import { canAdmin, serverNow } from "../state";
import { useStore } from "../store";
import type { LobbySnap, MemberSnap, PickMode, StandingRow } from "../types";
import HexAvatar from "./HexAvatar";
import Trophies from "./Trophies";
import { autoLabel } from "../autoplay";
import { clearStatus, hasSessionResults, sessionOrder } from "../session";
import { memberAvatar, memberName, PhaseBadge } from "./Overview";
import { DiffChip, PlatIcon } from "./Chips";
import { diffLabel } from "../difficulty";

const PICK_LABEL: Record<PickMode, string> = {
  rotate: "Rotate",
  owner: "Owner picks",
  assigned: "Assigned",
};

function memberOf(l: LobbySnap, mid: string | null | undefined) {
  return l.members.find((m) => m.mid === mid);
}

/* Live leaderboard: rows are ordered by current % (the in-game win
 * metric), animate into place when positions change (FLIP), flash and
 * show a ▲/▼ marker on overtakes, and the combo count "pops" on every
 * increase. */
function LiveBoard({ l, now }: { l: LobbySnap; now: number }) {
  const rows = (l.round?.players ?? [])
    .map((mid) => memberOf(l, mid))
    .filter((m): m is MemberSnap => !!m)
    .sort((a, b) => b.pct - a.pct || b.score - a.score);
  const ranks = competitionRanks(rows);

  const rowEls = useRef(new Map<string, HTMLTableRowElement>());
  const prevIdx = useRef(new Map<string, number>());
  const preTops = useRef(new Map<string, number>());
  const timers = useRef(new Map<string, ReturnType<typeof setTimeout>>());
  const [moves, setMoves] = useState<Map<string, "up" | "down">>(new Map());
  const idx = new Map(rows.map((m, i) => [m.mid, i] as const));

  // pre-commit: DOM still shows the previous order; capture each row's
  // current visual top (includes any in-flight FLIP transform) so the
  // post-commit pass can glide rows from wherever they actually are.
  rowEls.current.forEach((el, mid) => {
    preTops.current.set(mid, el.getBoundingClientRect().top);
  });

  useLayoutEffect(() => {
    const mv = new Map<string, "up" | "down">();
    rows.forEach((m) => {
      const ni = idx.get(m.mid);
      const p = prevIdx.current.get(m.mid);
      if (p === undefined || ni === undefined || p === ni) return;
      mv.set(m.mid, ni < p ? "up" : "down");
      const el = rowEls.current.get(m.mid);
      if (!el) return;
      const t = getComputedStyle(el).transform;
      const m42 = t === "none" ? 0 : new DOMMatrixReadOnly(t).m42;
      const lastTop = el.getBoundingClientRect().top - (m42 || 0);
      const d = (preTops.current.get(m.mid) ?? lastTop) - lastTop;
      if (Math.abs(d) > 2) {
        el.style.transition = "none";
        el.style.transform = `translateY(${d}px)`;
        void el.offsetHeight;
        el.style.transition = "transform .55s cubic-bezier(.25,.9,.3,1)";
        el.style.transform = "";
      }
    });
    prevIdx.current = idx;
    if (mv.size) {
      setMoves((prev) => new Map([...prev, ...mv]));
      mv.forEach((_, mid) => {
        clearTimeout(timers.current.get(mid));
        timers.current.set(
          mid,
          setTimeout(() => {
            setMoves((prev) => {
              const n = new Map(prev);
              n.delete(mid);
              return n;
            });
          }, 1600),
        );
      });
    }
  });

  useEffect(
    () => () => timers.current.forEach((t) => clearTimeout(t)),
    [],
  );

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
            {rows.map((m, i) => {
              const lat = moves.get(m.mid);
              return (
                <tr
                  key={m.mid}
                  ref={(el) => {
                    if (el) rowEls.current.set(m.mid, el);
                    else rowEls.current.delete(m.mid);
                  }}
                  className={`${m.connected ? "" : "off"}${lat ? ` rd-${lat}` : ""}`}
                >
                  <td className="rank">
                    {ranks[i]}
                    {lat && (
                      <span className={`dmark ${lat}`} aria-hidden>
                        {lat === "up" ? "▲" : "▼"}
                      </span>
                    )}
                  </td>
                  <td>
                    <span className="p-cell">
                      <HexAvatar
                        name={m.name}
                        src={m.avatar ?? undefined}
                        size={32}
                        status={m.connected ? "online" : "offline"}
                      />
                      <span>
                        {m.name}
                        <DiffChip diff={m.diff} ed={m.ed} short />
                        {m.autoplay && (
                          <span className="badge auto">{autoLabel(m.autoplay_pct, m.autoplay_mode)}</span>
                        )}
                        {m.final && (
                          <span className="badge done">✓ finished</span>
                        )}
                      </span>
                    </span>
                  </td>
                  <td className="num score">{fmtScore(m.score)}</td>
                  <td className="pct">
                    <div
                      className="bar gauge"
                      style={
                        m.border > 0
                          ? ({ "--clear-at": `${Math.min(100, m.border)}%` } as CSSProperties)
                          : undefined
                      }
                      title={m.border > 0 ? `clear at ${fmtPct(m.border)}` : undefined}
                    >
                      <i
                        style={{
                          width: `${100 - Math.min(100, m.pct)}%`,
                        }}
                      />
                      {m.border > 0 && (
                        <>
                          <span className="gauge-clear" aria-hidden />
                          <span className="gauge-border" aria-hidden />
                        </>
                      )}
                    </div>
                    <span className="pct-num">{fmtPct(m.pct)}</span>
                  </td>
                  <td className="num combo">
                    <span key={m.combo} className="pop">
                      {m.combo}x
                    </span>
                  </td>
                  <td className="life-cell">
                    <div className="bar life">
                      <i style={{ width: `${(m.life / 255) * 100}%` }} />
                    </div>
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
    </section>
  );
}

/* Judgment slots sent by the client as judge_count[5], in the order the
 * MegaMix EN results screen lists them. NOTE: only aggregate counts are
 * reported — a true beat-by-beat judgment timeline would need a new
 * protocol field from the client (left for a future iteration). */
const JUDGE = [
  { label: "COOL", cls: "j-cool" },
  { label: "GOOD", cls: "j-good" },
  { label: "SAFE", cls: "j-safe" },
  { label: "BAD", cls: "j-bad" },
  { label: "MISS", cls: "j-miss" },
];

/* eased count-up for the results score reveal */
function useCountUp(target: number, delayMs: number, dur = 900) {
  const [v, setV] = useState(0);
  useEffect(() => {
    let raf = 0;
    const t0 = performance.now() + delayMs;
    const step = (t: number) => {
      const k = Math.min(1, Math.max(0, (t - t0) / dur));
      setV(Math.round(target * (1 - Math.pow(1 - k, 3))));
      if (k < 1) raf = requestAnimationFrame(step);
    };
    raf = requestAnimationFrame(step);
    return () => cancelAnimationFrame(raf);
  }, [target, delayMs, dur]);
  return v;
}

function ResCard({
  r,
  m,
  delay,
  tags,
  hideRank,
  win,
}: {
  r: StandingRow;
  m: MemberSnap | undefined;
  delay: number;
  tags: string[];
  hideRank?: boolean;
  win: boolean; /* in the top group (a tie for first ranks as 2) */
}) {
  const score = useCountUp(r.score, delay + 150);
  const jc = m?.final?.jc;
  const tot = (jc ?? []).reduce((a, b) => a + (b || 0), 0);
  const clear = clearStatus(r, !!m?.final);
  const d = { "--d": `${delay}ms` } as CSSProperties;
  return (
    <div
      className={`res-card${win ? " win" : ""}${clear === "dnf" ? " dnf" : ""}`}
      style={d}
    >
      <div className="res-top">
        {!hideRank && (
          <span className={`rank-num r${r.rank}`}>{r.rank}</span>
        )}
        <HexAvatar
          name={r.name}
          src={r.avatar ?? undefined}
          size={38}
          status={r.connected ? "online" : "offline"}
          ring={win}
        />
        <span className="res-name">
          {r.name}
          <DiffChip diff={r.diff} ed={r.ed} />
          {r.autoplay && <span className="badge auto">{autoLabel(r.autoplay_pct, r.autoplay_mode)}</span>}
          {win && <span className="badge star">★ WINNER</span>}
          {tags.map((t) => (
            <span key={t} className="badge star">
              ★ {t}
            </span>
          ))}
        </span>
        <span
          className={`res-clear ${clear}`}
          title={r.border > 0 ? `clear at ${fmtPct(r.border)}` : undefined}
        >
          {clear === "clear"
            ? "STAGE CLEAR"
            : clear === "fail"
              ? "NOT CLEAR…"
              : clear === "dnf"
                ? "DROP×OUT"
                : ""}
        </span>
        <span className="res-score">{fmtScore(score)}</span>
      </div>
      {jc && tot > 0 && (
        <div className="jcbar">
          {JUDGE.map((j, k) => (
            <i
              key={j.label}
              className={`jseg ${j.cls}`}
              style={
                {
                  width: `${((jc[k] ?? 0) / tot) * 100}%`,
                  "--k": `${k * 90}ms`,
                } as CSSProperties
              }
            />
          ))}
        </div>
      )}
      <div className="res-nums">
        {jc && tot > 0 ? (
          JUDGE.map((j, k) => (
            <span
              key={j.label}
              className={`jnum ${j.cls}`}
              style={{ "--k": `${520 + k * 65}ms` } as CSSProperties}
            >
              <b>{j.label}</b> {jc[k] ?? 0}
            </span>
          ))
        ) : (
          <span
            className="jnum dim-note"
            style={{ "--k": "520ms" } as CSSProperties}
          >
            no note data
          </span>
        )}
        <span
          className="jnum j-misc"
          style={{ "--k": "880ms" } as CSSProperties}
        >
          <b>MAX COMBO</b> {r.max_combo}x
        </span>
        <span
          className="jnum j-misc"
          style={{ "--k": "950ms" } as CSSProperties}
        >
          <b>%</b> {fmtPct(r.pct)}
        </span>
        {jc && tot > 0 && (
          <span
            className="jnum j-misc"
            style={{ "--k": "1020ms" } as CSSProperties}
          >
            <b>COOL RATE</b> {(((jc[0] ?? 0) / tot) * 100).toFixed(0)}%
          </span>
        )}
      </div>
    </div>
  );
}

/* Animated results: cards reveal last-place-first so the winner lands
 * last with the flourish, scores count up, and each player's judgment
 * breakdown cascades across a MegaMix-colored segment bar. */
function ResultsBoard({ l }: { l: LobbySnap }) {
  const st = l.standings;
  if (!st) return null;
  const n = st.e.length;
  const jcOf = (mid: string) => memberOf(l, mid)?.final?.jc;
  // superlatives among human players who actually reported notes
  const done = st.e.filter(
    (r) => !r.autoplay && (jcOf(r.mid) ?? []).some((v) => v > 0),
  );
  const tags = new Map<string, string[]>();
  const push = (mid: string, t: string) =>
    tags.set(mid, [...(tags.get(mid) ?? []), t]);
  if (done.length > 1) {
    const topCool = done.reduce((a, b) =>
      (jcOf(b.mid)?.[0] ?? 0) > (jcOf(a.mid)?.[0] ?? 0) ? b : a,
    );
    push(topCool.mid, "MOST COOLS");
    const topCombo = done.reduce((a, b) =>
      b.max_combo > a.max_combo ? b : a,
    );
    if (topCombo.max_combo > 0) push(topCombo.mid, "COMBO KING");
  }
  /* consecutive rows with the same rank render as one tie card */
  const groups: StandingRow[][] = [];
  for (const r of st.e) {
    const g = groups[groups.length - 1];
    if (g && g[0].rank === r.rank) g.push(r);
    else groups.push([r]);
  }
  let seen = 0;
  return (
    <section className="panel res-panel">
      <span className="res-ribbon" aria-hidden>
        RESULT
      </span>
      <h3>Results</h3>
      {groups.map((g) => {
        const i0 = seen;
        seen += g.length;
        if (g.length === 1) {
          const r = g[0];
          return (
            <ResCard
              key={r.mid}
              r={r}
              m={memberOf(l, r.mid)}
              delay={(n - 1 - i0) * 170}
              tags={tags.get(r.mid) ?? []}
              win={i0 === 0}
            />
          );
        }
        const delay = (n - 1 - i0) * 170;   /* all tied members together */
        return (
          <div className="tie-group" key={`tie-${g[0].rank}-${i0}`}>
            <div className="tie-head">
              <span className={`rank-num r${g[0].rank}`}>{g[0].rank}</span>
              <span className="badge star">TIE</span>
            </div>
            <div className="tie-cards">
              {g.map((r) => (
                <ResCard
                  key={r.mid}
                  r={r}
                  m={memberOf(l, r.mid)}
                  delay={delay}
                  tags={tags.get(r.mid) ?? []}
                  hideRank
                  win={i0 === 0}
                />
              ))}
            </div>
          </div>
        );
      })}
    </section>
  );
}

/* top three of the room session ranking (wins, then placement score) */
function SessionPodium({ l }: { l: LobbySnap }) {
  if (!hasSessionResults(l.members)) return null;
  const top = sessionOrder(l.members).slice(0, 3);
  return (
    <div className="session">
      <h4>Session ranking</h4>
      <ol className="session-top">
        {top.map((m, i) => (
          <li key={m.mid} className={`sp${i + 1}${m.connected ? "" : " off"}`}>
            <span className={`rank-num r${i + 1}`}>{i + 1}</span>
            <HexAvatar
              name={m.name}
              src={m.avatar ?? undefined}
              size={i === 0 ? 46 : 38}
              ring={i === 0}
            />
            <span className="sp-name">{m.name}</span>
            <Trophies places={m.session.places} />
          </li>
        ))}
      </ol>
    </div>
  );
}

function PhasePanel({ l, now }: { l: LobbySnap; now: number }) {
  if (l.phase === "SELECT") {
    const noSong = l.sel ? l.members.filter((m) => m.nosong) : [];
    const notReady = l.members.filter(
      (m) => m.connected && !m.ready && !noSong.includes(m),
    );
    const picker = memberOf(l, l.leader);
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
            {l.same_diff && <DiffChip diff={picker?.diff} ed={picker?.ed} />}
          </p>
        )}
        {noSong.length > 0 && (
          <div className="nosong-strip" role="status">
            <span className="nosong-ico" aria-hidden>
              !
            </span>
            <span className="nosong-text">
              <b>
                {noSong.length === 1
                  ? `${noSong[0].name} doesn't have this song`
                  : `${noSong.length} players don't have this song`}
              </b>
              <span className="nosong-sub">
                {noSong.length === 1
                  ? "They will sit this round out"
                  : `${noSong.map((m) => m.name).join(", ")} will sit this round out`}
              </span>
            </span>
            <span className="nosong-hexes">
              {noSong.map((m) => (
                <HexAvatar key={m.mid} name={m.name} src={m.avatar ?? undefined} size={30} />
              ))}
            </span>
          </div>
        )}
        <SessionPodium l={l} />
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
                <HexAvatar name={m.name} src={m.avatar ?? undefined} size={30} />
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
    return <LiveBoard l={l} now={now} />;
  }
  const st = l.standings;
  if (!st) return null;
  if (st.complete) return <ResultsBoard l={l} />;
  return (
    <section className="panel">
      <h3>
        Results <span className="dim">(waiting for players…)</span>
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
                      src={r.avatar ?? undefined}
                      size={32}
                      status={r.connected ? "online" : "offline"}
                    />
                    <span>
                      {r.name}
                      <DiffChip diff={r.diff} ed={r.ed} short />
                      {r.autoplay && <span className="badge auto">{autoLabel(r.autoplay_pct, r.autoplay_mode)}</span>}
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
      <h3>
        Members <span className="dim">· by session wins</span>
      </h3>
      <ul className="members">
        {sessionOrder(l.members).map((m, i) => (
          <li key={m.mid} className={m.connected ? "" : "off"}>
            <span className="mem-hex">
              <HexAvatar
                name={m.name}
                src={m.avatar ?? undefined}
                size={56}
                status={m.connected ? "online" : "offline"}
                ring={
                  m.mid === l.leader ||
                  m.mid === l.owner ||
                  (state.you.name !== null && m.name === state.you.name)
                }
              />
              <span className="mem-num">{i + 1}</span>
              {(m.plat === "pc" || m.plat === "switch") && (
                <span className="mem-plat">
                  <PlatIcon plat={m.plat} size={14} />
                </span>
              )}
            </span>
            <span className="mem-name">{m.name}</span>
            {m.session && <Trophies places={m.session.places} size={16} />}
            <span className="mem-badges">
              <DiffChip diff={m.diff} ed={m.ed} />
              {m.nosong && l.phase === "SELECT" && (
                <span className="badge nosong" title="This console doesn't have the selected song">
                  No song
                </span>
              )}
              {m.mid === l.owner && <span className="badge">Owner</span>}
              {m.mid === l.leader && <span className="badge pick">Picker</span>}
              {m.role !== "player" && <span className="badge">{m.role}</span>}
              {m.autoplay && <span className="badge auto">{autoLabel(m.autoplay_pct, m.autoplay_mode)}</span>}
              {!m.connected && <span className="badge off">offline</span>}
              {m.connected && !m.ready && !(m.nosong && l.phase === "SELECT") && (
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
      <div className="opt-row">
        <span className="opt-label">
          Same difficulty
          <span className="dim">
            {l.same_diff
              ? "Everyone plays the picker's difficulty"
              : "Everyone picks their own difficulty"}
          </span>
        </span>
        <div className="seg" role="group" aria-label="Same difficulty">
          {[true, false].map((v) => (
            <button
              key={String(v)}
              className={`seg-btn${!!l.same_diff === v ? " on" : ""}`}
              aria-pressed={!!l.same_diff === v}
              onClick={() => act({ op: "set_opt", key: "same_diff", value: v })}
            >
              {v ? "On" : "Off"}
            </button>
          ))}
        </div>
      </div>
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
              src={m.avatar ?? undefined}
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
            {m.avatar && (
              <button
                className="btn sm"
                title="Stop showing this player's uploaded portrait here and on every console"
                onClick={() => act({ op: "hide_avatar", member: m.mid })}
              >
                Hide portrait
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
            owner <HexAvatar name={memberName(l, l.owner)} src={memberAvatar(l, l.owner)} size={20} />{" "}
            {memberName(l, l.owner)}
          </span>
          <span className="mi">
            picker <HexAvatar name={memberName(l, l.leader)} src={memberAvatar(l, l.leader)} size={20} />{" "}
            {memberName(l, l.leader)}
          </span>
          <span>{PICK_LABEL[l.pick_mode]}</span>
          {l.same_diff && (
            <span className="mi" title="Room option: everyone plays the picker's difficulty">
              same difficulty
              {diffLabel(memberOf(l, l.leader)?.diff, memberOf(l, l.leader)?.ed) && (
                <DiffChip diff={memberOf(l, l.leader)?.diff} ed={memberOf(l, l.leader)?.ed} />
              )}
            </span>
          )}
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
