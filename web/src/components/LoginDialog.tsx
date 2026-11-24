import { useState, type FormEvent } from "react";
import { getCreds } from "../auth";
import { useStore } from "../store";

export default function LoginDialog({ onClose }: { onClose: () => void }) {
  const { auth, state } = useStore();
  const saved = getCreds();
  const [name, setName] = useState(state.you.name ?? "");
  const [token, setToken] = useState(saved.token ?? "");
  const [showToken, setShowToken] = useState(!!saved.token);
  const [err, setErr] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  const submit = async (ev: FormEvent) => {
    ev.preventDefault();
    setBusy(true);
    setErr(null);
    try {
      const r = await auth(name.trim() || null, token.trim() || null);
      if (r.ok) onClose();
      else setErr(r.error === "bad_token" ? "Wrong admin token." : r.error ?? "failed");
    } catch {
      setErr("Not connected to the server.");
    }
    setBusy(false);
  };

  return (
    <div className="overlay" onClick={onClose}>
      <form className="dialog" onClick={(e) => e.stopPropagation()} onSubmit={submit}>
        <h2>Login</h2>
        <label>
          Lobby creator&apos;s username
          <input
            autoFocus
            value={name}
            onChange={(e) => setName(e.target.value)}
            placeholder="e.g. MikuFan"
          />
        </label>
        <p className="hint">
          Must match the in-game name of the lobby owner to unlock owner
          controls for their lobbies.
        </p>
        <button
          type="button"
          className="btn link"
          onClick={() => setShowToken(!showToken)}
        >
          {showToken ? "▾" : "▸"} Server admin token
        </button>
        {showToken && (
          <label>
            Admin token
            <input
              type="password"
              value={token}
              onChange={(e) => setToken(e.target.value)}
              placeholder="from the server log or ?token= link"
            />
          </label>
        )}
        {err && <p className="err">{err}</p>}
        <div className="row">
          <button className="btn primary" disabled={busy} type="submit">
            {busy ? "…" : "Sign in"}
          </button>
          <button className="btn" type="button" onClick={onClose}>
            Cancel
          </button>
        </div>
      </form>
    </div>
  );
}
