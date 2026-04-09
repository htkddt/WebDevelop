/**
 * Access model: **Platform → Module → Page → UI methods**.
 *
 * First layer for product structure; later map ABAC policies / roles to ``pageKey`` + ``uiMethods``.
 *
 * @see docs/ACCESS_MODEL.md
 */

/** @typedef {"WEB" | "ADMIN"} PlatformId */
/** @typedef {string} ModuleId */

export const Platform = Object.freeze({
  WEB: "WEB",
  ADMIN: "ADMIN",
});

/** Modules under each platform (product areas). */
export const Module = Object.freeze({
  Product: "Product",
  Cart: "Cart",
  /** ADMIN shell (desks) */
  Admin: "ADMIN",
  /** Bot / ops workspace */
  BotConfig: "BOT_CONFIG",
});

/**
 * Web routes (Vite pathname). Admin shell is one route ``/admin``; desks use ``adminSection``.
 * @type {ReadonlyArray<{
 *   path: string;
 *   platform: PlatformId;
 *   module: ModuleId;
 *   pageKey: string;
 *   uiMethods: string[];
 *   label?: string;
 * }>}
 */
export const WEB_PAGES = Object.freeze([
  {
    path: "/productions",
    platform: Platform.WEB,
    module: Module.Product,
    pageKey: "web.product.catalog",
    label: "Home",
    audiences: Object.freeze(["anonymous", "authenticated"]),
    uiMethods: Object.freeze(["viewCatalog", "filterCategory", "refreshCatalog", "openProduct"]),
  },
  {
    path: "/cart",
    platform: Platform.WEB,
    module: Module.Cart,
    pageKey: "web.cart.checkout",
    label: "Cart",
    audiences: Object.freeze(["anonymous", "authenticated"]),
    uiMethods: Object.freeze([
      "viewCart",
      "saveCart",
      "checkout",
      "viewOrderHistory",
      "confirmDelivery",
    ]),
  },
  {
    path: "/register",
    platform: Platform.WEB,
    module: Module.Cart,
    pageKey: "web.account.register",
    label: "Register",
    audiences: Object.freeze(["anonymous", "authenticated"]),
    uiMethods: Object.freeze(["submitRegister"]),
  },
]);

/**
 * Admin desks (hash section after ``/admin#``). All share path ``/admin``, platform ADMIN, module Admin.
 * Finance subgroup is still module **Admin** with pageKey prefix ``admin.finance.*``.
 * @type {ReadonlyArray<{
 *   adminSection: string;
 *   platform: PlatformId;
 *   module: ModuleId;
 *   pageKey: string;
 *   uiMethods: string[];
 *   label?: string;
 *   abacArea?: string;
 *   policyResource?: string;
 *   relatedPolicyIds?: string[];
 * }>}
 */
export const ADMIN_DESK_PAGES = Object.freeze([
  {
    adminSection: "department",
    platform: Platform.ADMIN,
    module: Module.Admin,
    pageKey: "admin.org.department",
    label: "Department",
    abacArea: "org",
    uiMethods: Object.freeze(["listDepartments", "createDepartment", "editDepartment"]),
  },
  {
    adminSection: "users",
    platform: Platform.ADMIN,
    module: Module.Admin,
    pageKey: "admin.hr.users",
    label: "Users",
    abacArea: "hr_abac",
    policyResource: "user",
    relatedPolicyIds: Object.freeze(["pol_user_manage"]),
    uiMethods: Object.freeze(["listUsers", "createUser", "changeRole", "changeDepartment", "assignPolicies"]),
  },
  {
    adminSection: "policies",
    platform: Platform.ADMIN,
    module: Module.Admin,
    pageKey: "admin.abac.policies",
    label: "Policies",
    abacArea: "hr_abac",
    policyResource: "policy",
    relatedPolicyIds: Object.freeze(["pol_policy_manage"]),
    uiMethods: Object.freeze(["listPolicies", "createPolicy", "reloadCatalog"]),
  },
  {
    adminSection: "bot_c_lib",
    platform: Platform.ADMIN,
    module: Module.BotConfig,
    pageKey: "admin.bot.c_lib_options",
    label: "Bot · c-lib options",
    abacArea: "bot",
    uiMethods: Object.freeze(["listOptionKeys", "editOptions", "saveToDb"]),
  },
  {
    adminSection: "finance_charts",
    platform: Platform.ADMIN,
    module: Module.Admin,
    pageKey: "admin.finance.charts",
    label: "Finance · Charts & export",
    abacArea: "finance",
    policyResource: "cart",
    relatedPolicyIds: Object.freeze(["pol_cart_finance"]),
    uiMethods: Object.freeze(["viewSummary", "refreshCharts", "exportCsv", "filterRange"]),
  },
  {
    adminSection: "finance_orders",
    platform: Platform.ADMIN,
    module: Module.Admin,
    pageKey: "admin.finance.orders",
    label: "Finance · Orders",
    abacArea: "finance",
    policyResource: "cart",
    relatedPolicyIds: Object.freeze(["pol_cart_finance"]),
    uiMethods: Object.freeze(["listOrders", "transitionOrder", "filterStatus"]),
  },
  {
    adminSection: "storage",
    platform: Platform.ADMIN,
    module: Module.Admin,
    pageKey: "admin.storage.catalog",
    label: "Storage",
    abacArea: "storage",
    uiMethods: Object.freeze(["listProducts", "createProduct", "editStock", "bulkImport", "manageCategories"]),
  },
  {
    adminSection: "orders",
    platform: Platform.ADMIN,
    module: Module.Admin,
    pageKey: "admin.ops.orders",
    label: "Orders",
    abacArea: "ops",
    uiMethods: Object.freeze(["listOrders", "transitionOrder", "filterStatus"]),
  },
  {
    adminSection: "delivery",
    platform: Platform.ADMIN,
    module: Module.Admin,
    pageKey: "admin.ops.delivery",
    label: "Delivery",
    abacArea: "ops",
    uiMethods: Object.freeze(["listOrders", "markShipped"]),
  },
  {
    adminSection: "sale",
    platform: Platform.ADMIN,
    module: Module.Admin,
    pageKey: "admin.sale.catalog",
    label: "Sale",
    abacArea: "sale",
    uiMethods: Object.freeze(["viewProductList"]),
  },
]);

export const ADMIN_BOT_PAGE = Object.freeze({
  path: "/bot",
  platform: Platform.ADMIN,
  module: Module.BotConfig,
  pageKey: "admin.bot.workspace",
  label: "Bot status",
  abacArea: "bot",
  /** Must match seeded ``pol_bot_use`` (or override in Mongo ``access_pages`` admin_bot). Empty = no policy gate. */
  relatedPolicyIds: Object.freeze(["pol_bot_use"]),
  uiMethods: Object.freeze(["viewStats", "viewHistory", "sendChat", "geoImport"]),
});

/** Overlays from ``GET /api/meta/access-model`` (Mongo ``access_pages``); null = use embedded tables. */
let _webPages = null;
let _adminDeskPages = null;
let _adminBotPage = null;
/** @type {Set<string> | null} */
let _staffShellPathsSet = null;

/** From API ``platforms``; drives WEB module order in chrome. */
let _platformsCatalog = null;

/** Server ``webNav`` / ``webMenu`` from ``GET /api/meta/access-model?platform=web``. */
let _webNav = null;
let _webMenu = null;

const DEFAULT_PLATFORMS_CATALOG = Object.freeze({
  WEB: Object.freeze({ modules: Object.freeze(["Product", "Cart"]) }),
  ADMIN: Object.freeze({ modules: Object.freeze(["ADMIN", "BOT_CONFIG"]) }),
});

/**
 * Platform → ordered modules (from ``GET /api/meta/access-model`` ``platforms``).
 * Anonymous / customer sessions use **WEB** module order for the storefront bar.
 */
export function getPlatformsCatalog() {
  return _platformsCatalog ?? DEFAULT_PLATFORMS_CATALOG;
}

function normalizePath(p) {
  const x = String(p || "").replace(/\/+$/, "") || "/";
  return x === "" ? "/" : x;
}

/**
 * When a ``webPages`` document omits ``mainNavAudiences``, derive top-bar visibility from path.
 * Route access still follows ``audiences``; chrome can be stricter.
 * @param {string} path
 * @returns {string[]}
 */
export function defaultMainNavAudiencesForPath(path) {
  const p = normalizePath(path);
  if (p === "/productions") return ["anonymous", "authenticated"];
  if (p === "/cart") return ["anonymous", "authenticated"];
  if (p === "/register") return [];
  return ["anonymous", "authenticated"];
}

/**
 * Account-menu links for WEB pages when ``popoverAudiences`` is omitted.
 * @param {string} path
 * @returns {string[]}
 */
export function defaultPopoverAudiencesForPath(path) {
  const p = normalizePath(path);
  if (p === "/register") return ["anonymous"];
  return ["anonymous", "authenticated"];
}

/** Landing ``/`` is not a separate page in chrome; same shell as ``/productions``. */
const WEB_CHROME_EXCLUDED_PATHS = new Set(["/"]);

const DEFAULT_STAFF_SHELL_PATHS = ["/admin", "/bot"];

function getStaffShellPathsSet() {
  if (_staffShellPathsSet && _staffShellPathsSet.size > 0) return _staffShellPathsSet;
  return new Set(DEFAULT_STAFF_SHELL_PATHS.map((x) => normalizePath(x)));
}

/**
 * WEB platform paths allowed for this session (from ``audiences`` on each web page).
 * @param {boolean} hasToken ``true`` if JWT present (customer or staff).
 * @returns {Set<string>}
 */
export function getAllowedWebPathsForSession(hasToken) {
  const role = hasToken ? "authenticated" : "anonymous";
  const set = new Set();
  for (const page of getWebPages()) {
    const raw = page.audiences;
    const aud = Array.isArray(raw) ? raw : ["anonymous", "authenticated"];
    if (aud.includes(role)) set.add(normalizePath(page.path));
  }
  return set;
}

/**
 * Whether the SPA may show this pathname (WEB platform rules + staff shell paths).
 * Staff shell paths (``/admin``, ``/bot``) require ``Platform.ADMIN`` in ``opts.sessionPlatforms``
 * when that option is passed (from session storage), so customers do not treat them as normal
 * storefront routes; gates still enforce login/role when they reach the shell.
 *
 * @param {string} path
 * @param {boolean} hasToken
 * @param {{ sessionPlatforms?: string[] }=} opts
 */
export function isSpaPathNavigable(path, hasToken, opts) {
  const p = normalizePath(path);
  if (getStaffShellPathsSet().has(p)) {
    if (!hasToken) return true;
    const pl = opts && opts.sessionPlatforms;
    if (Array.isArray(pl) && !pl.includes(Platform.ADMIN)) return false;
    return true;
  }
  return getAllowedWebPathsForSession(hasToken).has(p);
}

export function getWebPages() {
  return _webPages ?? WEB_PAGES;
}

/**
 * @param {object} page from ``getWebPages()``
 * @param {{ hasToken: boolean; isStaff: boolean }} session
 */
export function webPageVisibleInMainNav(page, session) {
  const path = normalizePath(page.path);
  const raw = page.mainNavAudiences;
  const aud = Array.isArray(raw) ? raw : defaultMainNavAudiencesForPath(path);
  const sessionRole = session.hasToken ? "authenticated" : "anonymous";
  if (!aud.includes(sessionRole)) return false;
  if (session.isStaff && (path === "/cart" || path === "/register")) return false;
  return true;
}

/**
 * @param {object} page from ``getWebPages()``
 * @param {{ hasToken: boolean }} session
 */
export function webPageVisibleInUserPopover(page, session) {
  const path = normalizePath(page.path);
  const raw = page.popoverAudiences;
  const aud = Array.isArray(raw) ? raw : defaultPopoverAudiencesForPath(path);
  const sessionRole = session.hasToken ? "authenticated" : "anonymous";
  return aud.includes(sessionRole);
}

function normalizeNavEntry(raw) {
  const path = normalizePath(raw.path ?? raw.link ?? raw.nav ?? "/");
  return {
    name: String(raw.name ?? "").trim() || path,
    nav: path,
    link: path,
    path,
    pageKey: raw.pageKey,
    module: raw.module,
  };
}

/**
 * Client-side replica of server ``build_web_nav_entries`` (offline / missing ``webNav``).
 */
export function buildWebNavFromModel() {
  const order = [...(getPlatformsCatalog().WEB?.modules || [])].map(String);
  const pages = getWebPages().filter((p) => String(p.platform) === Platform.WEB);
  const byMod = new Map();
  for (const p of pages) {
    const m = String(p.module || "");
    if (!byMod.has(m)) byMod.set(m, []);
    byMod.get(m).push(p);
  }
  for (const lst of byMod.values()) {
    lst.sort((a, b) => normalizePath(a.path).localeCompare(normalizePath(b.path)));
  }
  const out = [];
  for (const mod of order) {
    const cands = byMod.get(mod) || [];
    const p = cands[0];
    if (!p) continue;
    const path = normalizePath(p.path);
    out.push({
      name: p.label || path,
      nav: path,
      link: path,
      path,
      pageKey: p.pageKey,
      module: p.module,
    });
  }
  return out;
}

/** Client replica of server ``build_web_menu_entries``. */
export function buildWebMenuFromModel() {
  return getWebPages()
    .filter(
      (p) => String(p.platform) === Platform.WEB && !WEB_CHROME_EXCLUDED_PATHS.has(normalizePath(p.path)),
    )
    .sort((a, b) => normalizePath(a.path).localeCompare(normalizePath(b.path)))
    .map((p) => {
      const path = normalizePath(p.path);
      return {
        name: p.label || path,
        nav: path,
        link: path,
        path,
        pageKey: p.pageKey,
        module: p.module,
      };
    });
}

/**
 * Top bar items: prefer API ``webNav``, else ``buildWebNavFromModel``. Filtered by session vs ``webPages``.
 *
 * @param {{ hasToken: boolean; isStaff: boolean }} session
 */
export function getWebNavEntriesForSession(session) {
  const raw = Array.isArray(_webNav) && _webNav.length ? _webNav : buildWebNavFromModel();
  return raw.filter((entry) => {
    const page = getWebPages().find((wp) => wp.pageKey === entry.pageKey);
    if (!page) return true;
    return webPageVisibleInMainNav(page, session);
  });
}

/**
 * Account menu: prefer API ``webMenu``, else ``buildWebMenuFromModel``.
 *
 * @param {{ hasToken: boolean }} session
 */
export function getWebMenuEntriesForSession(session) {
  const raw = Array.isArray(_webMenu) && _webMenu.length ? _webMenu : buildWebMenuFromModel();
  return raw.filter((entry) => {
    const page = getWebPages().find((wp) => wp.pageKey === entry.pageKey);
    if (!page) return true;
    return webPageVisibleInUserPopover(page, session);
  });
}

export function getAdminDeskPages() {
  return _adminDeskPages ?? ADMIN_DESK_PAGES;
}

export function getAdminBotPage() {
  const base = ADMIN_BOT_PAGE;
  if (!_adminBotPage) return base;
  return {
    ...base,
    ..._adminBotPage,
    uiMethods: Array.isArray(_adminBotPage.uiMethods) ? [..._adminBotPage.uiMethods] : [...base.uiMethods],
    relatedPolicyIds: Array.isArray(_adminBotPage.relatedPolicyIds)
      ? [..._adminBotPage.relatedPolicyIds]
      : [...(base.relatedPolicyIds || [])],
  };
}

const _POLICY_BUILDER_PLATFORM_LABELS = {
  WEB: "Web storefront",
  ADMIN: "Admin & staff",
  MOBILE: "Mobile",
  PARTNER: "Partner",
};

const _ORDER_STATUSES_FOR_POLICY_UI = Object.freeze([
  "pending_confirmation",
  "confirmed",
  "rejected",
  "packed",
  "shipped",
  "delivered",
]);

function _slugPolicyPart(s, maxLen = 24) {
  const x = String(s || "")
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "_")
    .replace(/^_|_$/g, "");
  const t = x.slice(0, maxLen).replace(/_$/g, "") || "x";
  return t;
}

function _suggestPolicyIdFromPageKey(pageKey) {
  const base = `pol_${_slugPolicyPart(String(pageKey || "").replace(/\./g, "_"), 56)}`;
  return /^[a-zA-Z]/.test(base) ? base.slice(0, 63) : `pol_${base}`.slice(0, 63);
}

function _humanPolicyName(platformId, module, featureLabel) {
  const plab = _POLICY_BUILDER_PLATFORM_LABELS[platformId] || platformId;
  return `${plab} › ${module} › ${featureLabel}`.trim();
}

function _uiMethodsToSuggestedActions(uiMethods) {
  const out = [];
  const seen = new Set();
  for (const raw of uiMethods || []) {
    const m = String(raw || "").trim();
    if (!m) continue;
    const ml = m.toLowerCase();
    let a = "read";
    if (ml.startsWith("view") || ml.startsWith("open") || ml.startsWith("refresh") || ml.startsWith("filter")) a = "read";
    else if (ml.startsWith("list")) a = "list";
    else if (ml.includes("export")) a = "export";
    else if (ml.startsWith("delete") || ml.startsWith("remove")) a = "delete";
    else if (
      [
        "create",
        "add",
        "submit",
        "save",
        "edit",
        "update",
        "assign",
        "transition",
        "mark",
        "confirm",
        "checkout",
        "bulk",
        "manage",
        "change",
        "reload",
      ].some((p) => ml.startsWith(p))
    )
      a = "update";
    if (!seen.has(a)) {
      seen.add(a);
      out.push(a);
    }
  }
  return out.length ? out : ["read"];
}

function _suggestResourceForFeature(pageKey, kind, policyResourceHint) {
  if (policyResourceHint && String(policyResourceHint).trim()) return String(policyResourceHint).trim();
  const pk = String(pageKey || "").toLowerCase();
  if (pk.includes("cart") || pk.includes("finance")) return "cart";
  if (pk.includes("order") || pk.includes("ops") || pk.includes("delivery") || pk.includes("sale")) return "order";
  if (pk.includes("user") || pk.includes("hr")) return "user";
  if (pk.includes("polic") || pk.includes("abac")) return "policy";
  if (pk.includes("product") || pk.includes("catalog") || pk.includes("storage")) return "product";
  if (pk.includes("department") || pk.includes("org")) return "department";
  if (pk.includes("bot")) return "bot_config";
  return "resource";
}

/**
 * Fallback for policy builder when ``GET /api/admin/policies/builder-options`` is unavailable.
 * Uses hydrated access model in memory (same sources as the SPA router).
 */
export function buildPolicyBuilderClientCatalog() {
  const cat = getPlatformsCatalog();
  const platforms = Object.keys(cat)
    .map((id) => {
      const pid = String(id).toUpperCase();
      const mods = Array.isArray(cat[id]?.modules) ? [...cat[id].modules] : [];
      return {
        id: pid,
        label: _POLICY_BUILDER_PLATFORM_LABELS[pid] || pid,
        modules: mods.map(String),
      };
    })
    .sort((a, b) => a.id.localeCompare(b.id));

  const features = [];

  for (const wp of getWebPages()) {
    const pk = String(wp.pageKey || "");
    const plat = String(wp.platform || "WEB").toUpperCase();
    const mod = String(wp.module || "");
    const label = String(wp.label || pk || mod);
    const uim = Array.isArray(wp.uiMethods) ? [...wp.uiMethods] : [];
    features.push({
      pageKey: pk,
      label,
      platform: plat,
      module: mod,
      kind: "web",
      path: wp.path,
      uiMethods: uim,
      suggestedResource: _suggestResourceForFeature(pk, "web"),
      suggestedActions: _uiMethodsToSuggestedActions(uim),
      suggestedPolicyId: _suggestPolicyIdFromPageKey(pk),
      humanName: _humanPolicyName(plat, mod, label),
    });
  }

  for (const ad of getAdminDeskPages()) {
    const pk = String(ad.pageKey || "");
    const plat = String(ad.platform || "ADMIN").toUpperCase();
    const mod = String(ad.module || "ADMIN");
    const label = String(ad.label || pk);
    const uim = Array.isArray(ad.uiMethods) ? [...ad.uiMethods] : [];
    const hint = ad.policyResource;
    features.push({
      pageKey: pk,
      label,
      platform: plat,
      module: mod,
      kind: "admin_desk",
      adminSection: ad.adminSection,
      uiMethods: uim,
      suggestedResource: _suggestResourceForFeature(pk, "admin_desk", hint),
      suggestedActions: _uiMethodsToSuggestedActions(uim),
      suggestedPolicyId: _suggestPolicyIdFromPageKey(pk),
      humanName: _humanPolicyName(plat, mod, label),
    });
  }

  const bot = getAdminBotPage();
  if (bot && bot.pageKey) {
    const pk = String(bot.pageKey);
    const uim = Array.isArray(bot.uiMethods) ? [...bot.uiMethods] : [];
    const label = String(bot.label || "Bot");
    const mod = String(bot.module || "BOT_CONFIG");
    const rel = Array.isArray(bot.relatedPolicyIds) ? [...bot.relatedPolicyIds] : [];
    features.push({
      pageKey: pk,
      label,
      platform: "ADMIN",
      module: mod,
      kind: "admin_bot",
      path: bot.path,
      uiMethods: uim,
      ...(rel.length ? { relatedPolicyIds: rel } : {}),
      suggestedResource: _suggestResourceForFeature(pk, "admin_bot"),
      suggestedActions: _uiMethodsToSuggestedActions(uim),
      suggestedPolicyId: _suggestPolicyIdFromPageKey(pk),
      humanName: _humanPolicyName("ADMIN", mod, label),
    });
  }

  features.sort((a, b) =>
    `${a.platform}\0${a.module}\0${a.pageKey}`.localeCompare(`${b.platform}\0${b.module}\0${b.pageKey}`),
  );

  return {
    platforms,
    features,
    orderStatuses: [..._ORDER_STATUSES_FOR_POLICY_UI],
    hint: "Client catalog (fallback). Prefer server builder-options when logged in as HR/ADMIN.",
  };
}

function normWebPagesList(xs) {
  if (!Array.isArray(xs)) return [];
  return xs.map((x) => {
    const o = {
      ...x,
      uiMethods: Array.isArray(x.uiMethods) ? [...x.uiMethods] : [],
    };
    o.audiences = Array.isArray(x.audiences) ? [...x.audiences] : ["anonymous", "authenticated"];
    const pathNorm = normalizePath(String(o.path || "/"));
    o.mainNavAudiences = Array.isArray(x.mainNavAudiences)
      ? [...x.mainNavAudiences]
      : defaultMainNavAudiencesForPath(pathNorm);
    o.popoverAudiences = Array.isArray(x.popoverAudiences)
      ? [...x.popoverAudiences]
      : defaultPopoverAudiencesForPath(pathNorm);
    return o;
  });
}

function normAdminDeskList(xs) {
  if (!Array.isArray(xs)) return [];
  return xs.map((x) => {
    const o = {
      ...x,
      uiMethods: Array.isArray(x.uiMethods) ? [...x.uiMethods] : [],
    };
    if (Array.isArray(x.relatedPolicyIds)) o.relatedPolicyIds = [...x.relatedPolicyIds];
    return o;
  });
}

function mergePlatformsFromApi(platformsPartial) {
  if (!platformsPartial || typeof platformsPartial !== "object") return;
  const prev = _platformsCatalog ?? {
    WEB: { modules: [...DEFAULT_PLATFORMS_CATALOG.WEB.modules] },
    ADMIN: { modules: [...DEFAULT_PLATFORMS_CATALOG.ADMIN.modules] },
  };
  _platformsCatalog = {
    WEB: {
      modules: Array.isArray(platformsPartial.WEB?.modules)
        ? [...platformsPartial.WEB.modules]
        : [...prev.WEB.modules],
    },
    ADMIN: {
      modules: Array.isArray(platformsPartial.ADMIN?.modules)
        ? [...platformsPartial.ADMIN.modules]
        : [...prev.ADMIN.modules],
    },
  };
}

/**
 * Load access model: ``?platform=web`` (default) + optional ``?platform=admin`` when staff.
 *
 * @param {string} [apiBase]
 * @param {{ includeAdmin?: boolean }=} options Pass ``includeAdmin: true`` after auth when session has ADMIN.
 */
export async function hydrateAccessModelFromApi(apiBase = "", options = {}) {
  const base = String(apiBase || "").replace(/\/$/, "");
  const url = `${base}/api/meta/access-model`;
  const includeAdmin = !!options.includeAdmin;
  try {
    const rWeb = await fetch(`${url}?platform=web`);
    if (!rWeb.ok) throw new Error(rWeb.statusText || String(rWeb.status));
    const dataWeb = await rWeb.json();
    if (Array.isArray(dataWeb.webPages)) {
      _webPages = normWebPagesList(dataWeb.webPages);
    }
    if (Array.isArray(dataWeb.webNav) && dataWeb.webNav.length) {
      _webNav = dataWeb.webNav.map(normalizeNavEntry);
    } else {
      _webNav = null;
    }
    if (Array.isArray(dataWeb.webMenu) && dataWeb.webMenu.length) {
      _webMenu = dataWeb.webMenu.map(normalizeNavEntry);
    } else {
      _webMenu = null;
    }
    mergePlatformsFromApi(dataWeb.platforms);

    if (includeAdmin) {
      const rAd = await fetch(`${url}?platform=admin`);
      if (rAd.ok) {
        const dataAd = await rAd.json();
        if (Array.isArray(dataAd.adminDeskPages)) {
          _adminDeskPages = normAdminDeskList(dataAd.adminDeskPages);
        }
        if (dataAd.adminBotPage && typeof dataAd.adminBotPage === "object") {
          const ab = dataAd.adminBotPage;
          _adminBotPage = {
            ...ab,
            uiMethods: Array.isArray(ab.uiMethods) ? [...ab.uiMethods] : [],
            ...(Array.isArray(ab.relatedPolicyIds) ? { relatedPolicyIds: [...ab.relatedPolicyIds] } : {}),
          };
        }
        if (Array.isArray(dataAd.staffShellPaths) && dataAd.staffShellPaths.length) {
          _staffShellPathsSet = new Set(dataAd.staffShellPaths.map((x) => normalizePath(String(x))));
        }
        mergePlatformsFromApi(dataAd.platforms);
      }
    }

    return dataWeb;
  } catch (e) {
    console.warn("[access-model] hydrate failed, using embedded defaults:", e);
    _webPages = null;
    _adminDeskPages = null;
    _adminBotPage = null;
    _staffShellPathsSet = null;
    _platformsCatalog = null;
    _webNav = null;
    _webMenu = null;
    return null;
  }
}

/** @type {{ kind: "web"; def: typeof WEB_PAGES[number] } | { kind: "admin"; def: typeof ADMIN_DESK_PAGES[number] } | { kind: "admin_bot"; def: typeof ADMIN_BOT_PAGE } | null} */
let _currentContext = null;

/**
 * Resolve storefront route.
 * @param {string} path
 */
export function resolveWebPage(path) {
  const p = normalizePath(path);
  return getWebPages().find((r) => r.path === p) || null;
}

/**
 * Resolve admin desk by hash section key.
 * @param {string} adminSection
 */
export function resolveAdminDeskPage(adminSection) {
  const k = String(adminSection || "").trim();
  return getAdminDeskPages().find((r) => r.adminSection === k) || null;
}

/**
 * Call from router when pathname changes (web).
 * @param {string} path
 */
export function setWebAccessContextFromPath(path) {
  const p = normalizePath(path);
  if (p === "/admin") {
    _currentContext = null;
    return;
  }
  if (p === getAdminBotPage().path) {
    _currentContext = { kind: "admin_bot", def: getAdminBotPage() };
    return;
  }
  const def = resolveWebPage(p);
  _currentContext = def ? { kind: "web", def } : null;
}

/**
 * Call when ``/admin`` hash desk changes.
 * @param {string} adminSection
 */
export function setAdminAccessContextFromDesk(adminSection) {
  const def = resolveAdminDeskPage(adminSection);
  _currentContext = def ? { kind: "admin", def } : null;
}

/** Snapshot for debugging / future ABAC. */
export function getAccessContext() {
  return _currentContext;
}

/**
 * Flat descriptor for logging or sending to analytics.
 */
export function describeAccessContext() {
  const c = _currentContext;
  if (!c) return null;
  if (c.kind === "admin_bot") {
    const d = c.def;
    return {
      platform: d.platform,
      module: d.module,
      pageKey: d.pageKey,
      path: d.path,
      uiMethods: [...d.uiMethods],
    };
  }
  const d = c.def;
  return {
    platform: d.platform,
    module: d.module,
    pageKey: d.pageKey,
    path: "path" in d ? d.path : "/admin",
    adminSection: "adminSection" in d ? d.adminSection : undefined,
    uiMethods: [...d.uiMethods],
  };
}

/**
 * True if the user has at least one of the given policy ids (from ``/api/auth/me`` → ``policies``).
 * Empty or missing ``requiredIds`` → no policy requirement (allow).
 *
 * @param {unknown[]} userPolicies
 * @param {readonly string[] | string[] | null | undefined} requiredIds
 */
export function sessionSatisfiesRelatedPolicies(userPolicies, requiredIds) {
  if (!requiredIds || !requiredIds.length) return true;
  const need = new Set(requiredIds.map((x) => String(x || "").trim()).filter(Boolean));
  if (!need.size) return true;
  const arr = Array.isArray(userPolicies) ? userPolicies : [];
  return arr.some((p) => p && need.has(String(p.id || "").trim()));
}

/**
 * Floating assistant + ``/bot`` chat chrome.
 * Prefer policies on ``getAdminBotPage().relatedPolicyIds`` when present; any signed-in user (valid ``/me``)
 * may use the UI so JWT-scoped chat works without HR attaching ``pol_bot_use`` (server still enforces auth on APIs).
 *
 * @param {unknown[]} userPolicies from session (e.g. ``/api/auth/me``.policies)
 * @param {object | null | undefined} sessionUser from ``/api/auth/me``.user (after refresh)
 */
export function canUseBotChatUi(userPolicies, sessionUser) {
  const bot = getAdminBotPage();
  const req = bot && Array.isArray(bot.relatedPolicyIds) ? bot.relatedPolicyIds : [];
  if (sessionSatisfiesRelatedPolicies(userPolicies, req)) return true;
  const u = sessionUser && typeof sessionUser === "object" ? sessionUser : null;
  if (u && (u.id || u.email)) return true;
  return false;
}

/**
 * Stub until ABAC wires policies → uiMethods. Always ``true`` in v1.
 * Later: check JWT claims + policy evaluation.
 *
 * @param {string} method
 * @returns {boolean}
 */
export function canUseUiMethod(method) {
  const c = _currentContext;
  if (!c) return false;
  const methods = c.def.uiMethods;
  const ok = methods.includes(method);
  if (import.meta.env.DEV && !ok) {
    console.debug(`[access-model] UI method not declared on page: ${method}`, describeAccessContext());
  }
  return ok;
}

/**
 * Prefer this in UI code: declared method on current page **and** (later) ABAC.
 *
 * @param {string} method
 * @param {{ onDeny?: () => void }} [opts]
 * @returns {boolean}
 */
export function guardUiMethod(method, opts = {}) {
  if (!canUseUiMethod(method)) {
    opts.onDeny?.();
    return false;
  }
  return true;
}
