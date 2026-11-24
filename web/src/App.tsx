import {
  useCallback,
  useEffect,
  useMemo,
  useReducer,
  useRef,
  useState,
} from "react";
import { MpSocket } from "./api/socket";
import { saveCreds } from "./auth";
import { initialState, reducer } from "./state";
import { StoreCtx, type Store } from "./store";
import Header from "./components/Header";
import LoginDialog from "./components/LoginDialog";
import Overview from "./components/Overview";
import LobbyView from "./components/LobbyView";

function useHashRoute(): string {
  const [hash, setHash] = useState(window.location.hash);
  useEffect(() => {
    const f = () => setHash(window.location.hash);
    window.addEventListener("hashchange", f);
    return () => window.removeEventListener("hashchange", f);
  }, []);
  return hash;
}

interface Toast {
  id: number;
  text: string;
}

export default function App() {
  const [state, dispatch] = useReducer(reducer, initialState);
  const [toasts, setToasts] = useState<Toast[]>([]);
  const [showLogin, setShowLogin] = useState(false);
  const toastId = useRef(0);
  const hash = useHashRoute();

  const socketRef = useRef<MpSocket | null>(null);
  if (!socketRef.current) socketRef.current = new MpSocket();
  const socket = socketRef.current;

  useEffect(() => {
    socket.onmessage = (m) => {
      if (m.t === "hello") dispatch({ type: "hello", feed: m.feed });
      else if (m.t === "snapshot") dispatch({ type: "snapshot", snap: m });
      else if (m.t === "feed")
        dispatch({ type: "feed", lobby: m.lobby, entry: m.entry });
    };
    socket.onstatus = (s) => dispatch({ type: "status", status: s });
    socket.onyou = (you) => dispatch({ type: "you", you });
    socket.onnotice = (text) => toast(text);
    socket.start();
    return () => socket.stop();
  }, [socket]);

  const toast = useCallback((text: string) => {
    const id = ++toastId.current;
    setToasts((t) => [...t, { id, text }]);
    setTimeout(() => setToasts((t) => t.filter((x) => x.id !== id)), 4500);
  }, []);

  const auth = useCallback(
    async (name: string | null, token: string | null) => {
      const r = await socket.auth(name, token);
      if (r.ok && r.you) {
        saveCreds({ name: r.you.name, token });
        dispatch({ type: "you", you: r.you });
      }
      return r;
    },
    [socket],
  );

  const logout = useCallback(() => {
    saveCreds({ name: null, token: null });
    socket
      .auth(null, null)
      .then((r) => {
        if (r.you) dispatch({ type: "you", you: r.you });
      })
      .catch(() => {});
  }, [socket]);

  const store: Store = useMemo(
    () => ({ state, dispatch, socket, toast, auth, logout }),
    [state, socket, toast, auth, logout],
  );

  const m = /^#\/lobby\/([A-Z]+)/.exec(hash);

  return (
    <StoreCtx.Provider value={store}>
      <div className="app">
        <Header onLogin={() => setShowLogin(true)} />
        {m ? <LobbyView code={m[1]} /> : <Overview />}
        {showLogin && <LoginDialog onClose={() => setShowLogin(false)} />}
        <div className="toasts">
          {toasts.map((t) => (
            <div key={t.id} className="toast">
              {t.text}
            </div>
          ))}
        </div>
      </div>
    </StoreCtx.Provider>
  );
}
