import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import ReactDOM from "react-dom/client";

import { ProfilesApp } from "@/profiles/App";
import "@/styles/index.css";

const queryClient = new QueryClient();
const rootElement = document.getElementById("profiles-root");

if (!rootElement) {
  throw new Error("Profiles root is missing");
}

ReactDOM.createRoot(rootElement).render(
  <QueryClientProvider client={queryClient}>
    <ProfilesApp />
  </QueryClientProvider>,
);
