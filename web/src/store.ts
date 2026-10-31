import { createContext, useContext, type Dispatch } from "react";
import type { AppState, Action } from "./state";
import type { MpSocket } from "./api/socket";
import type { ReplyMsg } from "./types";

export interface Store {
  state: AppState;
  dispatch: Dispatch<Action>;
  socket: MpSocket;
  toast: (text: string) => void;
  auth: (name: string | null, token: string | null) => Promise<ReplyMsg>;
  logout: () => void;
}

export const StoreCtx = createContext<Store>(null as unknown as Store);
export const useStore = () => useContext(StoreCtx);
