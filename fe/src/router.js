/** Minimal History API router (no deps). */

export function normalizePath(pathname) {
  const p = pathname.replace(/\/+$/, "") || "/";
  return p === "" ? "/" : p;
}

export function getPath() {
  return normalizePath(window.location.pathname);
}

/**
 * @param {string} path — e.g. `/` (store) or `/cart`
 * @param {string} [search] — optional query without leading `?`, e.g. `next=/admin`
 */
export function navigate(path, search) {
  const next = normalizePath(path);
  const q =
    search == null || search === ""
      ? ""
      : String(search).startsWith("?")
        ? String(search)
        : `?${String(search)}`;
  const url = (next === "/" ? "/" : next) + q;
  const cur = `${window.location.pathname}${window.location.search}`;
  if (url === cur) {
    window.dispatchEvent(new CustomEvent("routechange", { detail: { path: next } }));
    return;
  }
  history.pushState(null, "", url);
  window.dispatchEvent(new CustomEvent("routechange", { detail: { path: next } }));
}

/**
 * Same as navigate but uses replaceState (e.g. canonical `/products` → `/` without extra history entry).
 */
export function navigateReplace(path, search) {
  const next = normalizePath(path);
  const q =
    search == null || search === ""
      ? ""
      : String(search).startsWith("?")
        ? String(search)
        : `?${String(search)}`;
  const url = (next === "/" ? "/" : next) + q;
  history.replaceState(null, "", url);
  window.dispatchEvent(new CustomEvent("routechange", { detail: { path: next } }));
}

/**
 * @param {(path: string) => void} onRoute
 */
export function initRouter(onRoute) {
  const run = () => onRoute(getPath());
  window.addEventListener("popstate", run);
  window.addEventListener("routechange", run);
  run();
}
