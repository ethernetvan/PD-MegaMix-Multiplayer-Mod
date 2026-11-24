const MODE_TAGS: Record<string, string> = {
  auto: "AUTO",
  pro: "AUTO PRO",
  "97": "AUTO 97",
  streak: "AUTO STREAK",
  human: "AUTO HUMAN",
  clutch: "AUTO CLUTCH",
  tie: "AUTO TIE",
};

export function autoLabel(pct?: number | null, mode?: string | null): string {
  if (mode && MODE_TAGS[mode]) return MODE_TAGS[mode];
  return pct == null || pct >= 100 ? "AUTO" : `AUTO ${Math.round(pct)}%`;
}
