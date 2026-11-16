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
const ORIGIN = "https://mikumod.onrender.com";

export default {
  async fetch(request) {
    const url = new URL(request.url);
    return fetch(new Request(ORIGIN + url.pathname + url.search, request));
  },
};
