import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import ReactDOM from "react-dom/client";

import { DeviceDetailApp } from "@/device-detail/App";
import type { DeviceDetailBootstrap } from "@/device-detail/types";
import "@/styles/index.css";

declare global {
  interface Window {
    __BREWESP_DEVICE_DETAIL__?: DeviceDetailBootstrap;
  }
}

const queryClient = new QueryClient();
const rootElement = document.getElementById("device-detail-root");
const bootstrap = window.__BREWESP_DEVICE_DETAIL__;

if (!rootElement || !bootstrap) {
  throw new Error("Device detail bootstrap is missing");
}

ReactDOM.createRoot(rootElement).render(
  <QueryClientProvider client={queryClient}>
    <DeviceDetailApp {...bootstrap} />
  </QueryClientProvider>,
);
