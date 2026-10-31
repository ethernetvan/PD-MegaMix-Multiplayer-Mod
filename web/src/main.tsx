import { createRoot } from "react-dom/client";
import App from "./App";
import { initAuth } from "./auth";
import "@fontsource/barlow-condensed/600.css";
import "@fontsource/barlow-condensed/600-italic.css";
import "@fontsource/barlow-condensed/700.css";
import "@fontsource/barlow-condensed/700-italic.css";
import "@fontsource/nunito/400.css";
import "@fontsource/nunito/700.css";
import "@fontsource/nunito/800.css";
import "./styles.css";

initAuth();
createRoot(document.getElementById("root")!).render(<App />);
