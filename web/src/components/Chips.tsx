import { diffClass, diffLabel, diffShort, platLabel } from "../difficulty";

/* The chart a player is on, in the game's own difficulty colours.
 * Renders nothing for consoles that don't report one. */
export function DiffChip({
  diff,
  ed,
  short,
}: {
  diff: number | undefined | null;
  ed?: number | null;
  short?: boolean;
}) {
  const cls = diffClass(diff, ed);
  const label = diffLabel(diff, ed);
  if (!cls || !label) return null;
  return (
    <span className={`diff-chip d-${cls}`} title={label}>
      {short ? diffShort(diff, ed) : label}
    </span>
  );
}

/* Which version of the mod a player runs: a Joy-Con pair for the Switch
 * game (Mega Mix), a monitor for the PC game (Mega Mix+). */
export function PlatIcon({
  plat,
  size = 18,
}: {
  plat: string | undefined | null;
  size?: number;
}) {
  const label = platLabel(plat);
  if (!label) return null;
  return (
    <svg
      className={`plat-icon p-${plat}`}
      width={size}
      height={size}
      viewBox="0 0 24 24"
      role="img"
      aria-label={label}
    >
      <title>{label}</title>
      {plat === "pc" ? (
        <>
          <rect className="pi-body" x="2" y="3.5" width="20" height="13" rx="2" />
          <rect className="pi-cut" x="4.2" y="5.7" width="15.6" height="8.6" rx="0.8" />
          <rect className="pi-body" x="10.4" y="16.5" width="3.2" height="2.6" />
          <rect className="pi-body" x="6.5" y="19" width="11" height="2" rx="1" />
        </>
      ) : (
        <>
          <path
            className="pi-body"
            d="M7 2.5h3.6v19H7a4.5 4.5 0 0 1-4.5-4.5V7A4.5 4.5 0 0 1 7 2.5z"
          />
          <path
            className="pi-body"
            d="M13.4 2.5H17A4.5 4.5 0 0 1 21.5 7v10a4.5 4.5 0 0 1-4.5 4.5h-3.6z"
          />
          <circle className="pi-cut" cx="6.8" cy="7.6" r="1.9" />
          <circle className="pi-cut" cx="17.2" cy="13.6" r="1.9" />
        </>
      )}
    </svg>
  );
}
