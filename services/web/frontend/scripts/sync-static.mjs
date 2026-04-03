import { copyFile, mkdir } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const frontendDir = path.resolve(__dirname, "..");
const distDir = path.join(frontendDir, "dist", "react");
const staticDir = path.resolve(frontendDir, "..", "app", "static", "react");

await mkdir(staticDir, { recursive: true });

for (const name of ["device-detail.js", "device-detail.css"]) {
  await copyFile(path.join(distDir, name), path.join(staticDir, name));
}

