import type { FeedEntry } from "./types";

export function feedText(e: FeedEntry): string {
  switch (e.kind) {
    case "event":
      switch (e.ev) {
        case "joined":
          return `${e.name} joined`;
        case "left":
          return `${e.name} left`;
        case "leader":
          return `${e.name} is now picking`;
        case "disconnected":
          return `${e.name} disconnected`;
        case "reconnected":
          return `${e.name} reconnected`;
        case "combo":
          return `${e.name} hit a ${e.value} note combo`;
        case "low_life":
          return `${e.name}'s life is low (${e.value})`;
        default:
          return `${e.name ?? "?"}: ${e.ev}`;
      }
    case "sel":
      return `Selected ${e.name ?? "?"} (PV ${e.pv})`;
    case "launch":
      return `Round ${e.round} launching — ${e.name ?? "?"} (PV ${e.pv})`;
    case "start":
      return `Round ${e.round} started`;
    case "pause":
      return `Paused by ${e.name ?? "?"}`;
    case "resume":
      return `Resumed by ${e.name ?? "?"}`;
    case "pause_vote":
      return `${e.vote} vote: ${e.agreed}/${e.need} agreed${e.expired ? " (expired)" : ""}`;
    case "results":
      return `Round ${e.round} results are in`;
    case "round_end":
      return `Round ${e.round} ended${e.name ? ` — ${e.name} picks next` : ""}`;
    case "web_action":
      if (e.op === "kick") return `${e.by ?? "web"} kicked ${e.name}`;
      if (e.op === "set_leader")
        return `${e.by ?? "web"} made ${e.name} the picker`;
      if (e.op === "set_pick_mode")
        return `${e.by ?? "web"} set pick mode to ${e.mode}${e.name ? ` (${e.name})` : ""}`;
      return `${e.by ?? "web"}: ${e.op}`;
    default:
      return e.kind;
  }
}
