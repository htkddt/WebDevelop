import { defineConfig } from "vite";

/** Flask bind port (must match `M4ENGINE_SERVER_PORT` / server stderr when 5000 is taken). */
const FLASK_PORT =
  process.env.M4ENGINE_SERVER_PORT ||
  process.env.VITE_FLASK_PROXY_PORT ||
  "5000";
const FLASK_TARGET = `http://127.0.0.1:${FLASK_PORT}`;

/** Dev: geo import with `embed=1` can call Ollama per row; default proxy timeout (~2m) causes "socket hang up". */
const PROXY_MS = Number(process.env.VITE_PROXY_TIMEOUT_MS || 86400000);

export default defineConfig({
  plugins: [
    {
      name: "spa-fallback",
      configureServer(server) {
        server.middlewares.use((req, _res, next) => {
          const raw = req.url || "";
          if (raw.startsWith("/api") || raw.startsWith("/@") || raw.startsWith("/node_modules")) {
            return next();
          }
          const pathOnly = raw.split("?")[0] || "";
          if (pathOnly.match(/\.[a-zA-Z0-9]+$/)) return next();
          if (pathOnly === "/" || pathOnly === "") return next();
          req.url = "/index.html";
          next();
        });
      },
    },
  ],
  server: {
    port: 8000,
    strictPort: true,
    /** `true` = listen on all addresses; `127.0.0.1` only also works with http://localhost:8000/ on most systems */
    host: true,
    /** Forward /api/* to Flask so same-origin fetches work in dev (see main.js API_BASE). */
    proxy: {
      "/api": {
        target: FLASK_TARGET,
        changeOrigin: true,
        timeout: PROXY_MS,
        proxyTimeout: PROXY_MS,
        /** Let SSE (`/api/chat/stream`) flush chunk-by-chunk; avoid fixed Content-Length buffering. */
        configure(proxy) {
          proxy.on("proxyRes", (proxyRes, req) => {
            const url = req.url || "";
            if (url.includes("stream")) {
              delete proxyRes.headers["content-length"];
            }
          });
        },
      },
    },
  },
});
