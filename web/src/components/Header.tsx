import { endpointLabel, pageWsTarget } from "../api/socket";
import { useStore } from "../store";

const target = pageWsTarget();

export default function Header({ onLogin }: { onLogin: () => void }) {
  const { state, logout } = useStore();
  const you = state.you;
  const snap = state.snapshot;
  const where = endpointLabel(target);
  return (
    <header className="hdr">
      <a className="title logo" href="#/" aria-label="MegaMix Multiplayer">
        <span className="logo-word" aria-hidden>
          <span className="logo-mega">MEGA</span>
          <span className="logo-mix">MIX</span>
        </span>
        <span className="logo-sub" aria-hidden>
          Multiplayer
        </span>
      </a>
      {snap && <span className="srv">{snap.server.name}</span>}
      <span
        className={`endpoint ${target.remote ? "remote" : "local"}`}
        title={`Dashboard connected to ${target.url}`}
      >
        {where}
      </span>
      <span className={`pill ${state.status}`}>
        <i className="pill-dot" aria-hidden />
        {state.status === "live" ? "live" : state.status}
      </span>
      {state.status === "live" && (
        <span className="dim hdr-consoles">
          {snap ? snap.server.clients_online : 0} console
          {snap && snap.server.clients_online === 1 ? "" : "s"} online
        </span>
      )}
      <span className="spacer" />
      {you.name || you.admin || you.owns.length > 0 ? (
        <span className="who">
          Signed in as{" "}
          <button className="btn link who-name" onClick={onLogin}>
            {you.name ?? (you.admin ? "server admin" : `host of ${you.owns.join(", ")}`)}
          </button>
          {you.admin && <span className="badge admin">Admin</span>}
          <button className="btn ghost" onClick={logout}>
            Logout
          </button>
        </span>
      ) : (
        <button className="btn chev" onClick={onLogin}>
          Login
        </button>
      )}
    </header>
  );
}
