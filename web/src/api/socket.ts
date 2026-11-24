import { getCreds, saveCreds, type Creds } from "../auth";
import type { ConnStatus } from "../state";
import type { ReplyMsg, ServerMsg, You } from "../types";

interface Pending {
  resolve: (m: ReplyMsg) => void;
  reject: (e: Error) => void;
  timer: ReturnType<typeof setTimeout>;
}

// pure helpers (vitest)
export const connStatus = (everConnected: boolean): ConnStatus =>
  everConnected ? "reconnecting" : "connecting";

// The local server's web/WebSocket port (mp_server.py --web-port default)
export const LOCAL_WS_PORT = 8080;

export interface PageLocation {
  protocol: string;
  hostname: string;
  host: string;
  port: string;
}

export interface WsEnv {
  VITE_WS_URL?: string;
  VITE_LOCAL_WS_PORT?: string;
  DEV?: boolean;
}

export interface WsTarget {
  url: string;
  remote: boolean; // public deployment (e.g. mikumod.evanthomas.dev)
  host: string; // host[:port] the socket goes to
}

// localhost, loopback, private/link-local IPv4, single-label and .local
// names, IPv6 loopback/ULA/link-local: a server on this PC or the LAN
export const isLocalHost = (hostname: string): boolean => {
  const h = hostname.toLowerCase().replace(/^\[|\]$/g, "");
  if (!h || h === "localhost" || h.endsWith(".localhost")) return true;
  if (h.endsWith(".local") || h.endsWith(".lan") || h.endsWith(".home.arpa"))
    return true;
  if (h.includes(":"))
    return h === "::1" || /^f[cd][0-9a-f]{2}:/.test(h) || h.startsWith("fe80:");
  const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(h);
  if (!m) return !h.includes(".");
  const a = Number(m[1]);
  const b = Number(m[2]);
  return (
    a === 127 ||
    a === 10 ||
    (a === 172 && b >= 16 && b <= 31) ||
    (a === 192 && b === 168) ||
    (a === 169 && b === 254)
  );
};

const urlHost = (url: string): { host: string; hostname: string } => {
  try {
    const u = new URL(url);
    return { host: u.host, hostname: u.hostname };
  } catch {
    return { host: url, hostname: url };
  }
};

// Where the dashboard's WebSocket goes:
//  1. VITE_WS_URL if set (build-time override)
//  2. a public host (mikumod.evanthomas.dev): same host, /ws, wss on https
//  3. a local/LAN host: the local server's port (VITE_LOCAL_WS_PORT; the
//     page's own port when the server serves the built UI; 8080 under the
//     vite dev server)
export const resolveWsTarget = (loc: PageLocation, env: WsEnv): WsTarget => {
  const override = env.VITE_WS_URL?.trim();
  if (override) {
    const u = urlHost(override);
    return { url: override, remote: !isLocalHost(u.hostname), host: u.host };
  }
  const proto = loc.protocol === "https:" ? "wss" : "ws";
  if (!isLocalHost(loc.hostname))
    return { url: `${proto}://${loc.host}/ws`, remote: true, host: loc.host };
  const port =
    env.VITE_LOCAL_WS_PORT?.trim() ||
    (env.DEV ? String(LOCAL_WS_PORT) : loc.port);
  const host = port ? `${loc.hostname}:${port}` : loc.hostname;
  return { url: `${proto}://${host}/ws`, remote: false, host };
};

export const endpointLabel = (t: WsTarget): string =>
  t.remote ? `${t.host.replace(/:443$/, "")} (Live Remote)` : "Local Server";

export const pageWsTarget = (): WsTarget =>
  resolveWsTarget(window.location, import.meta.env as WsEnv);

// A bad_token reply to an automatic re-auth means the server restarted
// with a fresh token: drop the stored one and retry with the name only.
export const retryAuthName = (
  reply: { ok: boolean; error?: string },
  hadToken: boolean,
): boolean => !reply.ok && reply.error === "bad_token" && hadToken;

// A saved credential the server no longer accepts (the server restarted
// with another token; the room closed or changed owner): what is left to
// sign in with, and what to tell the user. null = nothing to retry.
export const credsAfterRefusal = (
  reply: { ok: boolean; error?: string },
  c: Creds,
): { creds: Creds; notice: string } | null => {
  if (reply.ok) return null;
  if (reply.error === "bad_token" && c.token)
    return {
      creds: { ...c, token: null },
      notice: "Saved admin token is no longer valid — sign in again",
    };
  if (reply.error === "bad_key" && c.key)
    return {
      creds: { ...c, key: null },
      notice: "Saved room key no longer works — the room closed or has a new owner",
    };
  return null;
};

export class MpSocket {
  onmessage: (msg: ServerMsg) => void = () => {};
  onstatus: (s: ConnStatus) => void = () => {};
  onyou: (you: You) => void = () => {};
  onnotice: (text: string) => void = () => {};
  private ws: WebSocket | null = null;
  private seq = 1;
  private pending = new Map<number, Pending>();
  private backoff = 500;
  private timer: ReturnType<typeof setTimeout> | null = null;
  private stopped = false;
  private everConnected = false;

  start() {
    this.stopped = false;
    this.connect();
  }

  stop() {
    this.stopped = true;
    if (this.timer) clearTimeout(this.timer);
    this.ws?.close();
  }

  private connect() {
    this.onstatus(connStatus(this.everConnected));
    const ws = new WebSocket(pageWsTarget().url);
    this.ws = ws;
    ws.onopen = () => {
      this.everConnected = true;
      this.backoff = 500;
      this.onstatus("live");
      this.reauth();
    };
    ws.onmessage = (ev) => {
      let msg: ServerMsg;
      try {
        msg = JSON.parse(ev.data as string) as ServerMsg;
      } catch {
        return;
      }
      if (msg.t === "reply" && typeof msg.id === "number") {
        const p = this.pending.get(msg.id);
        if (p) {
          this.pending.delete(msg.id);
          clearTimeout(p.timer);
          p.resolve(msg);
          return;
        }
      }
      this.onmessage(msg);
    };
    ws.onclose = () => {
      if (this.ws !== ws) return; // stale socket from a previous connect()
      this.ws = null;
      for (const p of this.pending.values()) {
        clearTimeout(p.timer);
        p.reject(new Error("disconnected"));
      }
      this.pending.clear();
      if (!this.stopped) {
        this.onstatus("reconnecting");
        this.timer = setTimeout(() => this.connect(), this.backoff);
        this.backoff = Math.min(5000, this.backoff * 2);
      }
    };
    ws.onerror = () => ws.close();
  }

  private async reauth() {
    let c = getCreds();
    try {
      // each refused credential is dropped and the rest tried again
      for (let i = 0; i < 3 && (c.name || c.token || c.key); i++) {
        const r = await this.request({ t: "auth", ...c });
        if (r.ok && r.you) {
          this.onyou(r.you);
          return;
        }
        const next = credsAfterRefusal(r, c);
        if (!next) return;
        c = next.creds;
        saveCreds(c);
        this.onnotice(next.notice);
      }
    } catch {
      /* stay signed out */
    }
  }

  request(msg: Record<string, unknown>): Promise<ReplyMsg> {
    return new Promise((resolve, reject) => {
      const ws = this.ws;
      if (!ws || ws.readyState !== WebSocket.OPEN) {
        reject(new Error("not connected"));
        return;
      }
      const id = this.seq++;
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error("timeout"));
      }, 5000);
      this.pending.set(id, { resolve, reject, timer });
      ws.send(JSON.stringify({ ...msg, id }));
    });
  }

  auth(
    name: string | null,
    token: string | null,
    key: string | null = null,
  ): Promise<ReplyMsg> {
    return this.request({ t: "auth", name, token, key });
  }

  action(msg: Record<string, unknown>): Promise<ReplyMsg> {
    return this.request({ t: "action", ...msg });
  }
}
