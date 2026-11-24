export interface Creds {
  name: string | null;
  token: string | null;
  /** a room's web key (the owner reads it off the game) */
  key: string | null;
}

const NAME_KEY = "mmmp.name";
const TOKEN_KEY = "mmmp.token";
const ROOM_KEY = "mmmp.roomkey";

/** What someone typed -> "ABCDE-FGHJK", or null when it can't be a room key
 *  (10 characters, no I/O/0/1; dashes and spaces don't matter). */
export function formatRoomKey(typed: string): string | null {
  const k = typed.toUpperCase().replace(/[-\s]/g, "");
  if (!/^[A-HJ-NP-Z2-9]{10}$/.test(k)) return null;
  return `${k.slice(0, 5)}-${k.slice(5)}`;
}

export function tokenFromUrl(search: string): string | null {
  const t = new URLSearchParams(search).get("token");
  return t && t.trim() ? t : null;
}

export function stripToken(search: string): string {
  const p = new URLSearchParams(search);
  p.delete("token");
  const s = p.toString();
  return s ? `?${s}` : "";
}

export function getCreds(): Creds {
  return {
    name: localStorage.getItem(NAME_KEY),
    token: localStorage.getItem(TOKEN_KEY),
    key: localStorage.getItem(ROOM_KEY),
  };
}

export function saveCreds(c: Creds) {
  if (c.name) localStorage.setItem(NAME_KEY, c.name);
  else localStorage.removeItem(NAME_KEY);
  if (c.token) localStorage.setItem(TOKEN_KEY, c.token);
  else localStorage.removeItem(TOKEN_KEY);
  if (c.key) localStorage.setItem(ROOM_KEY, c.key);
  else localStorage.removeItem(ROOM_KEY);
}

// Runs once at startup: pulls ?token= out of the URL into storage so the
// shared link works but the token doesn't stick around in the address bar.
export function initAuth(): Creds {
  const t = tokenFromUrl(window.location.search);
  if (t) {
    localStorage.setItem(TOKEN_KEY, t);
    history.replaceState(
      null,
      "",
      window.location.pathname +
        stripToken(window.location.search) +
        window.location.hash,
    );
  }
  return getCreds();
}
