import { createRoot } from "react-dom/client";
import App from "./App";
import { initAuth } from "./auth";
import "@fontsource/barlow-condensed/600.css";
import "@fontsource/barlow-condensed/600-italic.css";
import "@fontsource/barlow-condensed/700.css";
import "@fontsource/barlow-condensed/700-italic.css";
import "@fontsource/montserrat/400.css";
import "@fontsource/montserrat/600.css";
import "@fontsource/montserrat/700.css";
import "@fontsource/montserrat/800.css";
import "@fontsource/baloo-2/800.css";
import "./styles.css";

initAuth();
createRoot(document.getElementById("root")!).render(<App />);
