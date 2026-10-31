// Hexagonal player avatar, in the style of the MegaMix room overlay.
// Placeholder art: a deterministic 2-stop diagonal gradient picked from a
// DIVA-ish palette by hashing the player name, with the first initial in
// condensed bold type. `src` is reserved for future per-player icon images.

const GRADS: [string, string][] = [
  ["#39c5bb", "#128f8a"], // miku teal
  ["#e12885", "#9e1660"], // magenta
  ["#ffc933", "#ff9f2e"], // yellow -> orange
  ["#57d15c", "#2e9e4f"], // clear green
  ["#9b4dca", "#6a2bb0"], // purple
  ["#4fa3ff", "#2e6be6"], // blue
  ["#ff6b81", "#e12885"], // pink -> magenta
  ["#00c2b8", "#4fa3ff"], // teal -> blue
];

function hashName(s: string): number {
  let h = 5381;
  for (let i = 0; i < s.length; i++) h = ((h << 5) + h + s.charCodeAt(i)) | 0;
  return Math.abs(h);
}

export interface HexAvatarProps {
  name: string;
  size?: number;
  className?: string;
  /** Future icon image; when set it replaces the gradient placeholder. */
  src?: string;
  /** Green dot when online, gray when offline, none when unset. */
  status?: "online" | "offline";
  /** Teal ring outline, used for the leader/self like the room overlay. */
  ring?: boolean;
}

export default function HexAvatar({
  name,
  size = 32,
  className = "",
  src,
  status,
  ring,
}: HexAvatarProps) {
  const [c1, c2] = GRADS[hashName(name || "?") % GRADS.length];
  const initial = (name.trim().charAt(0) || "?").toUpperCase();
  return (
    <span
      className={`hexav${ring ? " ring" : ""}${className ? " " + className : ""}`}
      style={{ width: size, height: size }}
      role="img"
      aria-label={name}
      title={name}
    >
      {ring && <span className="hexav-ring" aria-hidden />}
      <span
        className="hexav-in"
        style={
          src
            ? {
                backgroundImage: `url(${src})`,
                backgroundSize: "cover",
                backgroundPosition: "center",
              }
            : {
                backgroundImage: `linear-gradient(135deg, ${c1}, ${c2})`,
              }
        }
      >
        {!src && (
          <span className="hexav-i" style={{ fontSize: size * 0.42 }}>
            {initial}
          </span>
        )}
      </span>
      {status && <i className={`hexav-dot ${status}`} aria-hidden />}
    </span>
  );
}
