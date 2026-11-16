// mikumod-switch: Cloudflare Worker on mikumod.evanthomas.dev/switch*
//
// Consoles have no TLS, so they open ws://mikumod.evanthomas.dev/switch over
// plain HTTP. Render (reached through Cloudflare O2O) 301-redirects every
// http:// request to https, which a console can't follow. This Worker takes
// the console's plain-HTTP request and re-issues it to the same host over
// https; for a WebSocket upgrade, returning fetch()'s 101 response makes
// Cloudflare pipe the socket through untouched.
export default {
  async fetch(request) {
    const url = new URL(request.url);
    url.protocol = "https:";
    url.port = "";
    return fetch(new Request(url.toString(), request));
  },
};
