/** JWT for Flask /api/auth/* — see docs/AUTH_JWT.md */
export const JWT_STORAGE_KEY = "m4_jwt_token";

export function getToken() {
  try {
    return localStorage.getItem(JWT_STORAGE_KEY) || "";
  } catch {
    return "";
  }
}

export function setToken(token) {
  try {
    if (token) localStorage.setItem(JWT_STORAGE_KEY, token);
    else localStorage.removeItem(JWT_STORAGE_KEY);
  } catch {
    /* ignore */
  }
}

export function authHeaders() {
  const t = getToken().trim();
  if (!t) return {};
  return { Authorization: `Bearer ${t}` };
}

/** Anonymous storefront carts — sent as ``X-Guest-Cart-Id`` when not logged in as ``USER``. */
export const GUEST_CART_ID_KEY = "m4_guest_cart_id";

export function ensureGuestCartId() {
  try {
    let id = localStorage.getItem(GUEST_CART_ID_KEY);
    if (!id) {
      id = crypto.randomUUID();
      localStorage.setItem(GUEST_CART_ID_KEY, id);
    }
    return id;
  } catch {
    return "";
  }
}

/** Headers for ``/api/store/cart`` — staff JWT must not send guest id (server uses role). */
export function storeCartHeaders() {
  const base = { ...authHeaders() };
  if (!getToken().trim()) {
    const g = ensureGuestCartId();
    if (g) base["X-Guest-Cart-Id"] = g;
  }
  return base;
}

export async function authRegister(apiBase, email, password, name, description) {
  const body = { email, password };
  if (name && String(name).trim()) body.name = String(name).trim();
  if (description && String(description).trim()) body.description = String(description).trim();
  const r = await fetch(`${apiBase}/api/auth/register`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  const data = await r.json().catch(() => ({}));
  if (!r.ok) {
    const msg = [data.error || r.statusText, data.hint].filter(Boolean).join(" — ");
    throw new Error(msg);
  }
  return data;
}

export async function authLogin(apiBase, email, password) {
  const r = await fetch(`${apiBase}/api/auth/login`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ email, password }),
  });
  const data = await r.json().catch(() => ({}));
  if (!r.ok) {
    const msg = [data.error || r.statusText, data.hint].filter(Boolean).join(" — ");
    throw new Error(msg);
  }
  if (data.access_token) setToken(data.access_token);
  return data;
}

export function authLogout() {
  setToken("");
}

export async function authMe(apiBase) {
  const r = await fetch(`${apiBase}/api/auth/me`, {
    headers: { ...authHeaders(), "Content-Type": "application/json" },
  });
  const data = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(data.error || r.statusText);
  return data;
}
