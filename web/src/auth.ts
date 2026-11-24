export interface Creds {
  name: string | null;
  token: string | null;
}

const NAME_KEY = "mmmp.name";
const TOKEN_KEY = "mmmp.token";

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
  };
}

export function saveCreds(c: Creds) {
  if (c.name) localStorage.setItem(NAME_KEY, c.name);
  else localStorage.removeItem(NAME_KEY);
  if (c.token) localStorage.setItem(TOKEN_KEY, c.token);
  else localStorage.removeItem(TOKEN_KEY);
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
