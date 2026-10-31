import { useStore } from "../store";

export default function Header({ onLogin }: { onLogin: () => void }) {
  const { state, logout } = useStore();
  const you = state.you;
  const snap = state.snapshot;
  return (
    <header className="hdr">
      <a className="title" href="#/">
        MegaMix Multiplayer
      </a>
      {snap && <span className="srv">{snap.server.name}</span>}
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
      {you.name || you.admin ? (
        <span className="who">
          Signed in as{" "}
          <button className="btn link who-name" onClick={onLogin}>
            {you.name ?? "server admin"}
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
