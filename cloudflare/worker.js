// mikumod-switch: Cloudflare Worker on mikumod.evanthomas.dev/switch*
//
// Consoles have no TLS, so they open ws://mikumod.evanthomas.dev/switch over
// plain HTTP. Render (reached through Cloudflare O2O) 301-redirects every
// http:// request to https, which a console can't follow. This Worker takes
// the console's plain-HTTP request and re-issues it over https to the Render
// service's own onrender.com host (a subrequest back to our own hostname is
// not delivered to Render on an O2O hostname: empty 404). For a WebSocket
// upgrade, returning fetch()'s 101 response makes Cloudflare pipe the socket
// through untouched.
//
// Render sees every subrequest of this Worker as coming from one address
// (Cloudflare's Workers address), so the console's own address goes along in
// X-Console-IP and the server counts its per-address connection limits
// against that. The server only believes the header together with the
// secret both sides were given: `npx wrangler secret put PROXY_SECRET` here,
// MP_PROXY_SECRET on Render. Without the secret nothing is added (consoles
// then share one address and only the server-wide limits apply to them).
const ORIGIN = "https://mikumod.onrender.com";

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const out = new Request(ORIGIN + url.pathname + url.search, request);
    // never pass along what the console itself sent under these names
    out.headers.delete("X-Console-IP");
    out.headers.delete("X-Console-Auth");
    const ip = request.headers.get("CF-Connecting-IP");
    if (env && env.PROXY_SECRET && ip) {
      out.headers.set("X-Console-IP", ip);
      out.headers.set("X-Console-Auth", env.PROXY_SECRET);
    }
    return fetch(out);
  },
};
