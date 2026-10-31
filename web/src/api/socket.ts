import { getCreds, saveCreds } from "../auth";
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

// A bad_token reply to an automatic re-auth means the server restarted
// with a fresh token: drop the stored one and retry with the name only.
export const retryAuthName = (
  reply: { ok: boolean; error?: string },
  hadToken: boolean,
): boolean => !reply.ok && reply.error === "bad_token" && hadToken;

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
    const proto = location.protocol === "https:" ? "wss" : "ws";
    const ws = new WebSocket(`${proto}://${location.host}/ws`);
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

  private reauth() {
    const { name, token } = getCreds();
    if (!name && !token) return;
    this.request({ t: "auth", name, token })
      .then(async (r) => {
        if (r.ok && r.you) {
          this.onyou(r.you);
          return;
        }
        if (retryAuthName(r, !!token)) {
          saveCreds({ name, token: null });
          this.onnotice(
            "Saved admin token is no longer valid — sign in again",
          );
          try {
            const r2 = await this.request({ t: "auth", name, token: null });
            if (r2.ok && r2.you) this.onyou(r2.you);
          } catch {
            /* stay signed out */
          }
        }
      })
      .catch(() => {});
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

  auth(name: string | null, token: string | null): Promise<ReplyMsg> {
    return this.request({ t: "auth", name, token });
  }

  action(msg: Record<string, unknown>): Promise<ReplyMsg> {
    return this.request({ t: "action", ...msg });
  }
}
