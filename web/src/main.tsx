import { createRoot } from "react-dom/client";
import App from "./App";
import { initAuth } from "./auth";
import "./styles.css";

initAuth();
createRoot(document.getElementById("root")!).render(<App />);
