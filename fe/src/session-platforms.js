import { Platform } from "./access-model.js";

const STORAGE_KEY = "m4_session_platforms";
/** Persisted choice for which access-model slice to fetch first (mirrors session platforms). */
export const ACCESS_PLATFORM_STORAGE_KEY = "m4_access_platform";

/**
 * Session-facing platforms: storefront vs back-office chrome and router glue.
 * Anonymous and customers: WEB only. Staff: WEB + ADMIN (same browser can use store + desks).
 * Future: server may send `platforms` on auth/me; merge here when present.
 *
 * @param {object | null} user
 * @param {boolean} hasToken
 * @param {(role: string) => boolean} isStaffRole
 * @returns {readonly string[]}
 */
export function computeSessionPlatforms(user, hasToken, isStaffRole) {
  if (!hasToken || !user) return Object.freeze([Platform.WEB]);
  if (isStaffRole(String(user.role || ""))) {
    return Object.freeze([Platform.WEB, Platform.ADMIN]);
  }
  return Object.freeze([Platform.WEB]);
}

/**
 * Persist platforms for ``isSpaPathNavigable`` and future UI without re-fetching /me.
 * @param {object | null} user
 * @param {boolean} hasToken
 * @param {(role: string) => boolean} isStaffRole
 */
export function syncSessionPlatformsFromUser(user, hasToken, isStaffRole) {
  const pl = computeSessionPlatforms(user, hasToken, isStaffRole);
  try {
    sessionStorage.setItem(STORAGE_KEY, JSON.stringify([...pl]));
  } catch {
    /* private mode */
  }
  try {
    localStorage.setItem(ACCESS_PLATFORM_STORAGE_KEY, [...pl].join(","));
  } catch {
    /* private mode */
  }
  return pl;
}

/** @returns {string[]} */
export function readSessionPlatforms() {
  try {
    const raw = sessionStorage.getItem(STORAGE_KEY);
    if (!raw) return [Platform.WEB];
    const p = JSON.parse(raw);
    if (!Array.isArray(p) || !p.length) return [Platform.WEB];
    return p.map(String);
  } catch {
    return [Platform.WEB];
  }
}

export function sessionHasAdminPlatform() {
  return readSessionPlatforms().includes(Platform.ADMIN);
}

/** Primary platform for access-model fetches (defaults to WEB). */
export function readAccessPlatformPreference() {
  try {
    const raw = localStorage.getItem(ACCESS_PLATFORM_STORAGE_KEY);
    if (raw && String(raw).trim()) return String(raw).trim().toUpperCase();
  } catch {
    /* ignore */
  }
  const fromSession = readSessionPlatforms();
  if (fromSession.includes(Platform.ADMIN) && fromSession.includes(Platform.WEB)) return `${Platform.WEB},${Platform.ADMIN}`;
  if (fromSession.includes(Platform.ADMIN)) return Platform.ADMIN;
  return Platform.WEB;
}
