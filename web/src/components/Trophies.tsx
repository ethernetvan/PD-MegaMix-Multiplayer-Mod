/* trophy with its place number in the cup (1 gold, 2 silver, 3 bronze) */
export function Trophy({ place, size = 18 }: { place: 1 | 2 | 3; size?: number }) {
  return (
    <svg
      className={`trophy t${place}`}
      width={size}
      height={size}
      viewBox="0 0 24 24"
      aria-hidden
    >
      <path
        className="handle"
        d="M5.5 4H2.5v2.2A4 4 0 0 0 6 10.2M18.5 4h3v2.2a4 4 0 0 1-3.5 4"
      />
      <path className="cup" d="M5 2h14v5.5a7 7 0 0 1-14 0z" />
      <rect className="cup" x="10.6" y="13.5" width="2.8" height="4.5" />
      <rect className="cup" x="6.5" y="18" width="11" height="3.5" rx="1" />
      <text x="12" y="11.6" textAnchor="middle">
        {place}
      </text>
    </svg>
  );
}

/* [1] n  [2] n  [3] n: times ranked 1st / 2nd / 3rd this room session */
export default function Trophies({
  places,
  size,
}: {
  places: [number, number, number];
  size?: number;
}) {
  const label = `1st ×${places[0]}, 2nd ×${places[1]}, 3rd ×${places[2]}`;
  return (
    <span className="trophies" title={label} aria-label={label}>
      {([1, 2, 3] as const).map((p) => (
        <span key={p} className={`tro${places[p - 1] ? "" : " zero"}`}>
          <Trophy place={p} size={size} />
          <b>{places[p - 1]}</b>
        </span>
      ))}
    </span>
  );
}
