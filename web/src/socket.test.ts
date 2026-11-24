import { describe, expect, it } from "vitest";
import {
  connStatus,
  endpointLabel,
  isLocalHost,
  resolveWsTarget,
  credsAfterRefusal,
  retryAuthName,
} from "./api/socket";

const loc = (href: string) => {
  const u = new URL(href);
  return { protocol: u.protocol, hostname: u.hostname, host: u.host, port: u.port };
};

describe("resolveWsTarget", () => {
  it("uses same-host wss /ws on the public domain", () => {
    const t = resolveWsTarget(loc("https://mikumod.evanthomas.dev/"), {});
    expect(t.url).toBe("wss://mikumod.evanthomas.dev/ws");
    expect(t.remote).toBe(true);
    expect(endpointLabel(t)).toBe("mikumod.evanthomas.dev (Live Remote)");
  });
  it("uses ws on a plain-http public host", () => {
    const t = resolveWsTarget(loc("http://example.com:8000/lobby/AB"), {});
    expect(t.url).toBe("ws://example.com:8000/ws");
    expect(t.remote).toBe(true);
  });
  it("talks to the page's own port when the local server serves the UI", () => {
    for (const h of ["localhost", "127.0.0.1", "192.168.1.20", "10.169.1.126"]) {
      const t = resolveWsTarget(loc(`http://${h}:8080/`), {});
      expect(t.url).toBe(`ws://${h}:8080/ws`);
      expect(t.remote).toBe(false);
      expect(endpointLabel(t)).toBe("Local Server");
    }
    expect(resolveWsTarget(loc("http://localhost:18080/"), {}).url).toBe(
      "ws://localhost:18080/ws",
    );
  });
  it("uses the local server port under the vite dev server", () => {
    expect(resolveWsTarget(loc("http://localhost:5173/"), { DEV: true }).url).toBe(
      "ws://localhost:8080/ws",
    );
    expect(
      resolveWsTarget(loc("http://192.168.0.5:5173/"), {
        DEV: true,
        VITE_LOCAL_WS_PORT: "18080",
      }).url,
    ).toBe("ws://192.168.0.5:18080/ws");
  });
  it("honours VITE_WS_URL", () => {
    const t = resolveWsTarget(loc("http://localhost:5173/"), {
      DEV: true,
      VITE_WS_URL: "wss://mikumod.evanthomas.dev/ws",
    });
    expect(t.url).toBe("wss://mikumod.evanthomas.dev/ws");
    expect(t.remote).toBe(true);
    const l = resolveWsTarget(loc("https://mikumod.evanthomas.dev/"), {
      VITE_WS_URL: "ws://127.0.0.1:8080/ws",
    });
    expect(l.remote).toBe(false);
  });
});

describe("isLocalHost", () => {
  it("classifies hosts", () => {
    for (const h of ["localhost", "127.0.0.1", "10.0.0.1", "172.16.4.4",
      "172.31.0.1", "192.168.1.1", "169.254.3.3", "[::1]", "mypc", "nas.local"])
      expect(isLocalHost(h), h).toBe(true);
    for (const h of ["mikumod.evanthomas.dev", "8.8.8.8", "172.32.0.1",
      "100.64.0.1", "example.onrender.com"])
      expect(isLocalHost(h), h).toBe(false);
  });
});

describe("connStatus", () => {
  it("first attempt is connecting, retries are reconnecting", () => {
    expect(connStatus(false)).toBe("connecting");
    expect(connStatus(true)).toBe("reconnecting");
  });
});

describe("credsAfterRefusal", () => {
  const all = { name: "n", token: "t", key: "ABCDE-FGHJK" };
  it("drops the refused credential and keeps the rest", () => {
    expect(credsAfterRefusal({ ok: false, error: "bad_token" }, all)?.creds).toEqual({
      name: "n",
      token: null,
      key: "ABCDE-FGHJK",
    });
    expect(credsAfterRefusal({ ok: false, error: "bad_key" }, all)?.creds).toEqual({
      name: "n",
      token: "t",
      key: null,
    });
  });
  it("nothing to retry when it worked or nothing was saved", () => {
    expect(credsAfterRefusal({ ok: true }, all)).toBeNull();
    expect(credsAfterRefusal({ ok: false, error: "bad_request" }, all)).toBeNull();
    expect(
      credsAfterRefusal({ ok: false, error: "bad_key" }, { ...all, key: null }),
    ).toBeNull();
  });
});

describe("retryAuthName", () => {
  it("retries with name only on bad_token when a token was saved", () => {
    expect(retryAuthName({ ok: false, error: "bad_token" }, true)).toBe(true);
  });
  it("does not retry without a saved token or on other errors", () => {
    expect(retryAuthName({ ok: false, error: "bad_token" }, false)).toBe(false);
    expect(retryAuthName({ ok: false, error: "forbidden" }, true)).toBe(false);
    expect(retryAuthName({ ok: true }, true)).toBe(false);
  });
});
