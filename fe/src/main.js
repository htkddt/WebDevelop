import "./style.css";
import {
  buildPolicyBuilderClientCatalog,
  canUseBotChatUi,
  getAdminBotPage,
  getWebMenuEntriesForSession,
  getWebNavEntriesForSession,
  hydrateAccessModelFromApi,
  isSpaPathNavigable,
  Platform,
  setAdminAccessContextFromDesk,
  setWebAccessContextFromPath,
} from "./access-model.js";
import { readSessionPlatforms, syncSessionPlatformsFromUser } from "./session-platforms.js";
import {
  authHeaders,
  authLogin,
  authLogout,
  authMe,
  authRegister,
  getToken,
  storeCartHeaders,
} from "./auth.js";
import { getPath, initRouter, navigate, navigateReplace, normalizePath } from "./router.js";

/** Staff roles allowed to open /admin (publish customers are USER). */
const STAFF_ROLES = new Set(["ADMIN", "HR", "SALE", "STORAGE", "FINANCE", "DELIVERY"]);

function isStaffRole(role) {
  return STAFF_ROLES.has(String(role || "").trim().toUpperCase());
}

function isAdminRole(role) {
  return String(role || "").trim().toUpperCase() === "ADMIN";
}

if (!getToken().trim()) {
  syncSessionPlatformsFromUser(null, false, isStaffRole);
}

/** Department codes that share delivery order-desk actions (ship / delivered). */
function isDeliveryDepartment(dept) {
  const D = String(dept || "").toLowerCase();
  return D === "delivery" || D === "logistics" || D === "courier" || D === "shipping";
}

function canOverrideDeliveryAssignee(user) {
  const R = String(user?.role || "").trim().toUpperCase();
  const D = String(user?.department || "").trim().toLowerCase();
  return R === "ADMIN" || R === "FINANCE" || D === "finance";
}

/** Courier-facing order desk: DELIVERY role or logistics-style department. */
function isDeliveryDeskUser(user) {
  const R = String(user?.role || "").trim().toUpperCase();
  return R === "DELIVERY" || isDeliveryDepartment(user?.department);
}

function updateOrdersDeliveryHint(main, user, candidates) {
  const dh = main.querySelector("#adm-order-delivery-hint");
  if (!dh) return;
  if (canOverrideDeliveryAssignee(user)) {
    dh.hidden = false;
    const n = Array.isArray(candidates) ? candidates.length : 0;
    dh.innerHTML =
      n > 0
        ? `Courier list from <code>GET /api/admin/delivery-assignee-candidates</code> (<strong>${n}</strong> staff). Choose a person, click <strong>Assign</strong>; <strong>Assigned to</strong> shows their display name from the server (<code>delivery_assignee_name</code>).`
        : `No rows from <code>GET /api/admin/delivery-assignee-candidates</code> — add accounts with role <strong>DELIVERY</strong> or a logistics-style department, then refresh.`;
    return;
  }
  if (isDeliveryDeskUser(user) && String(user?.role || "").trim().toUpperCase() !== "ADMIN") {
    dh.hidden = false;
    const id = String(user?.id || "").trim();
    dh.innerHTML = id
      ? `<strong>Take order</strong> calls <code>PATCH /api/admin/orders/&lt;orderId&gt;</code> with your <code>delivery_assignee_user_id</code> set to <strong>your</strong> session id: <code>${escapeHtml(id)}</code> (same as <code>/api/auth/me</code> → <code>user.id</code>; this is the app user id, not MongoDB <code>_id</code>).`
      : `Your session has no <code>user.id</code>. Sign out and sign in again so <strong>Take order</strong> can assign you.`;
    return;
  }
  dh.hidden = true;
  dh.textContent = "";
}

/** Set while staff admin shell is mounted; drives hash sync + desk loads. */
let _adminContext = null;

/** Abort listeners from the previous admin desk load (storage forms, etc.). */
let _adminDeskUiAbort = null;

/** Last ``/api/auth/me`` user — used for storefront cart rules (staff cannot cart). */
let _sessionUser = null;
/** Floating assistant: reload history after logout/login (per-user server history). */
let _assistantPanelHistoryPrimed = false;
/** Policy objects from last ``/api/auth/me`` (e.g. Finance desk visibility for cross-hr). */
let _sessionPolicies = [];

const POL_FINANCE_CROSS_HR_PENDING = "pol_finance_cross_hr_pending";

function hasCrossFinancePendingPolicy() {
  return Array.isArray(_sessionPolicies) && _sessionPolicies.some((p) => p && p.id === POL_FINANCE_CROSS_HR_PENDING);
}

/** Product page listeners — must be declared before ``initRouter(applyRoute)`` (TDZ). */
let _storefrontBindingsDone = false;

/** Finance subgroup in sidebar: role FINANCE or department code ``finance``. */
function hasFinanceSubgroupAccess(role, department) {
  const R = String(role || "").trim().toUpperCase();
  const D = String(department || "").trim().toLowerCase();
  return R === "FINANCE" || D === "finance";
}

/** Finance charts + orders desks: HR with ``pol_finance_cross_hr_pending`` (server limits data to pending_confirmation; PATCH orders forbidden). */
function canAccessFinanceChartsCrossPolicy() {
  return hasCrossFinancePendingPolicy();
}

/**
 * Which admin menu tiles this session may see — mirrors docs/ABAC_PRODUCT_TEST_PLAN.md §3.
 * ADMIN short-circuits to almost all; others by role + department.
 * Top-level **Orders** is hidden when **Finance → Orders** covers the same workflow (finance subgroup or ADMIN).
 */
function canAccessAdminSection(role, department, key) {
  const R = String(role || "").trim().toUpperCase();
  const D = String(department || "").trim().toLowerCase();
  if (R === "ADMIN") {
    if (key === "orders") return false;
    return true;
  }
  switch (key) {
    case "bot_c_lib":
      return false;
    case "department":
    case "users":
    case "policies":
      return R === "HR" || D === "hr";
    case "storage":
      return R === "STORAGE" || D === "storage";
    case "finance_charts":
    case "finance_orders":
      return hasFinanceSubgroupAccess(role, department) || canAccessFinanceChartsCrossPolicy();
    case "orders":
      if (hasFinanceSubgroupAccess(role, department)) return false;
      if (R === "DELIVERY") return false;
      return R === "STORAGE" || D === "storage";
    case "delivery":
      // Ship / delivered steps — explicit sidebar label for delivery role (ADMIN also sees this tile).
      return R === "DELIVERY";
    case "sale":
      return R === "SALE" || D === "sale";
    default:
      return false;
  }
}

/** Ordered admin desk tiles (labels match seeded user descriptions in user_store.py). */
const ADMIN_MENU_SECTIONS = [
  {
    key: "department",
    title: "Department",
    help: "Create and edit department codes (org units). User records reference these codes.",
    api: "GET|POST /api/admin/departments · PATCH …/<code> — HR + ADMIN.",
  },
  {
    key: "users",
    title: "Users",
    help:
      "Create accounts, set job role and department, then tick which permission bundles (policies) each person gets. HR cannot change ADMIN accounts.",
    api: "GET|POST /api/admin/users · PATCH …/users/<id> · GET|PUT …/users/<id>/policies — HR + ADMIN.",
  },
  {
    key: "policies",
    title: "Policies & ABAC",
    help:
      "Create bundles below (saved to the server), then assign them on the Users desk → Policies. Route handlers still use role checks unless you wire policy evaluation. See docs/ABAC_HR_GUIDE.md for department vs role vs policies.",
    api: "GET|POST /api/admin/policies · assign via PUT /api/admin/users/<id>/policies",
  },
  {
    key: "bot_c_lib",
    title: "Bot · c-lib options",
    help: "Persist c-library option keys in Mongo (bot_c_lib_settings). Schema and allowed keys come from the server (training/full_options). The live engine still reads the environment only until you restart with matching env.",
    api: "GET|PUT /api/admin/bot-c-lib/settings · GET /api/admin/bot-c-lib/schema — ADMIN only.",
  },
  {
    key: "finance_charts",
    title: "Charts & export",
    group: "Finance",
    help: "KPIs, charts (incl. year-to-date pie), filters, and CSV export. Use Finance → Orders to approve or reject lines.",
    api: "GET /api/admin/finance/summary · GET /api/admin/finance/export — FINANCE + ADMIN; HR + pol_finance_cross_hr_pending (server: pending_confirmation only).",
    sensitive: true,
  },
  {
    key: "finance_orders",
    title: "Orders",
    group: "Finance",
    help: "Same order pipeline as the warehouse Orders desk: filter by status, confirm/reject, full lifecycle as your role allows.",
    api: "GET|PATCH /api/admin/orders — FINANCE + ADMIN (+ STORAGE/DELIVERY per role); HR + pol_finance_cross_hr_pending may GET only (pending_confirmation; PATCH forbidden).",
    sensitive: true,
  },
  {
    key: "storage",
    title: "Storage",
    help: "Products + category registry. Categories: GET|POST /api/admin/product-categories (STORAGE + ADMIN).",
    api: "GET|POST /api/admin/products · product-categories · PATCH …/bulk — STORAGE + ADMIN.",
  },
  {
    key: "orders",
    title: "Orders",
    help: "Full pipeline for warehouse staff (pack, etc.). Finance and ADMIN use Finance → Orders in the sidebar instead. Delivery uses the Delivery desk.",
    api: "GET|PATCH /api/admin/orders — STORAGE + ADMIN.",
    sensitive: true,
  },
  {
    key: "delivery",
    title: "Delivery",
    help: "Mark shipped when the order leaves the warehouse. Do not mark delivered — the customer confirms on the Cart page (ADMIN may override via Finance → Orders or Orders).",
    api: "GET /api/admin/orders · PATCH — DELIVERY + ADMIN.",
    sensitive: true,
  },
  {
    key: "sale",
    title: "Sale",
    help: "Sales desk: read-only product catalog including quantity_available for quoting and selling.",
    api: "GET /api/admin/products — SALE + FINANCE + STORAGE + ADMIN.",
  },
];

/**
 * API origin. If unset in dev, use same-origin `/api/*` (Vite proxies to Flask :5000).
 * Set `VITE_API_URL=http://127.0.0.1:5001` when Flask binds to 5001.
 */
const _rawApi = import.meta.env.VITE_API_URL;
const API_BASE =
  _rawApi != null && String(_rawApi).trim() !== ""
    ? String(_rawApi).replace(/\/$/, "")
    : import.meta.env.DEV
      ? ""
      : "http://127.0.0.1:5000";

/**
 * Single-tenant default: same value for tenant_id and user (c-lib tenant "default").
 * Override via VITE_TENANT_ID when multi-tenant.
 */
const TENANT_ID =
  (import.meta.env.VITE_TENANT_ID && String(import.meta.env.VITE_TENANT_ID).trim()) || "default";

/** Optional: must match server M4ENGINE_GEO_IMPORT_KEY when set */
const GEO_IMPORT_KEY =
  (import.meta.env.VITE_GEO_IMPORT_KEY && String(import.meta.env.VITE_GEO_IMPORT_KEY).trim()) || "";

function escapeHtml(s) {
  const d = document.createElement("div");
  d.textContent = s == null ? "" : String(s);
  return d.innerHTML;
}

/**
 * Legacy anonymous chat (``M4_CHAT_REQUIRE_AUTH=0``): ``tenant_id`` / ``user`` on ``/api/history`` and ``/api/chat/stream``.
 * When the SPA has a JWT, callers omit these — the server uses ``user_id`` from the token.
 */
const CHAT_TENANT_ID =
  (import.meta.env.VITE_CHAT_TENANT_ID && String(import.meta.env.VITE_CHAT_TENANT_ID).trim()) || TENANT_ID;
const CHAT_USER_ID =
  (import.meta.env.VITE_CHAT_USER_ID && String(import.meta.env.VITE_CHAT_USER_ID).trim()) || CHAT_TENANT_ID;

const CHAT_MESSAGE_PLACEHOLDER =
  (import.meta.env.VITE_CHAT_MESSAGE_PLACEHOLDER && String(import.meta.env.VITE_CHAT_MESSAGE_PLACEHOLDER).trim()) ||
  "Message the assistant…";

const ASSISTANT_ENABLED = !["0", "false", "no", "off"].includes(
  String(import.meta.env.VITE_ASSISTANT_ENABLED ?? "1")
    .trim()
    .toLowerCase(),
);

const ASSISTANT_PANEL_TITLE =
  (import.meta.env.VITE_ASSISTANT_TITLE && String(import.meta.env.VITE_ASSISTANT_TITLE).trim()) || "Assistant";
const ASSISTANT_INPUT_PLACEHOLDER =
  (import.meta.env.VITE_ASSISTANT_INPUT_PLACEHOLDER &&
    String(import.meta.env.VITE_ASSISTANT_INPUT_PLACEHOLDER).trim()) ||
  CHAT_MESSAGE_PLACEHOLDER;
const ASSISTANT_FAB_LABEL =
  (import.meta.env.VITE_ASSISTANT_FAB_LABEL && String(import.meta.env.VITE_ASSISTANT_FAB_LABEL).trim()) ||
  "Open assistant";
const ASSISTANT_CLOSE_LABEL =
  (import.meta.env.VITE_ASSISTANT_CLOSE_LABEL && String(import.meta.env.VITE_ASSISTANT_CLOSE_LABEL).trim()) ||
  "Close assistant";
const ASSISTANT_DOCK_ARIA =
  (import.meta.env.VITE_ASSISTANT_DOCK_ARIA_LABEL &&
    String(import.meta.env.VITE_ASSISTANT_DOCK_ARIA_LABEL).trim()) ||
  "Chat assistant";
const ASSISTANT_LOG_ARIA =
  (import.meta.env.VITE_ASSISTANT_LOG_ARIA_LABEL &&
    String(import.meta.env.VITE_ASSISTANT_LOG_ARIA_LABEL).trim()) ||
  "Assistant conversation";
const ASSISTANT_INPUT_ARIA =
  (import.meta.env.VITE_ASSISTANT_INPUT_ARIA_LABEL &&
    String(import.meta.env.VITE_ASSISTANT_INPUT_ARIA_LABEL).trim()) ||
  "Message to assistant";

/** When current path equals this, the floating dock is hidden (e.g. full-page bot). Set ``VITE_ASSISTANT_HIDE_WHEN_PATH=off`` to never hide. */
function readAssistantHideWhenPath() {
  const v = import.meta.env.VITE_ASSISTANT_HIDE_WHEN_PATH;
  if (v === undefined || v === null) return "/bot";
  const s = String(v).trim();
  if (s === "" || s.toLowerCase() === "off" || s.toLowerCase() === "none") return null;
  return normalizePath(s);
}

const ASSISTANT_HIDE_WHEN_PATH = readAssistantHideWhenPath();

/** Shown on the in-progress assistant bubble until ``done`` / ``assistant_meta`` (UI hint only; not a backend claim). */
const CHAT_STREAMING_SOURCE_LABEL =
  (import.meta.env.VITE_CHAT_STREAMING_SOURCE_LABEL &&
    String(import.meta.env.VITE_CHAT_STREAMING_SOURCE_LABEL).trim()) ||
  "stream";

function el(html) {
  const t = document.createElement("template");
  t.innerHTML = html.trim();
  return t.content.firstElementChild;
}

const root = document.getElementById("app");
root.appendChild(
  el(`
  <div class="app">
    <header class="app-header">
      <div class="brand">
        <div class="brand-mark" aria-hidden="true">M4</div>
        <div class="brand-text">
          <h1 class="brand-title">M4 Engine</h1>
          <p class="brand-sub">Assistant</p>
          <p class="header-meta" id="api-endpoint" title="API base URL"></p>
        </div>
      </div>
      <div class="header-end">
      <nav class="app-nav" aria-label="Main">
        <span id="app-nav-web-slots" class="app-nav-web-slots"></span>
        <span id="app-nav-admin-slots" class="app-nav-admin-slots"></span>
      </nav>
      <div class="header-right">
        <div class="header-account" id="header-account">
          <button type="button" class="btn-header-account" id="user-menu-trigger" aria-label="Account" aria-expanded="false" aria-haspopup="true" title="Account">
            <svg class="header-account-svg-guest" id="header-account-icon-guest" width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true"><path d="M20 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2"/><circle cx="12" cy="7" r="4"/></svg>
            <span class="header-account-avatar" id="header-account-avatar" hidden></span>
          </button>
          <div id="user-popover" class="user-popover" hidden role="dialog" aria-label="Account menu">
            <div class="user-popover-head">
              <span class="user-popover-name" id="user-popover-name"></span>
              <span class="user-popover-role" id="user-popover-role"></span>
            </div>
            <nav class="user-popover-nav" aria-label="Quick links">
              <span id="user-popover-web-slots" class="user-popover-web-slots"></span>
              <span id="user-popover-admin-slots" class="user-popover-admin-slots"></span>
            </nav>
            <button type="button" class="user-popover-logout" id="user-popover-logout">Sign out</button>
          </div>
        </div>
        <span class="status-pill" id="status" role="status">Connecting…</span>
      </div>
      </div>
    </header>

    <div id="route-products" class="route-view route-view--products is-active">
      <main class="page-main page-products">
        <h2 class="page-title">Store</h2>
        <p class="page-lead">
          Published catalog from <code>GET /api/store/products</code>. Guests and registered customers (<code>USER</code>) may add to cart;
          company staff accounts cannot use the cart (server enforced).
        </p>
        <div class="store-staff-notice admin-gate admin-gate--warn" id="store-staff-notice" hidden>
          <p>Signed in as <strong>staff</strong> — adding to cart is disabled. Use a customer account or sign out to shop as a guest.</p>
        </div>
        <div class="store-toolbar">
          <label class="store-filter-label">Category
            <select id="product-category-filter" class="store-filter-select" aria-label="Filter by category">
              <option value="">All</option>
            </select>
          </label>
          <button type="button" class="btn-store-reload" id="product-store-reload" title="Reload catalog and category list">Refresh catalog</button>
        </div>
        <div id="product-store-root" class="store-product-grid" aria-live="polite"></div>
        <p class="store-api-error" id="product-store-error" hidden role="status"></p>
      </main>
    </div>

    <div id="route-cart" class="route-view route-view--cart">
      <main class="page-main page-cart">
        <h2 class="page-title">Your cart</h2>
        <p class="page-lead" id="cart-context-hint">Loading…</p>
        <div class="admin-gate admin-gate--warn" id="cart-staff-block" hidden>
          <p>Company (staff) accounts cannot use the storefront cart.</p>
          <p><a href="/productions" class="btn-nav-cta btn-nav-cta--secondary" data-route>Back to productions</a></p>
        </div>
        <div id="cart-customer-shell" class="cart-customer-shell" hidden>
          <div class="cart-view-tabs" role="tablist" aria-label="Cart and order history">
            <button type="button" class="cart-view-tab is-active" role="tab" id="cart-tab-cart" aria-controls="cart-panel-column-cart" aria-selected="true" data-cart-view="cart">Shopping cart</button>
            <button type="button" class="cart-view-tab" role="tab" id="cart-tab-history" aria-controls="cart-panel-column-history" aria-selected="false" tabindex="-1" data-cart-view="history">Order history</button>
          </div>
          <div class="cart-view-panels">
            <div class="cart-panel-column cart-panel-column--cart is-active" id="cart-panel-column-cart" role="tabpanel" aria-labelledby="cart-tab-cart" data-cart-panel="cart">
              <div id="cart-panel" hidden>
                <div class="cart-table-wrap" id="cart-table-wrap"></div>
                <p class="cart-subtotal" id="cart-subtotal" aria-live="polite"></p>
                <button type="button" class="btn-cart-save" id="cart-save-btn" title="Save line quantities to your cart">Save cart</button>
                <div class="cart-checkout-block" id="cart-checkout-block">
                  <label class="login-label">Delivery note <span class="login-opt">(optional)</span>
                    <input type="text" id="cart-checkout-note" class="adm-prod-inp adm-prod-inp--wide" maxlength="500" placeholder="Instructions for fulfillment" autocomplete="off" />
                  </label>
                  <label class="login-label nav-customer-only">Contact email <span class="login-opt">(guest)</span>
                    <input type="email" id="cart-checkout-contact-email" class="adm-prod-inp adm-prod-inp--wide" placeholder="you@example.com" autocomplete="email" />
                  </label>
                  <p class="cart-checkout-status" id="cart-checkout-status" role="status" aria-live="polite"></p>
                  <button type="button" class="btn-login-primary" id="cart-checkout-btn" title="Turn this cart into an order (pending finance approval)">Place order</button>
                </div>
              </div>
            </div>
            <div class="cart-panel-column cart-panel-column--history" id="cart-panel-column-history" role="tabpanel" aria-labelledby="cart-tab-history" data-cart-panel="history" tabindex="-1">
              <h3 class="cart-orders-heading" id="cart-orders-heading">Order history</h3>
              <p class="page-lead cart-orders-hint" id="cart-orders-hint"></p>
              <div class="admin-table-wrap cart-orders-table-wrap" id="cart-orders-wrap"></div>
            </div>
          </div>
        </div>
      </main>
    </div>

    <div id="route-register" class="route-view route-view--register">
      <main class="page-main page-register">
        <h2 class="page-title">Create customer account</h2>
        <p class="page-lead">Registers a <code>USER</code> role for the storefront (cart, orders). Staff accounts are created separately.</p>
        <form id="register-form" class="register-form">
          <label class="login-label">Name <span class="login-opt">(optional)</span>
            <input type="text" id="reg-name" name="name" autocomplete="name" />
          </label>
          <label class="login-label">Email
            <input type="email" id="reg-email" name="email" required autocomplete="email" />
          </label>
          <label class="login-label">Password <span class="login-opt">(min 6)</span>
            <input type="password" id="reg-password" name="password" required autocomplete="new-password" minlength="6" />
          </label>
          <p class="register-status" id="register-status" role="status"></p>
          <button type="submit" class="btn-login-primary" id="register-submit">Create account</button>
        </form>
      </main>
    </div>

    <div id="route-bot" class="route-view route-view--home">
    <div id="bot-gate" class="bot-gate" hidden></div>
    <div id="bot-workspace" class="bot-workspace">
    <details class="system-panel">
      <summary>System status</summary>
      <div id="stats" class="stats-grid">—</div>
    </details>
    <details class="system-panel geo-import-panel">
      <summary>Import geo CSV</summary>
      <div class="geo-import-body">
        <p class="geo-import-hint">
          Bulk-load landmarks into <code>geo_atlas</code> (<code>POST /api/geo/import</code>). CSV needs a
          <strong>name</strong> column; optional JSON mapping renames columns (see <code>geo_csv_import.py</code>).
        </p>
        <form id="geo-import-form" class="geo-import-form">
          <div class="geo-file-row">
            <input
              type="file"
              id="geo-file"
              name="file"
              accept=".csv,text/csv"
              class="geo-file-input"
            />
            <span id="geo-file-name" class="geo-file-name">No file chosen</span>
          </div>
          <label class="geo-label" for="geo-mapping">Optional column mapping (JSON)</label>
          <textarea
            id="geo-mapping"
            class="geo-mapping-input"
            rows="2"
            placeholder='{"name":"landmark","district":"quan"}'
            autocomplete="off"
          ></textarea>
          <label class="geo-check">
            <input type="checkbox" id="geo-no-embed" checked />
            Skip Ollama embeddings (default — no server needed). Uncheck to use <code>?embed=1</code>.
          </label>
          <button type="submit" class="btn-geo-upload" id="geo-upload-btn">Upload</button>
          <p id="geo-import-result" class="geo-import-result" role="status" aria-live="polite"></p>
        </form>
      </div>
    </details>
    <main class="chat-main">
      <div class="chat-log" id="log" aria-live="polite" aria-label="Conversation"></div>
    </main>
    <footer class="composer">
      <form class="composer-form" id="form">
        <textarea
          id="input"
          rows="2"
          placeholder="${escapeHtml(CHAT_MESSAGE_PLACEHOLDER)}"
          autocomplete="off"
          aria-label="${escapeHtml(CHAT_MESSAGE_PLACEHOLDER)}"
        ></textarea>
        <button type="submit" class="btn-send">Send</button>
      </form>
      <p class="composer-hint">Enter to send · Ctrl+Enter new line</p>
    </footer>
    </div>
    </div>

    <div id="route-admin" class="route-view route-view--admin">
      <main class="page-main page-admin" id="admin-page-root">
        <p class="page-lead">Loading…</p>
      </main>
    </div>

    <div id="auth-modal" class="auth-modal" hidden aria-hidden="true" role="dialog" aria-modal="true" aria-labelledby="login-heading">
      <div class="auth-modal-backdrop" data-auth-modal-close tabindex="-1"></div>
      <div class="auth-modal-panel">
        <div class="auth-modal-toolbar">
          <h2 class="auth-modal-title" id="login-heading">Sign in</h2>
          <button type="button" class="auth-modal-close" data-auth-modal-close aria-label="Close">×</button>
        </div>
        <p class="login-card-hint auth-modal-hint">
          Default seed password <code>12345678@Ab</code> — e.g. <code>admin@mailinator.com</code> or <code>user@mailinator.com</code>.
          See <code>docs/AUTH_JWT.md</code>.
        </p>
        <div class="login-grid login-grid--signin-only">
          <div class="login-col login-col--full">
            <label class="login-label">Email
              <input type="email" id="auth-email" autocomplete="username" placeholder="admin@mailinator.com" />
            </label>
            <label class="login-label">Password
              <input type="password" id="auth-password" autocomplete="current-password" placeholder="12345678@Ab" />
            </label>
            <div class="login-actions">
              <button type="button" class="btn-login-primary" id="auth-login">Sign in</button>
              <button type="button" class="btn-login-secondary" id="auth-logout">Sign out</button>
            </div>
          </div>
        </div>
        <p class="auth-status" id="auth-status" role="status" aria-live="polite">Not logged in</p>
      </div>
    </div>

    <div class="assistant-dock${ASSISTANT_ENABLED ? "" : " assistant-dock--disabled"}" id="assistant-dock" aria-label="${escapeHtml(ASSISTANT_DOCK_ARIA)}">
      <div
        id="assistant-panel"
        class="assistant-panel"
        role="dialog"
        aria-modal="true"
        aria-labelledby="assistant-panel-title"
        hidden
      >
        <div class="assistant-panel-header">
          <h2 class="assistant-panel-title" id="assistant-panel-title">${escapeHtml(ASSISTANT_PANEL_TITLE)}</h2>
          <button type="button" class="assistant-panel-close" id="assistant-close" aria-label="${escapeHtml(ASSISTANT_CLOSE_LABEL)}">
            ×
          </button>
        </div>
        <div class="assistant-panel-chat">
          <div id="assistant-auth-gate" class="assistant-auth-gate">
            <div id="assistant-gate-signin" class="assistant-gate-panel">
              <p class="assistant-auth-gate-lead">Sign in to use the assistant. Your messages are tied to your account id on the server.</p>
              <p class="assistant-auth-gate-actions">
                <button type="button" class="btn-nav-cta" id="assistant-sign-in" data-open-auth-modal data-auth-next="/productions">Sign in</button>
              </p>
              <p class="admin-bulk-hint">Phase 2 (later): anonymous chat with a saved temp id in the browser.</p>
            </div>
            <div id="assistant-gate-policy" class="assistant-gate-panel" hidden>
              <p class="assistant-auth-gate-lead">Your account does not have the chat permission bundle yet.</p>
              <p class="admin-bulk-hint">Ask an admin to attach policy <code>pol_bot_use</code> (or the ids listed on the bot workspace in <strong>Policies</strong>) to your user. You can open <a href="/admin" class="assistant-gate-link" data-route>Admin</a> if you already have staff access.</p>
            </div>
          </div>
          <div id="assistant-chat-shell" class="assistant-chat-shell">
            <div
              class="chat-log assistant-chat-log"
              id="assistant-log"
              aria-live="polite"
              aria-label="${escapeHtml(ASSISTANT_LOG_ARIA)}"
            ></div>
            <footer class="assistant-composer">
              <form id="assistant-form" class="composer-form">
                <textarea
                  id="assistant-input"
                  rows="2"
                  placeholder="${escapeHtml(ASSISTANT_INPUT_PLACEHOLDER)}"
                  autocomplete="off"
                  aria-label="${escapeHtml(ASSISTANT_INPUT_ARIA)}"
                ></textarea>
                <button type="submit" class="btn-send" id="assistant-send">Send</button>
              </form>
              <p class="composer-hint assistant-composer-hint">Enter to send · Ctrl+Enter new line</p>
            </footer>
          </div>
        </div>
      </div>
      <button
        type="button"
        id="assistant-fab"
        class="assistant-fab"
        aria-expanded="false"
        aria-controls="assistant-panel"
        title="${escapeHtml(ASSISTANT_FAB_LABEL)}"
        aria-label="${escapeHtml(ASSISTANT_FAB_LABEL)}"
      >
        <svg width="26" height="26" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true">
          <path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z" />
        </svg>
      </button>
    </div>
  </div>
`)
);

const statusEl = document.getElementById("status");
const statsEl = document.getElementById("stats");
const logEl = document.getElementById("log");
const form = document.getElementById("form");
const input = document.getElementById("input");
const apiEndpointEl = document.getElementById("api-endpoint");
apiEndpointEl.textContent = API_BASE
  ? API_BASE.replace(/^https?:\/\//, "")
  : "Vite :8000 → /api → Flask";
apiEndpointEl.title = API_BASE
  ? "Direct API base"
  : "Open this app at http://127.0.0.1:8000/ — Vite proxies /api to Flask (see vite.config.js)";

const geoImportForm = document.getElementById("geo-import-form");
const geoFileInput = document.getElementById("geo-file");
const geoFileNameEl = document.getElementById("geo-file-name");
const geoMappingInput = document.getElementById("geo-mapping");
const geoNoEmbedInput = document.getElementById("geo-no-embed");
const geoUploadBtn = document.getElementById("geo-upload-btn");
const geoImportResultEl = document.getElementById("geo-import-result");
const authEmailEl = document.getElementById("auth-email");
const authPasswordEl = document.getElementById("auth-password");
const authLoginBtn = document.getElementById("auth-login");
const authLogoutBtn = document.getElementById("auth-logout");
const authStatusEl = document.getElementById("auth-status");
const routeBotEl = document.getElementById("route-bot");
const routeProductsEl = document.getElementById("route-products");
const routeCartEl = document.getElementById("route-cart");
const routeRegisterEl = document.getElementById("route-register");
const routeAdminEl = document.getElementById("route-admin");
const assistantDockEl = document.getElementById("assistant-dock");
const assistantPanelEl = document.getElementById("assistant-panel");
const assistantFabEl = document.getElementById("assistant-fab");
const assistantLogEl = document.getElementById("assistant-log");
const assistantFormEl = document.getElementById("assistant-form");
const assistantInputEl = document.getElementById("assistant-input");
const assistantAuthGateEl = document.getElementById("assistant-auth-gate");
const assistantChatShellEl = document.getElementById("assistant-chat-shell");

/** Phase 1: floating assistant requires JWT + access-model policies on ``getAdminBotPage().relatedPolicyIds``. */
function refreshAssistantDockChrome() {
  if (!assistantDockEl || !ASSISTANT_ENABLED) return;
  const tok = getToken().trim();
  const denyFab = !!(tok && !canUseBotChatUi(_sessionPolicies, _sessionUser));
  assistantDockEl.classList.toggle("assistant-dock--policy-denied", denyFab);
}

function syncAssistantPanelAuth() {
  const gate = assistantAuthGateEl;
  const shell = assistantChatShellEl;
  const signin = document.getElementById("assistant-gate-signin");
  const policy = document.getElementById("assistant-gate-policy");
  if (!gate || !shell) return;
  const tok = getToken().trim();
  if (!tok) {
    gate.hidden = false;
    shell.hidden = true;
    if (signin) signin.hidden = false;
    if (policy) policy.hidden = true;
    refreshAssistantDockChrome();
    return;
  }
  if (!canUseBotChatUi(_sessionPolicies, _sessionUser)) {
    gate.hidden = false;
    shell.hidden = true;
    if (signin) signin.hidden = true;
    if (policy) policy.hidden = false;
    refreshAssistantDockChrome();
    return;
  }
  gate.hidden = true;
  shell.hidden = false;
  if (signin) signin.hidden = false;
  if (policy) policy.hidden = true;
  refreshAssistantDockChrome();
}

function syncMainNavAriaForPath(path) {
  const p = normalizePath(path);
  document.querySelectorAll(".app-nav .app-nav-link[data-route]").forEach((a) => {
    const active = normalizePath(a.getAttribute("href") || "/") === p;
    a.classList.toggle("is-active", active);
    if (active) a.setAttribute("aria-current", "page");
    else a.removeAttribute("aria-current");
  });
}

function applyRoute(path) {
  const p = normalizePath(path);
  if (p === "/" || p === "/products") {
    navigateReplace("/productions");
    return;
  }
  const hasToken = !!getToken().trim();
  const sessionPlatforms = readSessionPlatforms();
  if (!isSpaPathNavigable(p, hasToken, { sessionPlatforms })) {
    navigateReplace("/productions");
    return;
  }

  routeProductsEl.classList.toggle("is-active", p === "/productions");
  routeCartEl.classList.toggle("is-active", p === "/cart");
  routeRegisterEl.classList.toggle("is-active", p === "/register");
  routeBotEl.classList.toggle("is-active", p === "/bot");
  routeAdminEl.classList.toggle("is-active", p === "/admin");
  document.querySelector(".app")?.classList.toggle("app--admin-desk", p === "/admin");
  if (assistantDockEl && ASSISTANT_ENABLED) {
    assistantDockEl.classList.toggle(
      "assistant-dock--hidden",
      ASSISTANT_HIDE_WHEN_PATH != null && p === ASSISTANT_HIDE_WHEN_PATH,
    );
    refreshAssistantDockChrome();
  }

  syncMainNavAriaForPath(p);

  if (p === "/productions") {
    document.title = "M4 Engine · Home";
    ensureStorefrontProductsBindings();
    void renderStorefrontProductsPage();
  } else if (p === "/cart") {
    document.title = "M4 Engine · Cart";
    void renderCartPage();
  } else if (p === "/register") {
    document.title = "M4 Engine · Register";
  } else if (p === "/bot") {
    document.title = "M4 Engine · Bot status";
    void renderBotPage();
  } else if (p === "/admin") {
    document.title = "M4 Engine · Admin";
    renderAdminGate();
  }

  setWebAccessContextFromPath(p);
}

async function renderBotPage() {
  const gate = document.getElementById("bot-gate");
  const ws = document.getElementById("bot-workspace");
  if (!gate || !ws) return;

  gate.hidden = true;
  gate.innerHTML = "";
  ws.hidden = false;

  if (!getToken().trim()) {
    gate.hidden = false;
    ws.hidden = true;
    gate.innerHTML = `
      <div class="admin-gate admin-gate--warn">
        <p><strong>Bot workspace</strong> — sign in is required (chat uses your account id on the server).</p>
        <p><a href="#" class="btn-nav-cta" data-open-auth-modal data-auth-next="/bot">Sign in</a></p>
        <p><a href="/productions" class="btn-nav-cta btn-nav-cta--secondary" data-route>Back to home</a></p>
      </div>`;
    return;
  }

  try {
    const data = await authMe(API_BASE);
    const role = (data.user && data.user.role) || "";
    _sessionUser = data.user || {};
    _sessionPolicies = Array.isArray(data.policies) ? data.policies : [];
    if (!canUseBotChatUi(_sessionPolicies, _sessionUser)) {
      gate.hidden = false;
      ws.hidden = true;
      gate.innerHTML = `
        <div class="admin-gate admin-gate--warn">
          <p><strong>Bot workspace</strong> — your account needs the chat permission bundle (e.g. policy <code>pol_bot_use</code>).</p>
          <p class="admin-bulk-hint">Ask an admin to attach it under <strong>Users</strong> → <strong>Policies</strong>, or edit <code>relatedPolicyIds</code> on the bot page in Mongo <code>access_pages</code> to change which policies apply.</p>
          <p><a href="/productions" class="btn-nav-cta btn-nav-cta--secondary" data-route>Back to home</a></p>
        </div>`;
      return;
    }
    gate.hidden = true;
    ws.hidden = false;
    ws.querySelectorAll(".system-panel").forEach((panel) => {
      panel.hidden = !isAdminRole(role);
    });
  } catch (e) {
    gate.hidden = false;
    ws.hidden = true;
    gate.innerHTML = `
      <div class="admin-gate admin-gate--warn">
        <p>Session invalid: ${escapeHtml(String(e.message || e))}</p>
        <p><a href="/" class="btn-nav-cta btn-nav-cta--secondary" data-route>Back to home</a></p>
      </div>`;
    return;
  }

  try {
    await refreshStatsPanel();
    await refreshHistoryAndChat();
  } catch {
    /* panels/chat may fail if API down */
  }
}

async function renderAdminGate() {
  const root = document.getElementById("admin-page-root");
  if (!root) return;

  _adminContext = null;

  if (!getToken().trim()) {
    root.innerHTML = `
      <h2 class="page-title">Admin</h2>
      <div class="admin-gate admin-gate--warn">
        <p>Sign in is required to access the admin area.</p>
        <p><a href="#" class="btn-nav-cta" data-open-auth-modal data-auth-next="/admin">Sign in</a></p>
      </div>`;
    return;
  }

  root.innerHTML = `<p class="page-lead">Checking session…</p>`;
  try {
    const data = await authMe(API_BASE);
    const role = (data.user && data.user.role) || "";
    _sessionUser = data.user || {};
    _sessionPolicies = Array.isArray(data.policies) ? data.policies : [];
    if (!isStaffRole(role)) {
      root.innerHTML = `
        <h2 class="page-title">Admin</h2>
        <div class="admin-gate admin-gate--warn">
          <p>This area is for <strong>staff</strong> only. Your account role is <strong>${escapeHtml(role)}</strong>.</p>
          <p><a href="/" class="btn-nav-cta btn-nav-cta--secondary" data-route>Back to home</a></p>
        </div>`;
      return;
    }
    const u = data.user || {};
    const dept = u.department || "";
    const visibleKeys = ADMIN_MENU_SECTIONS.filter((s) =>
      canAccessAdminSection(u.role || "", dept, s.key),
    ).map((s) => s.key);
    _adminContext = { user: u, visibleKeys };
    root.innerHTML = buildAdminShellHtml(u);
    applyAdminHash();
  } catch (e) {
    root.innerHTML = `
      <h2 class="page-title">Admin</h2>
      <div class="admin-gate admin-gate--warn">
        <p>Session invalid: ${escapeHtml(String(e.message || e))}</p>
        <p><a href="/" class="btn-nav-cta btn-nav-cta--secondary" data-route>Back to home</a></p>
      </div>`;
  }
}

root.addEventListener("click", (e) => {
  if (e.target.closest("[data-auth-modal-close]")) {
    e.preventDefault();
    closeAuthModal();
    return;
  }
  const openAuth = e.target.closest("[data-open-auth-modal]");
  if (openAuth && root.contains(openAuth)) {
    e.preventDefault();
    const nextDesk = openAuth.getAttribute("data-auth-next");
    if (nextDesk && nextDesk.startsWith("/") && !nextDesk.startsWith("//")) {
      navigate("/", `next=${encodeURIComponent(nextDesk)}`);
    }
    openAuthModal();
    return;
  }
  const a = e.target.closest("a[data-route]");
  if (a && root.contains(a)) {
    e.preventDefault();
    navigate(a.getAttribute("href") || "/");
  }
});

function openAuthModal() {
  const m = document.getElementById("auth-modal");
  if (!m) return;
  closeUserPopover();
  m.hidden = false;
  m.setAttribute("aria-hidden", "false");
  document.body.classList.add("auth-modal-open");
  queueMicrotask(() => document.getElementById("auth-email")?.focus());
}

function closeAuthModal() {
  const m = document.getElementById("auth-modal");
  if (!m || m.hidden) return;
  m.hidden = true;
  m.setAttribute("aria-hidden", "true");
  document.body.classList.remove("auth-modal-open");
}

function openUserPopover() {
  const p = document.getElementById("user-popover");
  const btn = document.getElementById("user-menu-trigger");
  if (p) p.hidden = false;
  if (btn) btn.setAttribute("aria-expanded", "true");
}

function closeUserPopover() {
  const p = document.getElementById("user-popover");
  const btn = document.getElementById("user-menu-trigger");
  if (p) p.hidden = true;
  if (btn) btn.setAttribute("aria-expanded", "false");
}

function toggleUserPopover() {
  const p = document.getElementById("user-popover");
  if (p?.hidden) openUserPopover();
  else closeUserPopover();
}

function refreshCustomerNavVisibility() {
  const loggedIn = !!getToken().trim();
  document.querySelectorAll(".nav-customer-only").forEach((node) => {
    node.hidden = loggedIn;
  });
}

/**
 * Main bar + account menu from server ``webNav`` / ``webMenu`` (or same logic client-side if absent).
 */
function renderWebPlatformNavSlots(user, hasToken) {
  const mainEl = document.getElementById("app-nav-web-slots");
  const popEl = document.getElementById("user-popover-web-slots");
  if (!mainEl || !popEl) return;
  const isStaff = !!(user && hasToken && isStaffRole(user.role));

  mainEl.replaceChildren();
  for (const item of getWebNavEntriesForSession({ hasToken, isStaff })) {
    const a = document.createElement("a");
    a.href = item.link;
    a.className = "app-nav-link";
    a.setAttribute("data-route", "");
    const span = document.createElement("span");
    span.className = "app-nav-text";
    span.textContent = item.name;
    a.appendChild(span);
    mainEl.appendChild(a);
  }

  popEl.replaceChildren();
  for (const item of getWebMenuEntriesForSession({ hasToken })) {
    const a = document.createElement("a");
    a.href = item.link;
    a.className = "user-popover-link";
    a.setAttribute("data-route", "");
    if (normalizePath(item.path) === "/register") a.classList.add("nav-customer-only");
    a.textContent = item.name;
    popEl.appendChild(a);
  }
}

function renderAdminPlatformNavSlots(user, hasToken) {
  const mainEl = document.getElementById("app-nav-admin-slots");
  const popEl = document.getElementById("user-popover-admin-slots");
  if (!mainEl || !popEl) return;
  mainEl.replaceChildren();
  popEl.replaceChildren();

  const showBot = !!(user && isAdminRole(user.role));
  const showAdminShell = !!(user && hasToken && isStaffRole(user.role));

  const botIcon =
    '<svg class="app-nav-icon" width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="3" y="11" width="18" height="10" rx="2"/><circle cx="12" cy="5" r="3"/><path d="M12 8v3"/></svg>';
  const adminIcon =
    '<svg class="app-nav-icon" width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/></svg>';

  const appendMainBar = (href, label, iconHtml) => {
    const a = document.createElement("a");
    a.href = href;
    a.className = "app-nav-link";
    a.setAttribute("data-route", "");
    const span = document.createElement("span");
    span.className = "app-nav-text";
    span.textContent = label;
    a.appendChild(span);
    const tpl = document.createElement("template");
    tpl.innerHTML = iconHtml;
    a.appendChild(tpl.content);
    mainEl.appendChild(a);
  };

  if (showBot) {
    const bot = getAdminBotPage();
    const href = String(bot.path || "/bot").trim() || "/bot";
    const label = String(bot.label || "Bot status").trim() || "Bot status";
    appendMainBar(href, label, botIcon);
    const pa = document.createElement("a");
    pa.href = href;
    pa.className = "user-popover-link user-popover-link--bot";
    pa.setAttribute("data-route", "");
    pa.textContent = label;
    popEl.appendChild(pa);
  }

  if (showAdminShell) {
    appendMainBar("/admin", "Admin", adminIcon);
    const aa = document.createElement("a");
    aa.href = "/admin";
    aa.className = "user-popover-link user-popover-link--admin";
    aa.setAttribute("data-route", "");
    aa.textContent = "Admin";
    popEl.appendChild(aa);
  }
}

function updateUserNavBar(user) {
  const guestSvg = document.getElementById("header-account-icon-guest");
  const av = document.getElementById("header-account-avatar");
  if (!guestSvg || !av) return;

  const hasToken = !!getToken().trim();
  syncSessionPlatformsFromUser(user, hasToken, isStaffRole);

  renderWebPlatformNavSlots(user, hasToken);
  renderAdminPlatformNavSlots(user, hasToken);
  syncMainNavAriaForPath(getPath());

  if (!user || !hasToken) {
    guestSvg.hidden = false;
    av.hidden = true;
    closeUserPopover();
    refreshCustomerNavVisibility();
    return;
  }

  guestSvg.hidden = true;
  av.hidden = false;
  const display = (user.name || user.email || "User").trim();
  av.textContent = (display.charAt(0) || "?").toUpperCase();

  const nameEl = document.getElementById("user-popover-name");
  const roleEl = document.getElementById("user-popover-role");
  const r = user.role || "?";
  const d = user.department;
  if (nameEl) nameEl.textContent = display;
  if (roleEl) roleEl.textContent = d ? `${r} · ${d}` : String(r);
  refreshCustomerNavVisibility();
}

function performLogout() {
  authLogout();
  _sessionUser = null;
  _sessionPolicies = [];
  authStatusEl.textContent = "Not logged in";
  closeUserPopover();
  closeAuthModal();
  updateUserNavBar(null);
  syncAssistantPanelAuth();
  _assistantPanelHistoryPrimed = false;
  const cur = getPath();
  if (cur === "/admin" || cur === "/bot") {
    navigateReplace("/productions");
  } else if (!isSpaPathNavigable(cur, false, { sessionPlatforms: readSessionPlatforms() })) {
    navigateReplace("/productions");
  }
}

async function refreshAuthStatus() {
  if (!getToken().trim()) {
    _sessionUser = null;
    _sessionPolicies = [];
    authStatusEl.textContent = "Not logged in";
    updateUserNavBar(null);
    refreshCustomerNavVisibility();
    syncAssistantPanelAuth();
    return;
  }
  try {
    const data = await authMe(API_BASE);
    const u = data.user || {};
    _sessionUser = u;
    _sessionPolicies = Array.isArray(data.policies) ? data.policies : [];
    const nPol = _sessionPolicies.length;
    let desc = "";
    if (u.description) {
      const d = String(u.description);
      desc = d.length > 160 ? ` — ${d.slice(0, 160)}…` : ` — ${d}`;
    }
    authStatusEl.textContent = `Logged in: ${u.name || u.email || "?"} · ${u.email} · role ${u.role || "?"}${nPol ? ` · ${nPol} policies` : ""}${desc}`;
    updateUserNavBar(u);
    if (readSessionPlatforms().includes(Platform.ADMIN)) {
      await hydrateAccessModelFromApi(API_BASE, { includeAdmin: true });
      updateUserNavBar(u);
    }
    syncAssistantPanelAuth();
    if (assistantPanelEl && !assistantPanelEl.hidden && getToken().trim()) {
      void refreshHistoryAndChatInto(assistantLogEl).catch(() => {});
    }
  } catch (e) {
    _sessionUser = null;
    _sessionPolicies = [];
    authStatusEl.textContent = `Token invalid: ${e.message}`;
    updateUserNavBar(null);
    syncAssistantPanelAuth();
  }
}

authLoginBtn.addEventListener("click", async () => {
  const email = (authEmailEl.value || "").trim();
  const password = authPasswordEl.value || "";
  if (!email || !password) {
    authStatusEl.textContent = "Enter email and password";
    return;
  }
  authLoginBtn.disabled = true;
  try {
    await authLogin(API_BASE, email, password);
    authPasswordEl.value = "";
    await refreshAuthStatus();
    closeAuthModal();
    const next = new URLSearchParams(window.location.search).get("next");
    if (next && next.startsWith("/") && !next.startsWith("//")) {
      navigate(next);
    } else {
      applyRoute(getPath());
    }
  } catch (e) {
    authStatusEl.textContent = String(e.message || e);
  } finally {
    authLoginBtn.disabled = false;
  }
});

authLogoutBtn.addEventListener("click", () => {
  performLogout();
});

document.getElementById("register-form")?.addEventListener("submit", async (e) => {
  e.preventDefault();
  const email = (document.getElementById("reg-email")?.value || "").trim();
  const password = document.getElementById("reg-password")?.value || "";
  const name = (document.getElementById("reg-name")?.value || "").trim();
  const statusEl = document.getElementById("register-status");
  const btn = document.getElementById("register-submit");
  if (!email || !password || password.length < 6) {
    if (statusEl) statusEl.textContent = "Enter a valid email and password (min 6 characters).";
    return;
  }
  if (btn) btn.disabled = true;
  if (statusEl) statusEl.textContent = "";
  try {
    await authRegister(API_BASE, email, password, name || undefined, undefined);
    await authLogin(API_BASE, email, password);
    await refreshAuthStatus();
    if (statusEl) statusEl.textContent = "Account created — you are signed in.";
    navigate("/productions");
  } catch (err) {
    if (statusEl) statusEl.textContent = String(err.message || err);
  } finally {
    if (btn) btn.disabled = false;
  }
});

document.getElementById("user-popover-logout")?.addEventListener("click", () => {
  performLogout();
});

document.getElementById("user-menu-trigger")?.addEventListener("click", (e) => {
  e.stopPropagation();
  if (!getToken().trim()) {
    openAuthModal();
    return;
  }
  toggleUserPopover();
});

document.addEventListener("click", (e) => {
  const wrap = document.getElementById("header-account");
  const pop = document.getElementById("user-popover");
  if (!pop || pop.hidden || !wrap) return;
  if (wrap.contains(e.target)) return;
  closeUserPopover();
});

document.addEventListener("keydown", (e) => {
  if (e.key !== "Escape") return;
  closeAuthModal();
  closeUserPopover();
});

geoFileInput.addEventListener("change", () => {
  const f = geoFileInput.files?.[0];
  geoFileNameEl.textContent = f ? f.name : "No file chosen";
});

function setStatus(text, variant) {
  statusEl.textContent = text;
  statusEl.className = "status-pill";
  if (variant) statusEl.classList.add(variant);
}

function pad2(n) {
  return n < 10 ? `0${n}` : `${n}`;
}

function nowHms() {
  const d = new Date();
  return `${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
}

function formatTurnTime(ts) {
  if (ts == null) return nowHms();
  const s = String(ts).trim();
  if (!s) return nowHms();
  if (/^\d+$/.test(s)) {
    const n = parseInt(s, 10);
    const ms = n < 1e12 ? n * 1000 : n;
    const d = new Date(ms);
    if (!Number.isNaN(d.getTime())) {
      return d.toLocaleTimeString("en-GB", {
        hour12: false,
        hour: "2-digit",
        minute: "2-digit",
        second: "2-digit",
      });
    }
  }
  if (/^\d{1,2}:\d{2}/.test(s)) return s.length >= 8 ? s.slice(0, 8) : s;
  return s.length > 16 ? s.slice(0, 16) : s;
}

function normalizeSource(raw) {
  const map = {
    MEMORY: "memory",
    REDIS: "redis",
    MONGODB: "mongodb",
    OLLAMA: "ollama",
    CLOUD: "cloud",
    GROQ: "groq",
    GEMINI: "gemini",
    CEREBRAS: "cerebras",
    EXTERNAL: "external",
    REDIS_RAG: "redis_rag",
  };
  const u = String(raw || "").toUpperCase();
  if (map[u]) return map[u];
  const low = String(raw || "memory").toLowerCase();
  if (
    ["memory", "redis", "mongodb", "ollama", "cloud", "groq", "gemini", "cerebras", "external", "redis_rag"].includes(
      low,
    )
  )
    return low;
  return low || "memory";
}

/** Client correlation id for streaming + Mongo temp_message_id (c-lib). */
function newTempMessageId() {
  return crypto.randomUUID();
}

/** Build one chat row (same structure as renderTurns). */
function buildMessageRow(role, content, time, sourceForAssistant) {
  const wrap = document.createElement("div");
  wrap.className = `msg ${role === "user" ? "user" : "bot"}`;

  const inner = document.createElement("div");
  inner.className = "msg-inner";

  const meta = document.createElement("div");
  meta.className = "msg-meta";

  const roleSpan = document.createElement("span");
  roleSpan.className = "msg-role";
  roleSpan.textContent = role === "user" ? "You" : "Assistant";

  const timeSpan = document.createElement("span");
  timeSpan.className = "msg-time";
  timeSpan.textContent = time;

  meta.appendChild(roleSpan);
  meta.appendChild(timeSpan);

  if (role === "assistant") {
    const badge = document.createElement("span");
    badge.className = "source-badge";
    badge.textContent = normalizeSource(sourceForAssistant);
    meta.appendChild(badge);
  }

  const body = document.createElement("div");
  body.className = "turn-body";
  body.textContent = content;

  inner.appendChild(meta);
  inner.appendChild(body);
  wrap.appendChild(inner);
  return { wrap, body };
}

/** Pull complete SSE `data:` events from a buffer; returns unconsumed tail. */
function consumeSSEBlocks(buf) {
  const events = [];
  let s = buf.replace(/\r\n/g, "\n");
  while (true) {
    const idx = s.indexOf("\n\n");
    if (idx < 0) return { events, rest: s };
    const block = s.slice(0, idx);
    s = s.slice(idx + 2);
    const line = block.split("\n").find((l) => l.startsWith("data:"));
    if (line == null) continue;
    const json = line.replace(/^data:\s*/i, "").trim();
    if (!json) continue;
    try {
      events.push(JSON.parse(json));
    } catch {
      /* ignore bad chunk */
    }
  }
}

/**
 * Parse Flask SSE (data: {...}\\n\\n) from fetch body — streams tokens like a one-way socket.
 * Handles CRLF, comment-only events (`:` lines), and a trailing `data:` line without final blank line.
 */
async function* parseSSEStream(body) {
  const reader = body.getReader();
  const dec = new TextDecoder();
  let buf = "";
  try {
    while (true) {
      const { done, value } = await reader.read();
      if (value) buf += dec.decode(value, { stream: true });
      const { events, rest } = consumeSSEBlocks(buf);
      buf = rest;
      for (const ev of events) yield ev;
      if (done) break;
    }
    const tail = buf.replace(/\r\n/g, "\n").trim();
    if (tail) {
      const line = tail.split("\n").find((l) => l.startsWith("data:"));
      if (line) {
        const json = line.replace(/^data:\s*/i, "").trim();
        if (json) {
          try {
            yield JSON.parse(json);
          } catch {
            /* ignore */
          }
        }
      }
    }
  } finally {
    reader.releaseLock();
  }
}

function renderTurns(messages, logContainer = logEl) {
  const box = logContainer || logEl;
  if (!box) return;
  box.replaceChildren();
  if (!messages || !messages.length) {
    const empty = document.createElement("div");
    empty.className = "chat-empty";
    empty.textContent =
      "No conversation yet. Type a message below to reach your M4 engine backend.";
    box.appendChild(empty);
    return;
  }

  for (const m of messages) {
    const role = m.role === "assistant" ? "assistant" : "user";
    const content = m.content ?? "";
    const time = formatTurnTime(m.timestamp);
    const wrap = document.createElement("div");
    wrap.className = `msg ${role === "user" ? "user" : "bot"}`;

    const inner = document.createElement("div");
    inner.className = "msg-inner";

    const meta = document.createElement("div");
    meta.className = "msg-meta";

    const roleSpan = document.createElement("span");
    roleSpan.className = "msg-role";
    roleSpan.textContent = role === "user" ? "You" : "Assistant";

    const timeSpan = document.createElement("span");
    timeSpan.className = "msg-time";
    timeSpan.textContent = time;

    meta.appendChild(roleSpan);
    meta.appendChild(timeSpan);

    if (role === "assistant") {
      const badge = document.createElement("span");
      badge.className = "source-badge";
      badge.textContent = normalizeSource(m.source);
      meta.appendChild(badge);
    }

    const body = document.createElement("div");
    body.className = "turn-body";
    body.textContent = content;

    inner.appendChild(meta);
    inner.appendChild(body);
    wrap.appendChild(inner);
    box.appendChild(wrap);
  }
  box.scrollTop = box.scrollHeight;
}

async function fetchJson(path, opts = {}) {
  const r = await fetch(`${API_BASE}${path}`, {
    ...opts,
    headers: { "Content-Type": "application/json", ...authHeaders(), ...opts.headers },
  });
  const data = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(data.message || data.error || data.hint || r.statusText);
  return data;
}

async function fetchStoreJson(path, opts = {}) {
  const r = await fetch(`${API_BASE}${path}`, {
    ...opts,
    headers: { "Content-Type": "application/json", ...storeCartHeaders(), ...opts.headers },
  });
  const data = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(data.message || data.error || data.hint || r.statusText);
  return data;
}

function mayUseStoreCart() {
  const t = getToken().trim();
  if (!t) return true;
  return String(_sessionUser?.role || "").toUpperCase() === "USER";
}

function ensureStorefrontProductsBindings() {
  if (_storefrontBindingsDone) return;
  _storefrontBindingsDone = true;
  document.getElementById("product-category-filter")?.addEventListener("change", () => {
    void renderStorefrontProductsPage();
  });
  document.getElementById("product-store-reload")?.addEventListener("click", () => {
    void renderStorefrontProductsPage();
  });
}

async function renderStorefrontProductsPage() {
  const root = document.getElementById("product-store-root");
  const errEl = document.getElementById("product-store-error");
  const staffNotice = document.getElementById("store-staff-notice");
  const catSel = document.getElementById("product-category-filter");
  if (!root || !catSel) return;

  await refreshAuthStatus();
  const allowCart = mayUseStoreCart();
  if (staffNotice) staffNotice.hidden = allowCart;

  root.innerHTML = "<p class=\"store-loading\">Loading catalog…</p>";
  if (errEl) {
    errEl.hidden = true;
    errEl.textContent = "";
  }

  try {
    const cat = catSel.value || "";
    const qs = cat ? `?category=${encodeURIComponent(cat)}` : "";
    const data = await fetchStoreJson(`/api/store/products${qs}`);
    const products = data.products || [];
    const categories = data.categories || [];

    const keep = catSel.value;
    catSel.innerHTML = `<option value="">All</option>${categories.map((c) => `<option value="${escapeHtml(c)}">${escapeHtml(c)}</option>`).join("")}`;
    catSel.value = categories.includes(keep) ? keep : "";

    if (!products.length) {
      root.innerHTML = "<p class=\"store-empty\">No published products in this category.</p>";
      return;
    }

    root.innerHTML = "";
    for (const p of products) {
      const card = document.createElement("article");
      card.className = "store-product-card";
      const imgHtml = p.image_url
        ? `<div class="store-product-img-wrap"><img class="store-product-img" src="${escapeHtml(p.image_url)}" alt="" loading="lazy" /></div>`
        : `<div class="store-product-img-ph" aria-hidden="true">No image</div>`;
      card.innerHTML = `
        ${imgHtml}
        <div class="store-product-body">
          <div class="store-product-sku"><code>${escapeHtml(p.sku)}</code></div>
          <h3 class="store-product-name">${escapeHtml(p.name)}</h3>
          <p class="store-product-meta">${escapeHtml(p.category || "—")} · stock ${escapeHtml(String(p.quantity_available))}</p>
          <p class="store-product-price">$${escapeHtml(Number(p.price).toFixed(2))}</p>
          <div class="store-product-actions">
            <label class="store-qty-label">Qty <input type="number" class="store-qty-input" min="1" max="${escapeHtml(String(p.quantity_available))}" value="1" aria-label="Quantity for ${escapeHtml(p.name)}" /></label>
            <button type="button" class="btn-add-cart" data-product-id="${escapeHtml(p.id)}" ${allowCart ? "" : "disabled"}>Add to cart</button>
          </div>
        </div>`;
      root.appendChild(card);
    }

    root.querySelectorAll(".btn-add-cart").forEach((btn) => {
      btn.addEventListener("click", async () => {
        if (!mayUseStoreCart()) return;
        const id = btn.getAttribute("data-product-id");
        const card = btn.closest(".store-product-card");
        const inp = card?.querySelector(".store-qty-input");
        const qty = Math.max(1, parseInt(String(inp?.value || "1"), 10) || 1);
        try {
          const cur = await fetchStoreJson("/api/store/cart");
          const lines = Array.isArray(cur.lines) ? cur.lines.map((x) => ({ product_id: x.product_id, qty: x.qty })) : [];
          const ix = lines.findIndex((l) => l.product_id === id);
          if (ix >= 0) lines[ix].qty += qty;
          else lines.push({ product_id: id, qty });
          await fetchStoreJson("/api/store/cart", { method: "PUT", body: JSON.stringify({ lines }) });
          btn.textContent = "Added ✓";
          setTimeout(() => {
            btn.textContent = "Add to cart";
          }, 1200);
        } catch (e) {
          alert(e.message || String(e));
        }
      });
    });
  } catch (e) {
    root.innerHTML = "";
    if (errEl) {
      errEl.hidden = false;
      errEl.textContent = e.message || String(e);
    }
  }
}

let _cartDraft = [];

function setCartActiveView(view) {
  const cartCol = document.getElementById("cart-panel-column-cart");
  const histCol = document.getElementById("cart-panel-column-history");
  const tabCart = document.getElementById("cart-tab-cart");
  const tabHist = document.getElementById("cart-tab-history");
  if (!cartCol || !histCol || !tabCart || !tabHist) return;
  const v = view === "history" ? "history" : "cart";
  cartCol.classList.toggle("is-active", v === "cart");
  histCol.classList.toggle("is-active", v === "history");
  tabCart.classList.toggle("is-active", v === "cart");
  tabHist.classList.toggle("is-active", v === "history");
  tabCart.setAttribute("aria-selected", v === "cart" ? "true" : "false");
  tabHist.setAttribute("aria-selected", v === "history" ? "true" : "false");
  tabCart.tabIndex = v === "cart" ? 0 : -1;
  tabHist.tabIndex = v === "history" ? 0 : -1;
  if (v === "history") histCol.focus();
}

function bindCartViewTabsOnce() {
  const shell = document.getElementById("cart-customer-shell");
  if (!shell || shell.dataset.viewTabsBound) return;
  shell.dataset.viewTabsBound = "1";
  shell.querySelector(".cart-view-tabs")?.addEventListener("click", (ev) => {
    const btn = ev.target.closest("[data-cart-view]");
    if (!btn || !shell.contains(btn)) return;
    const raw = btn.getAttribute("data-cart-view");
    if (raw === "cart" || raw === "history") setCartActiveView(raw);
  });
}

bindCartViewTabsOnce();

async function renderCartPage() {
  const hint = document.getElementById("cart-context-hint");
  const staff = document.getElementById("cart-staff-block");
  const panel = document.getElementById("cart-panel");
  const wrap = document.getElementById("cart-table-wrap");
  const subEl = document.getElementById("cart-subtotal");
  const shell = document.getElementById("cart-customer-shell");
  if (!hint || !staff || !panel || !wrap) return;

  await refreshAuthStatus();
  if (!mayUseStoreCart()) {
    staff.hidden = false;
    panel.hidden = true;
    if (shell) shell.hidden = true;
    hint.textContent = "Cart is not available for company staff accounts.";
    wrap.innerHTML = "";
    return;
  }

  if (shell) shell.hidden = false;
  setCartActiveView("cart");
  await renderCartOrdersInline();

  staff.hidden = true;
  panel.hidden = false;
  hint.textContent = getToken().trim()
    ? "Signed in as customer — cart is tied to your account."
    : "Guest cart — cart is tied to this browser (header X-Guest-Cart-Id).";

  try {
    const data = await fetchStoreJson("/api/store/cart");
    _cartDraft = (data.lines || []).map((l) => ({
      product_id: l.product_id,
      sku: l.sku,
      name: l.name,
      qty: l.qty,
      unit_price: l.unit_price,
    }));
    renderCartTableInner(wrap, subEl);
  } catch (e) {
    panel.hidden = true;
    hint.textContent = e.message || String(e);
  }
}

function renderCartTableInner(wrap, subEl) {
  if (!_cartDraft.length) {
    wrap.innerHTML = "<p class=\"cart-empty\">Your cart is empty.</p>";
    if (subEl) subEl.textContent = "";
    return;
  }
  const rows = _cartDraft
    .map(
      (l, i) =>
        `<tr>
          <td><code>${escapeHtml(l.sku)}</code></td>
          <td>${escapeHtml(l.name)}</td>
          <td>$${escapeHtml(Number(l.unit_price).toFixed(2))}</td>
          <td><input type="number" class="cart-qty-input" data-ci="${i}" min="0" value="${escapeHtml(String(l.qty))}" aria-label="Quantity" /></td>
          <td>$${escapeHtml((Number(l.unit_price) * l.qty).toFixed(2))}</td>
        </tr>`,
    )
    .join("");
  wrap.innerHTML = `<table class="admin-data-table cart-lines-table"><thead><tr><th>SKU</th><th>Item</th><th>Each</th><th>Qty</th><th>Line</th></tr></thead><tbody>${rows}</tbody></table>`;
  const sub = _cartDraft.reduce((s, l) => s + Number(l.unit_price) * l.qty, 0);
  if (subEl) subEl.textContent = `Subtotal: $${sub.toFixed(2)}`;
}

function bindCartSaveOnce() {
  const btn = document.getElementById("cart-save-btn");
  if (!btn || btn.dataset.bound) return;
  btn.dataset.bound = "1";
  btn.addEventListener("click", async () => {
    const wrap = document.getElementById("cart-table-wrap");
    const subEl = document.getElementById("cart-subtotal");
    if (!wrap) return;
    wrap.querySelectorAll(".cart-qty-input").forEach((inp) => {
      const i = parseInt(inp.getAttribute("data-ci") || "-1", 10);
      if (i >= 0 && _cartDraft[i]) _cartDraft[i].qty = Math.max(0, parseInt(String(inp.value), 10) || 0);
    });
    const lines = _cartDraft.filter((l) => l.qty > 0).map((l) => ({ product_id: l.product_id, qty: l.qty }));
    try {
      const data = await fetchStoreJson("/api/store/cart", { method: "PUT", body: JSON.stringify({ lines }) });
      _cartDraft = (data.lines || []).map((l) => ({
        product_id: l.product_id,
        sku: l.sku,
        name: l.name,
        qty: l.qty,
        unit_price: l.unit_price,
      }));
      renderCartTableInner(wrap, subEl);
    } catch (e) {
      alert(e.message || String(e));
    }
  });
}

bindCartSaveOnce();

function bindCartCheckoutOnce() {
  const btn = document.getElementById("cart-checkout-btn");
  if (!btn || btn.dataset.bound) return;
  btn.dataset.bound = "1";
  btn.addEventListener("click", async () => {
    if (!mayUseStoreCart()) return;
    const st = document.getElementById("cart-checkout-status");
    const noteEl = document.getElementById("cart-checkout-note");
    const emailEl = document.getElementById("cart-checkout-contact-email");
    const note = (noteEl?.value || "").trim();
    const guest = !getToken().trim();
    const contact_email = guest ? (emailEl?.value || "").trim() : "";
    try {
      const body = {};
      if (note) body.delivery_note = note;
      if (guest && contact_email) body.contact_email = contact_email;
      const data = await fetchStoreJson("/api/store/checkout", {
        method: "POST",
        body: JSON.stringify(body),
      });
      if (st) st.textContent = `Order ${data.order?.id || ""} created — status: ${data.order?.status || ""}.`;
      const wrap = document.getElementById("cart-table-wrap");
      const subEl = document.getElementById("cart-subtotal");
      _cartDraft = [];
      renderCartTableInner(wrap, subEl);
      if (noteEl) noteEl.value = "";
      if (emailEl) emailEl.value = "";
      void renderCartOrdersInline();
    } catch (e) {
      if (st) st.textContent = e.message || String(e);
      else alert(e.message || String(e));
    }
  });
}

bindCartCheckoutOnce();

function bindStoreOrdersConfirmDeliveryOnce() {
  const root = document.getElementById("route-cart");
  if (!root || root.dataset.confirmDeliveryBound) return;
  root.dataset.confirmDeliveryBound = "1";
  root.addEventListener("click", async (ev) => {
    const btn = ev.target.closest("[data-store-confirm-delivery]");
    if (!btn || !root.contains(btn)) return;
    const oid = btn.getAttribute("data-store-confirm-delivery");
    if (!oid) return;
    btn.disabled = true;
    try {
      await fetchJson(`/api/store/orders/${encodeURIComponent(oid)}/confirm-delivery`, {
        method: "POST",
        body: "{}",
      });
      await renderCartOrdersInline();
    } catch (e) {
      alert(e.message || String(e));
      btn.disabled = false;
    }
  });
}

bindStoreOrdersConfirmDeliveryOnce();

async function renderCartOrdersInline() {
  const hint = document.getElementById("cart-orders-hint");
  const wrap = document.getElementById("cart-orders-wrap");
  if (!hint || !wrap) return;

  const isUser =
    getToken().trim() && String(_sessionUser?.role || "").trim().toUpperCase() === "USER";
  if (!isUser) {
    hint.textContent =
      "Sign in with a customer account to see your order history here. Guests can still check out below.";
    wrap.innerHTML = "";
    return;
  }

  hint.textContent =
    "Orders placed while signed in. When an order is shipped, confirm receipt with the button in the table.";

  try {
    const data = await fetchJson("/api/store/orders");
    const orders = data.orders || [];
    if (!orders.length) {
      wrap.innerHTML = "<p class=\"cart-empty\">No orders yet.</p>";
      return;
    }
    const rows = orders
      .map((o) => {
        const lines = (o.lines || []).map((l) => `${l.qty}× ${l.sku || l.name || ""}`).join("; ");
        const shipped = String(o.status || "") === "shipped";
        const action = shipped
          ? `<button type="button" class="btn-login-primary" data-store-confirm-delivery="${escapeHtml(o.id)}" title="Mark this order as received">I received my order</button>`
          : "—";
        return `<tr>
        <td><code>${escapeHtml(o.id || "")}</code></td>
        <td>${escapeHtml(o.status || "")}</td>
        <td>$${escapeHtml(Number(o.subtotal || 0).toFixed(2))}</td>
        <td>${escapeHtml(String(o.created_at || "—"))}</td>
        <td>${escapeHtml(lines || "—")}</td>
        <td class="orders-action-cell">${action}</td>
      </tr>`;
      })
      .join("");
    wrap.innerHTML = `<table class="admin-data-table"><thead><tr><th>Id</th><th>Status</th><th>Total</th><th>Created</th><th>Lines</th><th>Receipt</th></tr></thead><tbody>${rows}</tbody></table>`;
  } catch (e) {
    wrap.innerHTML = `<p class="cart-empty">${escapeHtml(e.message || String(e))}</p>`;
  }
}

function orderDeskNextActions(order, actorUser) {
  const R = String(actorUser?.role || "").trim().toUpperCase();
  const D = String(actorUser?.department || "").trim().toLowerCase();
  const s = String(order.status || "");
  const admin = R === "ADMIN";
  const finance = admin || R === "FINANCE" || D === "finance";
  const storage = admin || R === "STORAGE" || D === "storage";
  const delivery = admin || R === "DELIVERY" || isDeliveryDepartment(actorUser?.department);
  const myId = String(actorUser?.id || "").trim();
  const assignee = String(order.delivery_assignee_user_id || "").trim();
  const assignedToMe = Boolean(myId && assignee && assignee === myId);
  const out = [];
  if (s === "pending_confirmation" && finance) {
    out.push({
      label: "Approve order",
      status: "confirmed",
      title: "Confirm payment and send this order to the warehouse for packing.",
    });
    out.push({
      label: "Reject order",
      status: "rejected",
      title: "Decline the order. It will not be fulfilled.",
    });
  }
  if (s === "confirmed" && storage) {
    out.push({
      label: "Pack order",
      status: "packed",
      title: "Mark items picked and packed. Warehouse stock for this order will be reduced.",
    });
  }
  if (s === "packed" && delivery && (admin || assignedToMe)) {
    out.push({
      label: "Mark shipped",
      status: "shipped",
      title: "Order has left the warehouse or been handed to the carrier.",
    });
  }
  if (s === "shipped" && admin) {
    out.push({
      label: "Mark delivered (override)",
      status: "delivered",
      title:
        "Close without customer confirmation — guest orders, or if the buyer cannot confirm in the app. Customers use “I received it” on the Cart page.",
    });
  }
  return out;
}

function deliveryAssigneeNameCellHtml(o) {
  const name = (o.delivery_assignee_name && String(o.delivery_assignee_name).trim()) || "";
  const aid = o.delivery_assignee_user_id ? String(o.delivery_assignee_user_id) : "";
  if (!aid) {
    return `<td class="adm-delivery-name" title="Unassigned">—</td>`;
  }
  const display = name || "Unknown user";
  const tip = name ? `${name} · id ${aid}` : aid;
  return `<td class="adm-delivery-name" title="${escapeHtml(tip)}">${escapeHtml(display)}</td>`;
}

function deliveryAssigneeActionsCellHtml(o, actorUser, candidates) {
  const aid = o.delivery_assignee_user_id ? String(o.delivery_assignee_user_id) : "";
  const R = String(actorUser?.role || "").trim().toUpperCase();
  const delivery = R === "DELIVERY" || isDeliveryDepartment(actorUser?.department);
  const admin = R === "ADMIN";
  const override = canOverrideDeliveryAssignee(actorUser);
  const myId = String(actorUser?.id || "").trim();
  const s = String(o.status || "");
  const canSelfSvc = s === "packed" || s === "shipped";

  let inner = "—";
  if (override) {
    const opts = [`<option value="">Unassigned</option>`].concat(
      (candidates || []).map((u) => {
        const label =
          [u.name, u.email].filter(Boolean).join(" — ") || u.id;
        return `<option value="${escapeHtml(u.id)}"${u.id === aid ? " selected" : ""}>${escapeHtml(
          label,
        )}</option>`;
      }),
    );
    inner = `<div class="adm-delivery-ctl"><select class="store-filter-select adm-order-assign-sel" data-adm-order-assign-sel data-oid="${escapeHtml(o.id)}" aria-label="Assign delivery">${opts.join("")}</select>
      <button type="button" class="btn-admin-mini btn-admin-action" data-adm-order-assign-apply data-oid="${escapeHtml(o.id)}">Assign</button></div>`;
  } else if (delivery && canSelfSvc && !admin) {
    if (!aid && s === "packed") {
      inner = `<button type="button" class="btn-admin-mini btn-admin-action" data-adm-order-take data-oid="${escapeHtml(o.id)}">Take order</button>`;
    } else if (aid === myId) {
      inner = `<button type="button" class="btn-admin-mini" data-adm-order-release data-oid="${escapeHtml(o.id)}">Release</button>`;
    }
  }
  return `<td class="adm-delivery-actions">${inner}</td>`;
}

function adminOrdersDeskInnerHtml() {
  return `
        <div class="admin-storage-toolbar">
          <label class="admin-storage-field">Status filter
            <select id="adm-order-status" class="store-filter-select">
              <option value="">All (for my role)</option>
              <option value="pending_confirmation">pending_confirmation</option>
              <option value="confirmed">confirmed</option>
              <option value="rejected">rejected</option>
              <option value="packed">packed</option>
              <option value="shipped">shipped</option>
              <option value="delivered">delivered</option>
            </select>
          </label>
          <button type="button" class="btn-store-reload" id="adm-order-reload" title="Reload order list with current status filter">Refresh orders</button>
        </div>
        <p class="admin-bulk-hint" id="adm-order-delivery-hint" hidden></p>
        <div class="admin-table-wrap"><table class="admin-data-table"><thead><tr><th>Id</th><th>Status</th><th>Customer</th><th>Assigned to</th><th>Assign</th><th>Total</th><th>Created</th><th>Next step</th></tr></thead><tbody id="adm-order-tbody"><tr><td colspan="8">Loading…</td></tr></tbody></table></div>`;
}

function adminFinanceChartsOnlyInnerHtml() {
  const statusOpts = [
    ["", "All statuses"],
    ["pending_confirmation", "pending_confirmation"],
    ["confirmed", "confirmed"],
    ["rejected", "rejected"],
    ["packed", "packed"],
    ["shipped", "shipped"],
    ["delivered", "delivered"],
  ]
    .map(([v, lab]) => `<option value="${escapeHtml(v)}">${escapeHtml(lab)}</option>`)
    .join("");
  return `
        <div class="fin-desk-overview">
          <h4 class="admin-storage-sub">Flow overview</h4>
          <p class="admin-bulk-hint">Filters below apply to <strong>bar chart</strong>, <strong>daily trend</strong>, and <strong>CSV export</strong> (order date = <code>created_at</code>, UTC). The <strong>pie</strong> always uses the <strong>current UTC calendar year</strong> (all statuses); customer type filter still applies.</p>
          <p class="admin-bulk-hint fin-cross-links">To work the <strong>order queue</strong> (approve, reject, etc.), open <button type="button" class="btn-admin-mini btn-admin-action" data-admin-jump="finance_orders">Finance → Orders</button> in the sidebar.</p>
          <div class="admin-storage-toolbar fin-filters-toolbar">
            <label class="admin-storage-field">From (UTC)
              <input type="date" id="adm-fin-from" class="adm-prod-inp" />
            </label>
            <label class="admin-storage-field">To
              <input type="date" id="adm-fin-to" class="adm-prod-inp" />
            </label>
            <label class="admin-storage-field">Status
              <select id="adm-fin-f-status" class="store-filter-select">${statusOpts}</select>
            </label>
            <label class="admin-storage-field">Customer
              <select id="adm-fin-f-cust" class="store-filter-select">
                <option value="">All</option>
                <option value="user">Registered</option>
                <option value="guest">Guest</option>
              </select>
            </label>
            <button type="button" class="btn-store-reload" id="adm-fin-refresh" title="Reload charts from API">Refresh charts</button>
            <button type="button" class="btn-admin-mini btn-admin-action" id="adm-fin-export" title="Download CSV (UTF-8, Excel-friendly)">Export CSV</button>
          </div>
          <p class="fin-export-status" id="adm-fin-export-status" role="status" aria-live="polite"></p>
          <div id="adm-fin-kpis" class="fin-kpis" aria-live="polite"></div>
          <div class="fin-charts-grid">
            <div class="fin-chart-card fin-chart-card--pie">
              <h5 class="fin-chart-title" id="adm-fin-pie-title">Orders by status (year)</h5>
              <div id="adm-fin-chart-pie-ytd" class="fin-chart-body fin-pie-host" aria-live="polite"></div>
            </div>
            <div class="fin-chart-card">
              <h5 class="fin-chart-title">Subtotal by status</h5>
              <div id="adm-fin-chart-status" class="fin-chart-body"></div>
            </div>
            <div class="fin-chart-card">
              <h5 class="fin-chart-title">Subtotal by day (UTC)</h5>
              <div id="adm-fin-chart-day" class="fin-chart-body fin-chart-body--days"></div>
            </div>
          </div>
        </div>`;
}

function readFinanceFiltersFromMain(main) {
  return {
    from: main.querySelector("#adm-fin-from")?.value?.trim() || "",
    to: main.querySelector("#adm-fin-to")?.value?.trim() || "",
    status: main.querySelector("#adm-fin-f-status")?.value?.trim() || "",
    customer_type: main.querySelector("#adm-fin-f-cust")?.value?.trim() || "",
  };
}

function financeFiltersQueryString(f) {
  const qs = new URLSearchParams();
  if (f.from) qs.set("from", f.from);
  if (f.to) qs.set("to", f.to);
  if (f.status) qs.set("status", f.status);
  if (f.customer_type) qs.set("customer_type", f.customer_type);
  return qs.toString();
}

/** Slice colors for pie — distinct hues per lifecycle stage. */
const FIN_PIE_STATUS_COLORS = {
  pending_confirmation: "hsl(38, 88%, 52%)",
  confirmed: "hsl(152, 52%, 42%)",
  rejected: "hsl(0, 62%, 50%)",
  packed: "hsl(210, 72%, 48%)",
  shipped: "hsl(268, 58%, 54%)",
  delivered: "hsl(165, 45%, 36%)",
};

function finPieColorForStatus(status) {
  const k = String(status || "");
  return FIN_PIE_STATUS_COLORS[k] || "hsl(220, 8%, 55%)";
}

function renderFinanceYearPie(main, ytdData, utcYear) {
  const host = main.querySelector("#adm-fin-chart-pie-ytd");
  const titleEl = main.querySelector("#adm-fin-pie-title");
  if (titleEl) titleEl.textContent = `Orders by status (${utcYear}, UTC)`;
  if (!host) return;
  const rows = ytdData.by_status || [];
  const total = rows.reduce((s, r) => s + (Number(r.count) || 0), 0);
  if (!total) {
    host.innerHTML = "<p class=\"admin-bulk-hint\">No orders in this year for the selected customer filter.</p>";
    return;
  }
  let acc = 0;
  const parts = [];
  for (const r of rows) {
    const c = Number(r.count) || 0;
    if (!c) continue;
    const pct = (c / total) * 100;
    const start = acc;
    acc += pct;
    const col = finPieColorForStatus(r.status);
    parts.push(`${col} ${start.toFixed(4)}% ${acc.toFixed(4)}%`);
  }
  const gradient = parts.length ? `conic-gradient(${parts.join(", ")})` : "transparent";
  const aria = rows
    .filter((r) => (Number(r.count) || 0) > 0)
    .map((r) => `${r.status}: ${r.count} orders (${((Number(r.count) / total) * 100).toFixed(1)}%)`)
    .join("; ");
  const legend = rows
    .filter((r) => (Number(r.count) || 0) > 0)
    .map((r) => {
      const col = finPieColorForStatus(r.status);
      const pct = ((Number(r.count) / total) * 100).toFixed(1);
      return `<li class="fin-pie-legend-item">
        <span class="fin-pie-swatch" style="background:${col}"></span>
        <span class="fin-pie-legend-text"><strong>${escapeHtml(r.status)}</strong> · ${escapeHtml(String(r.count))} (${escapeHtml(pct)}%) · $${escapeHtml(Number(r.subtotal_sum).toFixed(2))}</span>
      </li>`;
    })
    .join("");
  host.innerHTML = `<div class="fin-pie-wrap">
    <div class="fin-pie-disk" style="background:${gradient}" role="img" aria-label="${escapeHtml(aria)}"></div>
    <ul class="fin-pie-legend">${legend}</ul>
  </div>`;
}

function renderFinanceCharts(main, data, ytdData, utcYear) {
  const kpis = main.querySelector("#adm-fin-kpis");
  const stEl = main.querySelector("#adm-fin-chart-status");
  const dyEl = main.querySelector("#adm-fin-chart-day");
  renderFinanceYearPie(main, ytdData, utcYear);
  if (kpis) {
    kpis.innerHTML = `<div class="fin-kpi-row">
      <span class="fin-kpi"><strong>${escapeHtml(String(data.total_orders ?? 0))}</strong> orders</span>
      <span class="fin-kpi"><strong>$${escapeHtml(Number(data.total_subtotal || 0).toFixed(2))}</strong> subtotal (filtered)</span>
    </div>`;
  }
  const byS = data.by_status || [];
  const maxS = Math.max(...byS.map((x) => Number(x.subtotal_sum) || 0), 1e-9);
  if (stEl) {
    stEl.innerHTML = byS.length
      ? byS
          .map(
            (r) => `<div class="fin-bar-row">
        <span class="fin-bar-label">${escapeHtml(r.status)}</span>
        <div class="fin-bar-track" role="img" aria-label="${escapeHtml(r.status)} ${Number(r.subtotal_sum).toFixed(2)} dollars"><div class="fin-bar-fill" style="width:${(Number(r.subtotal_sum) / maxS) * 100}%"></div></div>
        <span class="fin-bar-val">$${escapeHtml(Number(r.subtotal_sum).toFixed(2))} · ${escapeHtml(String(r.count))} orders</span>
      </div>`,
          )
          .join("")
      : "<p class=\"admin-bulk-hint\">No orders match these filters.</p>";
  }
  const byD = data.by_day || [];
  const maxD = Math.max(...byD.map((x) => Number(x.subtotal_sum) || 0), 1e-9);
  if (dyEl) {
    dyEl.innerHTML = byD.length
      ? `<div class="fin-day-bars">${byD
          .map((r) => {
            const h = Math.max(6, (Number(r.subtotal_sum) / maxD) * 120);
            return `<div class="fin-day-col" title="${escapeHtml(r.day)}: $${Number(r.subtotal_sum).toFixed(2)} (${r.count})">
          <div class="fin-day-bar" style="height:${h}px"></div>
          <span class="fin-day-lbl">${escapeHtml(String(r.day || "").slice(5))}</span>
        </div>`;
          })
          .join("")}</div>`
      : "<p class=\"admin-bulk-hint\">No daily buckets (widen date range or add orders).</p>";
  }
}

async function loadFinanceSummary(main) {
  const f = readFinanceFiltersFromMain(main);
  const q = financeFiltersQueryString(f);
  const utcYear = new Date().getUTCFullYear();
  const ytdQs = new URLSearchParams();
  ytdQs.set("from", `${utcYear}-01-01`);
  ytdQs.set("to", `${utcYear}-12-31`);
  if (f.customer_type) ytdQs.set("customer_type", f.customer_type);
  const [data, ytdData] = await Promise.all([
    fetchJson(`/api/admin/finance/summary${q ? `?${q}` : ""}`),
    fetchJson(`/api/admin/finance/summary?${ytdQs.toString()}`),
  ]);
  renderFinanceCharts(main, data, ytdData, utcYear);
}

async function downloadFinanceExport(main) {
  const st = main.querySelector("#adm-fin-export-status");
  const f = readFinanceFiltersFromMain(main);
  const q = financeFiltersQueryString(f);
  const r = await fetch(`${API_BASE}/api/admin/finance/export${q ? `?${q}` : ""}`, {
    headers: { ...authHeaders() },
  });
  if (!r.ok) {
    const err = await r.json().catch(() => ({}));
    throw new Error(err.message || err.error || r.statusText);
  }
  const blob = await r.blob();
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = `orders_export_${new Date().toISOString().slice(0, 10)}.csv`;
  a.rel = "noopener";
  document.body.appendChild(a);
  a.click();
  a.remove();
  URL.revokeObjectURL(url);
  if (st) st.textContent = "Export downloaded.";
}

function adminBotCLibDeskInnerHtml() {
  return `
    <p class="admin-bulk-hint">Each field is a supported option key (schema from <code>GET /api/admin/bot-c-lib/schema</code>, backed by <code>training/full_options.py</code>). Values are stored in Mongo <code>bot_c_lib_settings</code> only. The <strong>running</strong> chat engine still uses your process environment; restart the server (or sync <code>.env</code>) to match what you saved.</p>
    <p class="admin-bulk-hint"><strong>Server profile:</strong> <span id="adm-bot-clib-profile"></span></p>
    <p id="adm-bot-clib-msg" class="admin-bulk-hint" role="status"></p>
    <div class="admin-storage-toolbar">
      <button type="button" class="btn-admin-mini btn-admin-action" id="adm-bot-clib-save">Save overrides to DB</button>
      <button type="button" class="btn-store-reload" id="adm-bot-clib-refresh">Refresh</button>
    </div>
    <form id="adm-bot-clib-form" class="adm-bot-clib-form adm-prod-form"></form>
    <h4 class="admin-storage-sub">Effective preview (env + saved)</h4>
    <pre class="adm-pol-bulk adm-bot-clib-json" id="adm-bot-clib-effective"></pre>`;
}

function renderBotCLibFields(main, data) {
  const form = main.querySelector("#adm-bot-clib-form");
  const pre = main.querySelector("#adm-bot-clib-effective");
  const profile = main.querySelector("#adm-bot-clib-profile");
  if (!form) return;
  const schema = data.schema || [];
  const saved = data.saved_overrides || {};
  const base = data.base_from_env || {};
  const eff = data.effective_preview || {};
  if (profile) {
    profile.innerHTML = data.use_max_options
      ? "<code>M4ENGINE_SERVER_MAX</code> → <strong>max</strong> defaults + env + DB"
      : "<code>M4ENGINE_SERVER_MAX=0</code> → <strong>full</strong> env + DB";
  }
  if (pre) pre.textContent = JSON.stringify(eff, null, 2);
  form.innerHTML = schema
    .map((row) => {
      const k = row.key;
      const typ = row.type || "string";
      const env = escapeHtml(row.env || "");
      const lab = escapeHtml(row.label || k);
      const help = escapeHtml(row.help || "");
      const hasOwn = Object.prototype.hasOwnProperty.call(saved, k);
      let control = "";
      const defStr = base[k] === undefined || base[k] === null ? "" : String(base[k]);
      if (typ === "int") {
        const v = hasOwn && saved[k] !== null && saved[k] !== undefined ? String(saved[k]) : "";
        const mn = row.min != null ? ` min="${row.min}"` : "";
        const mx = row.max != null ? ` max="${row.max}"` : "";
        control = `<input type="number" name="${escapeHtml(k)}" class="adm-prod-inp"${mn}${mx} value="${escapeHtml(v)}" placeholder="default: ${escapeHtml(defStr)}" />`;
      } else if (typ === "bool_int") {
        const sel = !hasOwn ? "" : String(saved[k] === 1 || saved[k] === true ? "1" : "0");
        control = `<select name="${escapeHtml(k)}" class="store-filter-select">
          <option value=""${sel === "" ? " selected" : ""}>Default (env): ${escapeHtml(defStr || "0")}</option>
          <option value="0"${sel === "0" ? " selected" : ""}>Off (0)</option>
          <option value="1"${sel === "1" ? " selected" : ""}>On (1)</option>
        </select>`;
      } else {
        const v = hasOwn && saved[k] != null ? String(saved[k]) : "";
        control = `<input type="text" name="${escapeHtml(k)}" class="adm-prod-inp adm-prod-inp--wide" value="${escapeHtml(v)}" placeholder="${escapeHtml(defStr)}" />`;
      }
      return `<label class="admin-storage-field adm-bot-clib-row"><span>${lab} <code class="admin-bulk-hint">${env}</code></span><span class="admin-bulk-hint">${help}</span>${control}</label>`;
    })
    .join("");
}

function collectBotCLibPayload(form, schema) {
  const values = {};
  for (const row of schema) {
    const k = row.key;
    const typ = row.type || "string";
    const el = form.querySelector(`[name="${k}"]`);
    if (!el) continue;
    if (typ === "bool_int") {
      const s = String(el.value || "");
      if (s === "") values[k] = null;
      else values[k] = s === "1" ? 1 : 0;
      continue;
    }
    if (typ === "int") {
      const s = String(el.value || "").trim();
      if (s === "") values[k] = null;
      else values[k] = parseInt(s, 10);
      continue;
    }
    const s = String(el.value || "").trim();
    if (s === "") values[k] = null;
    else values[k] = s;
  }
  return values;
}

async function loadBotCLibDesk(main) {
  const data = await fetchJson("/api/admin/bot-c-lib/settings");
  main.dataset.botClibSchema = JSON.stringify(data.schema || []);
  renderBotCLibFields(main, data);
}

function bindBotCLibDesk(main, signal) {
  const opt = signal ? { signal } : undefined;
  const msg = main.querySelector("#adm-bot-clib-msg");
  const show = (t, isErr) => {
    if (msg) {
      msg.textContent = t || "";
      msg.classList.toggle("admin-main-error", Boolean(isErr));
    }
  };
  const go = () =>
    loadBotCLibDesk(main).catch((e) => {
      show(e.message || String(e), true);
    });
  main.querySelector("#adm-bot-clib-refresh")?.addEventListener("click", () => void go(), opt);
  main.querySelector("#adm-bot-clib-save")?.addEventListener(
    "click",
    async () => {
      let schema = [];
      try {
        schema = JSON.parse(main.dataset.botClibSchema || "[]");
      } catch {
        schema = [];
      }
      const form = main.querySelector("#adm-bot-clib-form");
      if (!form) return;
      try {
        const values = collectBotCLibPayload(form, schema);
        const data = await fetchJson("/api/admin/bot-c-lib/settings", {
          method: "PUT",
          body: JSON.stringify({ values, replace: false }),
        });
        show(data.message || "Saved.", false);
        await go();
      } catch (e) {
        show(e.message || String(e), true);
      }
    },
    opt,
  );
  void go();
}

function bindFinanceChartsDesk(main, user, signal) {
  const opt = signal ? { signal } : undefined;
  const refresh = () =>
    loadFinanceSummary(main).catch((e) => {
      const kpis = main.querySelector("#adm-fin-kpis");
      if (kpis) kpis.innerHTML = `<p class="admin-main-error">${escapeHtml(e.message || String(e))}</p>`;
    });
  main.querySelector("#adm-fin-refresh")?.addEventListener("click", () => void refresh(), opt);
  main.querySelector("#adm-fin-export")?.addEventListener(
    "click",
    () => {
      downloadFinanceExport(main).catch((e) => {
        const st = main.querySelector("#adm-fin-export-status");
        if (st) st.textContent = e.message || String(e);
        else alert(e.message || String(e));
      });
    },
    opt,
  );
  void refresh();
}

async function loadAdminOrdersTable(main, user) {
  const st = main.querySelector("#adm-order-status")?.value?.trim() || "";
  const qs = st ? `?status=${encodeURIComponent(st)}` : "";
  const tbody = main.querySelector("#adm-order-tbody");
  if (!tbody) return;

  let candidates = [];
  if (canOverrideDeliveryAssignee(user)) {
    try {
      const d = await fetchJson(`/api/admin/delivery-assignee-candidates`);
      candidates = d.users || [];
    } catch {
      candidates = [];
    }
  }

  let data;
  try {
    data = await fetchJson(`/api/admin/orders${qs}`);
  } catch (e) {
    tbody.innerHTML = `<tr><td colspan="8">${escapeHtml(e.message || String(e))}</td></tr>`;
    updateOrdersDeliveryHint(main, user, candidates);
    return;
  }
  const orders = data.orders || [];
  if (!orders.length) {
    tbody.innerHTML = "<tr><td colspan=\"8\">No orders</td></tr>";
    updateOrdersDeliveryHint(main, user, candidates);
    return;
  }
  tbody.innerHTML = orders
    .map((o) => {
      const cust = o.customer_email || o.contact_email || o.customer_type || "—";
      const actions = orderDeskNextActions(o, user)
        .map((a) => {
          const tip = a.title ? ` title="${escapeHtml(a.title)}" aria-label="${escapeHtml(a.title)}"` : "";
          return `<button type="button" class="btn-admin-mini btn-admin-action"${tip} data-adm-order-act data-oid="${escapeHtml(o.id)}" data-next="${escapeHtml(a.status)}">${escapeHtml(a.label)}</button>`;
        })
        .join(" ");
      return `<tr data-order-id="${escapeHtml(o.id)}">
        <td><code>${escapeHtml(o.id)}</code></td>
        <td>${escapeHtml(o.status)}</td>
        <td>${escapeHtml(String(cust))}</td>
        ${deliveryAssigneeNameCellHtml(o)}
        ${deliveryAssigneeActionsCellHtml(o, user, candidates)}
        <td>$${escapeHtml(Number(o.subtotal || 0).toFixed(2))}</td>
        <td>${escapeHtml(String(o.created_at || "—"))}</td>
        <td class="adm-actions">${actions || "—"}</td>
      </tr>`;
    })
    .join("");
  updateOrdersDeliveryHint(main, user, candidates);
}

function bindAdminOrdersDesk(main, user, signal, opts = {}) {
  const statusSel = main.querySelector("#adm-order-status");
  if (statusSel && opts.defaultStatus) {
    const allowed = new Set([
      "",
      "pending_confirmation",
      "confirmed",
      "rejected",
      "packed",
      "shipped",
      "delivered",
    ]);
    const v = String(opts.defaultStatus);
    if (allowed.has(v)) statusSel.value = v;
  }
  const reload = () =>
    loadAdminOrdersTable(main, user).catch((e) => {
      const tbody = main.querySelector("#adm-order-tbody");
      if (tbody) tbody.innerHTML = `<tr><td colspan="8">${escapeHtml(e.message || String(e))}</td></tr>`;
    });
  const opt = signal ? { signal } : undefined;
  main.querySelector("#adm-order-reload")?.addEventListener("click", () => void reload(), opt);
  main.querySelector("#adm-order-status")?.addEventListener("change", () => void reload(), opt);
  main.addEventListener(
    "click",
    async (ev) => {
      const take = ev.target.closest("[data-adm-order-take]");
      if (take) {
        const oid = take.getAttribute("data-oid");
        const myId = String(user?.id || "").trim();
        if (!oid || !myId) return;
        try {
          await fetchJson(`/api/admin/orders/${encodeURIComponent(oid)}`, {
            method: "PATCH",
            body: JSON.stringify({ delivery_assignee_user_id: myId }),
          });
          await reload();
          if (typeof opts.afterOrdersMutate === "function") opts.afterOrdersMutate();
        } catch (err) {
          alert(err.message || String(err));
        }
        return;
      }
      const rel = ev.target.closest("[data-adm-order-release]");
      if (rel) {
        const oid = rel.getAttribute("data-oid");
        if (!oid) return;
        try {
          await fetchJson(`/api/admin/orders/${encodeURIComponent(oid)}`, {
            method: "PATCH",
            body: JSON.stringify({ delivery_assignee_user_id: null }),
          });
          await reload();
          if (typeof opts.afterOrdersMutate === "function") opts.afterOrdersMutate();
        } catch (err) {
          alert(err.message || String(err));
        }
        return;
      }
      const apply = ev.target.closest("[data-adm-order-assign-apply]");
      if (apply) {
        const oid = apply.getAttribute("data-oid");
        if (!oid) return;
        const sel = main.querySelector(`tr[data-order-id="${oid}"] [data-adm-order-assign-sel]`);
        const v = sel ? String(sel.value || "").trim() : "";
        try {
          await fetchJson(`/api/admin/orders/${encodeURIComponent(oid)}`, {
            method: "PATCH",
            body: JSON.stringify({ delivery_assignee_user_id: v || null }),
          });
          await reload();
          if (typeof opts.afterOrdersMutate === "function") opts.afterOrdersMutate();
        } catch (err) {
          alert(err.message || String(err));
        }
        return;
      }
      const t = ev.target.closest("[data-adm-order-act]");
      if (!t) return;
      const oid = t.getAttribute("data-oid");
      const next = t.getAttribute("data-next");
      if (!oid || !next) return;
      try {
        await fetchJson(`/api/admin/orders/${encodeURIComponent(oid)}`, {
          method: "PATCH",
          body: JSON.stringify({ status: next }),
        });
        await reload();
        if (typeof opts.afterOrdersMutate === "function") opts.afterOrdersMutate();
      } catch (err) {
        alert(err.message || String(err));
      }
    },
    opt,
  );
  void reload();
}

function canWriteProductCatalog(user) {
  const R = String(user?.role || "").toUpperCase();
  return R === "ADMIN" || R === "STORAGE";
}

async function loadAdminStorageTable(main) {
  const cat = main.querySelector("#adm-prod-cat")?.value?.trim() || "";
  const pub = main.querySelector("#adm-prod-pub")?.value?.trim() || "";
  const qs = new URLSearchParams();
  if (cat) qs.set("category", cat);
  if (pub) qs.set("published", pub);
  const q = qs.toString();
  const data = await fetchJson(`/api/admin/products${q ? `?${q}` : ""}`);
  const tbody = main.querySelector("#adm-prod-tbody");
  const catSel = main.querySelector("#adm-prod-cat");
  if (catSel && Array.isArray(data.categories)) {
    const cur = catSel.value;
    catSel.innerHTML = `<option value="">All</option>${data.categories.map((c) => `<option value="${escapeHtml(c)}">${escapeHtml(c)}</option>`).join("")}`;
    catSel.value = data.categories.includes(cur) ? cur : "";
  }
  if (!tbody) return;
  const rows = (data.products || [])
    .map(
      (p) =>
        `<tr data-id="${escapeHtml(p.id)}">
        <td><code>${escapeHtml(p.sku)}</code></td>
        <td>${escapeHtml(p.name)}</td>
        <td>${escapeHtml(p.category || "")}</td>
        <td>${escapeHtml(Number(p.price).toFixed(2))}</td>
        <td>${escapeHtml(String(p.quantity_available))}</td>
        <td>${p.published ? "yes" : "no"}</td>
        <td class="adm-img-cell">${p.image_url ? `<span class="adm-img-yes" title="${escapeHtml(p.image_url)}">yes</span>` : "—"}</td>
        <td class="adm-actions" data-adm-write>
          <button type="button" class="btn-admin-mini btn-admin-action" data-adm-qty title="Change quantity available in the warehouse">Edit stock</button>
          <button type="button" class="btn-admin-mini btn-admin-action" data-adm-flip-pub data-published="${p.published ? "1" : "0"}" title="${p.published ? "Remove from customer-facing catalog" : "Show on the storefront"}">${p.published ? "Hide from store" : "Show in store"}</button>
        </td>
      </tr>`,
    )
    .join("");
  tbody.innerHTML = rows || "<tr><td colspan=\"8\">No rows</td></tr>";
}

function bindAdminStorageDesk(main, user, signal) {
  const write = canWriteProductCatalog(user);
  main.querySelectorAll("[data-adm-write]").forEach((el) => {
    el.hidden = !write;
  });

  const fillCategorySelects = async () => {
    const sel = main.querySelector("#adm-prod-create-category");
    if (!sel) return;
    try {
      const pc = await fetchJson("/api/admin/product-categories");
      const active = (pc.categories || []).filter((c) => c.active !== false);
      const codes = new Set(active.map((c) => c.code));
      if (!codes.has("general")) codes.add("general");
      sel.innerHTML = Array.from(codes)
        .sort()
        .map((c) => {
          const cat = active.find((x) => x.code === c);
          const label = cat ? `${cat.name} (${c})` : c;
          return `<option value="${escapeHtml(c)}">${escapeHtml(label)}</option>`;
        })
        .join("");
    } catch {
      sel.innerHTML = '<option value="general">general</option>';
    }
  };

  const reloadPcats = async () => {
    const tb = main.querySelector("#adm-pcat-tbody");
    if (!tb) return;
    try {
      const pc = await fetchJson("/api/admin/product-categories");
      const rows = (pc.categories || [])
        .map((c) => {
          const act = write
            ? `<td data-adm-write><button type="button" class="btn-admin-mini btn-admin-action" data-adm-pcat-toggle data-pcat-set-active="${c.active ? "0" : "1"}" title="${c.active ? "Stop showing this category on the storefront" : "Allow this category in store filters again"}">${c.active ? "Hide category" : "Enable category"}</button></td>`
            : "<td>—</td>";
          return `<tr data-pcat-code="${escapeHtml(c.code)}"><td><code>${escapeHtml(c.code)}</code></td><td>${escapeHtml(c.name || "")}</td><td>${c.active ? "yes" : "no"}</td>${act}</tr>`;
        })
        .join("");
      tb.innerHTML = rows || "<tr><td colspan=\"4\">No categories</td></tr>";
    } catch (e) {
      tb.innerHTML = `<tr><td colspan="4">${escapeHtml(e.message || String(e))}</td></tr>`;
    }
  };

  const reload = async () => {
    try {
      await loadAdminStorageTable(main);
      await fillCategorySelects();
      await reloadPcats();
    } catch (err) {
      const tbody = main.querySelector("#adm-prod-tbody");
      if (tbody) tbody.innerHTML = `<tr><td colspan="8">${escapeHtml(err.message || String(err))}</td></tr>`;
    }
  };

  const opt = signal ? { signal } : undefined;

  main.querySelector("#adm-prod-reload")?.addEventListener("click", () => void reload(), opt);
  main.querySelector("#adm-prod-cat")?.addEventListener("change", () => void reload(), opt);
  main.querySelector("#adm-prod-pub")?.addEventListener("change", () => void reload(), opt);

  main.querySelector("#adm-prod-create")?.addEventListener(
    "submit",
    async (e) => {
      e.preventDefault();
      if (!write) return;
      const f = e.target;
      const fd = new FormData(f);
      const body = {
        sku: (fd.get("sku") || "").toString().trim(),
        name: (fd.get("name") || "").toString().trim(),
        price: parseFloat(String(fd.get("price") || "0")) || 0,
        quantity_available: parseInt(String(fd.get("quantity_available") || "0"), 10) || 0,
        category: (fd.get("category") || "").toString().trim() || "general",
        image_url: (fd.get("image_url") || "").toString().trim(),
        published: fd.get("published") === "on",
      };
      try {
        await fetchJson("/api/admin/products", { method: "POST", body: JSON.stringify(body) });
        f.reset();
        await reload();
      } catch (err) {
        alert(err.message || String(err));
      }
    },
    opt,
  );

  main.querySelector("#adm-pcat-create")?.addEventListener(
    "submit",
    async (e) => {
      e.preventDefault();
      if (!write) return;
      const f = e.target;
      const fd = new FormData(f);
      const body = {
        code: (fd.get("code") || "").toString().trim(),
        name: (fd.get("name") || "").toString().trim(),
        active: true,
      };
      try {
        await fetchJson("/api/admin/product-categories", { method: "POST", body: JSON.stringify(body) });
        f.reset();
        await reload();
      } catch (err) {
        alert(err.message || String(err));
      }
    },
    opt,
  );

  main.addEventListener(
    "click",
    async (e) => {
      const t = e.target;
      if (!(t instanceof HTMLElement) || !write || !t.matches("[data-adm-pcat-toggle]")) return;
      const tr = t.closest("tr[data-pcat-code]");
      const code = tr?.getAttribute("data-pcat-code");
      if (!code) return;
      const setOne = t.getAttribute("data-pcat-set-active") === "1";
      try {
        await fetchJson(`/api/admin/product-categories/${encodeURIComponent(code)}`, {
          method: "PATCH",
          body: JSON.stringify({ active: setOne }),
        });
        await reload();
      } catch (err) {
        alert(err.message || String(err));
      }
    },
    opt,
  );

  main.querySelector("#adm-prod-bulk-go")?.addEventListener(
    "click",
    async () => {
      if (!write) return;
      const raw = main.querySelector("#adm-prod-bulk")?.value?.trim() || "";
      let products;
      try {
        products = JSON.parse(raw);
      } catch {
        alert("Bulk field must be valid JSON array");
        return;
      }
      if (!Array.isArray(products)) {
        alert("Bulk JSON must be an array of product objects");
        return;
      }
      try {
        const out = await fetchJson("/api/admin/products/bulk", {
          method: "POST",
          body: JSON.stringify({ products }),
        });
        alert(`Imported ${out.imported ?? 0} product(s). Errors: ${(out.errors || []).length}`);
        await reload();
      } catch (err) {
        alert(err.message || String(err));
      }
    },
    opt,
  );

  main.addEventListener(
    "click",
    async (e) => {
      const t = e.target;
      if (!(t instanceof HTMLElement)) return;
      const tr = t.closest("tr[data-id]");
      const id = tr?.getAttribute("data-id");
      if (!id || !write) return;

      if (t.matches("[data-adm-qty]")) {
        const q = prompt("Units in stock (quantity available for sale)");
        if (q == null) return;
        const quantity_available = Math.max(0, parseInt(String(q), 10) || 0);
        try {
          await fetchJson(`/api/admin/products/${encodeURIComponent(id)}`, {
            method: "PATCH",
            body: JSON.stringify({ quantity_available }),
          });
          await reload();
        } catch (err) {
          alert(err.message || String(err));
        }
      }
      if (t.matches("[data-adm-flip-pub]")) {
        const cur = t.getAttribute("data-published") === "1";
        try {
          await fetchJson(`/api/admin/products/${encodeURIComponent(id)}`, {
            method: "PATCH",
            body: JSON.stringify({ published: !cur }),
          });
          await reload();
        } catch (err) {
          alert(err.message || String(err));
        }
      }
    },
    opt,
  );

  void reload();
}

function formatPolicyConditionsLine(p) {
  const c = p && p.conditions;
  if (!c || typeof c !== "object" || Array.isArray(c)) return "";
  const parts = [];
  if (Array.isArray(c.department_any_of) && c.department_any_of.length) {
    parts.push(`Departments: ${c.department_any_of.join(", ")}`);
  }
  if (Array.isArray(c.order_status_any_of) && c.order_status_any_of.length) {
    parts.push(`Order statuses: ${c.order_status_any_of.join(", ")}`);
  }
  return parts.join(" · ");
}

function renderPoliciesTableRows(policies) {
  const rows = (policies || [])
    .map((p) => {
      const acts = Array.isArray(p.actions) ? p.actions.join(", ") : "";
      const desc = p.description != null ? String(p.description) : "";
      const sum = p.scopeLabel != null ? String(p.scopeLabel) : "";
      const cond = formatPolicyConditionsLine(p);
      const notes = [desc, cond].filter(Boolean).join(cond && desc ? " — " : "");
      return `<tr><td><code>${escapeHtml(p.id || "")}</code></td><td>${escapeHtml(p.name || "")}</td><td class="admin-policy-sum">${escapeHtml(sum)}</td><td class="admin-policy-desc">${escapeHtml(notes)}</td><td>${escapeHtml(p.resource || "")}</td><td>${escapeHtml(acts)}</td></tr>`;
    })
    .join("");
  return rows || "<tr><td colspan=\"6\">No policies</td></tr>";
}

const _POL_ACTION_LABELS = {
  read: "View / read",
  list: "List",
  export: "Export",
  update: "Change / update",
  delete: "Delete",
};

function _policyBuilderShowOrderStatuses(feat) {
  if (!feat || typeof feat !== "object") return false;
  const pk = String(feat.pageKey || "").toLowerCase();
  const res = String(feat.suggestedResource || "");
  return (
    res === "order" ||
    res === "cart" ||
    pk.includes("order") ||
    pk.includes("finance") ||
    pk.startsWith("admin.ops") ||
    pk.includes("cart") ||
    pk.includes("delivery")
  );
}

function bindAdminPoliciesDesk(main, signal) {
  const opt = signal ? { signal } : undefined;
  const root = main.querySelector(".admin-main-content") || main;
  const statusEl = root.querySelector("#adm-pol-create-status");
  let builderData = null;
  /** @type {{ code: string; name?: string }[]} */
  let policyDeptList = [];
  let selectedFeature = null;
  let idUnlocked = false;

  const reload = async () => {
    const tb = root.querySelector("#adm-pol-tbody");
    if (!tb) return;
    try {
      const data = await fetchJson("/api/admin/policies");
      tb.innerHTML = renderPoliciesTableRows(data.policies);
    } catch (e) {
      tb.innerHTML = `<tr><td colspan="6">${escapeHtml(e.message || String(e))}</td></tr>`;
    }
  };

  const platSel = root.querySelector("#adm-pol-b-plat");
  const modSel = root.querySelector("#adm-pol-b-mod");
  const featSel = root.querySelector("#adm-pol-b-feat");
  const actWrap = root.querySelector("#adm-pol-b-actions-wrap");
  const orderWrap = root.querySelector("#adm-pol-b-order-wrap");
  const orderInner = root.querySelector("#adm-pol-b-order-statuses");
  const deptWrap = root.querySelector("#adm-pol-b-dept-wrap");
  const deptInner = root.querySelector("#adm-pol-b-departments");
  const hId = root.querySelector("#adm-pol-h-id");
  const hName = root.querySelector("#adm-pol-h-name");
  const hRes = root.querySelector("#adm-pol-h-res");
  const hActions = root.querySelector("#adm-pol-h-actions");
  const idVisible = root.querySelector("#adm-pol-b-id-visible");
  const descEl = root.querySelector("#adm-pol-b-desc");

  const syncHidden = () => {
    if (!hId || !hName || !hRes || !hActions) return;
    const acts = Array.from(root.querySelectorAll(".adm-pol-action-cb:checked")).map((x) => x.value);
    hActions.value = acts.join(",");
    if (selectedFeature) {
      if (!idUnlocked) hId.value = selectedFeature.suggestedPolicyId || "";
      hName.value = selectedFeature.humanName || "";
      hRes.value = selectedFeature.suggestedResource || "";
    }
  };

  const renderActionCheckboxes = (actions) => {
    if (!actWrap) return;
    const set = [...new Set((actions || []).map(String))];
    actWrap.innerHTML = set
      .map(
        (a) =>
          `<label class="adm-pol-action-label"><input type="checkbox" class="adm-pol-action-cb" value="${escapeHtml(a)}" checked /> ${_POL_ACTION_LABELS[a] || a}</label>`,
      )
      .join("");
    actWrap.querySelectorAll(".adm-pol-action-cb").forEach((cb) => cb.addEventListener("change", syncHidden));
  };

  const renderOrderStatuses = () => {
    if (!orderInner || !builderData) return;
    const st = builderData.orderStatuses || [];
    orderInner.innerHTML = st
      .map(
        (s) =>
          `<label class="adm-pol-ost-label"><input type="checkbox" class="adm-pol-ost-cb" value="${escapeHtml(s)}" /> ${escapeHtml(s)}</label>`,
      )
      .join("");
  };

  const renderDepartmentScope = () => {
    if (!deptInner || !deptWrap) return;
    if (!policyDeptList.length) {
      deptWrap.hidden = true;
      deptInner.innerHTML = "";
      return;
    }
    deptWrap.hidden = false;
    deptInner.innerHTML = policyDeptList
      .map((d) => {
        const code = String(d.code || "").trim();
        if (!code) return "";
        const nm = String(d.name || code).trim();
        return `<label class="adm-pol-ost-label"><input type="checkbox" class="adm-pol-dept-cb" value="${escapeHtml(code)}" /> ${escapeHtml(nm)} <code>${escapeHtml(code)}</code></label>`;
      })
      .filter(Boolean)
      .join("");
  };

  const applyFeature = (feat) => {
    selectedFeature = feat;
    if (feat) {
      renderActionCheckboxes(feat.suggestedActions);
      if (orderWrap) {
        const show = _policyBuilderShowOrderStatuses(feat);
        orderWrap.hidden = !show;
        if (show) renderOrderStatuses();
      }
    } else {
      if (actWrap) actWrap.innerHTML = "";
      if (orderWrap) orderWrap.hidden = true;
    }
    syncHidden();
    if (idVisible && !idUnlocked) idVisible.value = feat ? feat.suggestedPolicyId || "" : "";
  };

  const refillModules = () => {
    if (!modSel || !platSel || !builderData) return;
    const pid = platSel.value;
    const p = (builderData.platforms || []).find((x) => x.id === pid);
    const mods = p && Array.isArray(p.modules) ? p.modules : [];
    modSel.innerHTML =
      `<option value="">— Module —</option>` +
      mods.map((m) => `<option value="${escapeHtml(m)}">${escapeHtml(m)}</option>`).join("");
    featSel.innerHTML = '<option value="">— Feature / screen —</option>';
    applyFeature(null);
  };

  const refillFeatures = () => {
    if (!featSel || !platSel || !modSel || !builderData) return;
    const pid = platSel.value;
    const mid = modSel.value;
    const feats = (builderData.features || []).filter((f) => f.platform === pid && f.module === mid);
    featSel.innerHTML =
      '<option value="">— Feature / screen —</option>' +
      feats
        .map((f, i) => {
          const lab = `${f.label || f.pageKey} (${f.pageKey})`;
          return `<option value="${i}">${escapeHtml(lab)}</option>`;
        })
        .join("");
    applyFeature(null);
  };

  const populatePlatformSelect = () => {
    if (!platSel || !builderData) return;
    const plats = builderData.platforms;
    if (!Array.isArray(plats) || plats.length === 0) {
      platSel.innerHTML = '<option value="">— No platforms in catalog —</option>';
      return;
    }
    platSel.innerHTML =
      '<option value="">— Platform —</option>' +
      plats
        .map((p) => `<option value="${escapeHtml(p.id)}">${escapeHtml(p.label)} (${escapeHtml(p.id)})</option>`)
        .join("");
  };

  void (async () => {
    try {
      const dr = await fetchJson("/api/admin/departments");
      policyDeptList = Array.isArray(dr.departments) ? dr.departments : [];
    } catch {
      policyDeptList = [];
    }
    renderDepartmentScope();

    try {
      builderData = await fetchJson("/api/admin/policies/builder-options");
    } catch {
      builderData = null;
    }
    if (!builderData || !Array.isArray(builderData.platforms) || builderData.platforms.length === 0) {
      builderData = buildPolicyBuilderClientCatalog();
      if (statusEl) {
        statusEl.textContent =
          "Using on-device catalog for dropdowns (server builder-options unavailable or empty). Save still posts to the API.";
      }
    } else if (statusEl) {
      statusEl.textContent = "";
    }
    populatePlatformSelect();
  })();

  root.querySelectorAll('input[name="adm_pol_mode"]').forEach((r) => {
    r.addEventListener(
      "change",
      () => {
        const m = root.querySelector('input[name="adm_pol_mode"]:checked')?.value;
        const man = root.querySelector("#adm-pol-panel-manual");
        const bui = root.querySelector("#adm-pol-panel-builder");
        if (man) man.hidden = m !== "manual";
        if (bui) bui.hidden = m === "manual";
      },
      opt,
    );
  });

  platSel?.addEventListener("change", refillModules, opt);
  modSel?.addEventListener("change", refillFeatures, opt);
  featSel?.addEventListener("change", () => {
    if (!featSel || !platSel || !modSel || !builderData) return;
    const pid = platSel.value;
    const mid = modSel.value;
    const feats = (builderData.features || []).filter((f) => f.platform === pid && f.module === mid);
    const idx = parseInt(featSel.value, 10);
    applyFeature(Number.isFinite(idx) ? feats[idx] : null);
  });

  root.querySelector("#adm-pol-b-id-unlock")?.addEventListener(
    "click",
    () => {
      idUnlocked = true;
      if (idVisible) {
        idVisible.readOnly = false;
        idVisible.classList.add("adm-prod-inp--editable");
      }
    },
    opt,
  );

  idVisible?.addEventListener("input", () => {
    if (hId && idVisible) hId.value = idVisible.value.trim();
  });

  root.querySelector("#adm-pol-create")?.addEventListener(
    "submit",
    async (e) => {
      e.preventDefault();
      const f = e.target;
      if (!(f instanceof HTMLFormElement)) return;
      const mode = (root.querySelector('input[name="adm_pol_mode"]:checked') || {}).value || "builder";
      if (statusEl) statusEl.textContent = "";
      try {
        if (mode === "manual") {
          const fd = new FormData(f);
          const id = (fd.get("manual_policy_id") || "").toString().trim();
          const name = (fd.get("manual_name") || "").toString().trim();
          const resource = (fd.get("manual_resource") || "").toString().trim();
          const actionsRaw = (fd.get("manual_actions") || "").toString();
          const actions = actionsRaw
            .split(",")
            .map((x) => x.trim())
            .filter(Boolean);
          const description = (fd.get("manual_description") || "").toString().trim() || null;
          if (!id || !name || !resource || !actions.length) {
            if (statusEl) statusEl.textContent = "Fill id, name, resource, and comma-separated actions.";
            return;
          }
          const ostM = Array.from(root.querySelectorAll(".adm-pol-ost-cb:checked")).map((x) => x.value);
          const deptM = Array.from(root.querySelectorAll(".adm-pol-dept-cb:checked")).map((x) => x.value);
          let conditions = null;
          if (ostM.length || deptM.length) {
            conditions = {};
            if (ostM.length) conditions.order_status_any_of = ostM;
            if (deptM.length) conditions.department_any_of = deptM;
          }
          await fetchJson("/api/admin/policies", {
            method: "POST",
            body: JSON.stringify({ id, name, resource, actions, description, conditions }),
          });
        } else {
          syncHidden();
          const id = (hId?.value || "").trim();
          const name = (hName?.value || "").trim();
          const resource = (hRes?.value || "").trim();
          const actions = (hActions?.value || "")
            .split(",")
            .map((x) => x.trim())
            .filter(Boolean);
          const description = (descEl?.value || "").trim() || null;
          if (!selectedFeature) {
            if (statusEl) statusEl.textContent = "Choose platform, module, and feature.";
            return;
          }
          if (!id || !name || !resource || !actions.length) {
            if (statusEl) statusEl.textContent = "Missing id, name, resource, or actions.";
            return;
          }
          const ost = Array.from(root.querySelectorAll(".adm-pol-ost-cb:checked")).map((x) => x.value);
          const deptSel = Array.from(root.querySelectorAll(".adm-pol-dept-cb:checked")).map((x) => x.value);
          let conditions = null;
          if (ost.length || deptSel.length) {
            conditions = {};
            if (ost.length) conditions.order_status_any_of = ost;
            if (deptSel.length) conditions.department_any_of = deptSel;
          }
          const body = {
            id,
            name,
            resource,
            actions,
            description,
            conditions,
            pageKeys: [selectedFeature.pageKey],
          };
          await fetchJson("/api/admin/policies", { method: "POST", body: JSON.stringify(body) });
        }
        f.reset();
        if (idVisible) {
          idVisible.readOnly = true;
          idVisible.classList.remove("adm-prod-inp--editable");
        }
        idUnlocked = false;
        if (featSel) featSel.innerHTML = '<option value="">— Feature / screen —</option>';
        applyFeature(null);
        renderDepartmentScope();
        if (statusEl) statusEl.textContent = "Policy created.";
        await reload();
      } catch (err) {
        if (statusEl) statusEl.textContent = err.message || String(err);
        else alert(err.message || String(err));
      }
    },
    opt,
  );

  void reload();
}

function bindAdminDepartmentDesk(main, signal) {
  const opt = signal ? { signal } : undefined;

  const reload = async () => {
    try {
      const data = await fetchJson("/api/admin/departments");
      const tbody = main.querySelector("#adm-dept-tbody");
      if (!tbody) return;
      const rows = (data.departments || [])
        .map(
          (d) =>
            `<tr data-dept-code="${escapeHtml(d.code)}"><td><code>${escapeHtml(d.code)}</code></td><td>${escapeHtml(d.name || "")}</td><td>${escapeHtml(d.description || "—")}</td><td><button type="button" class="btn-admin-mini btn-admin-action" data-adm-dept-edit title="Change display name or description">Edit department</button></td></tr>`,
        )
        .join("");
      tbody.innerHTML = rows || "<tr><td colspan=\"4\">No departments</td></tr>";
    } catch (e) {
      const tbody = main.querySelector("#adm-dept-tbody");
      if (tbody) tbody.innerHTML = `<tr><td colspan="4">${escapeHtml(e.message || String(e))}</td></tr>`;
    }
  };

  main.querySelector("#adm-dept-create")?.addEventListener(
    "submit",
    async (e) => {
      e.preventDefault();
      const f = e.target;
      const fd = new FormData(f);
      const body = {
        code: (fd.get("code") || "").toString().trim(),
        name: (fd.get("name") || "").toString().trim(),
        description: (fd.get("description") || "").toString().trim() || null,
      };
      try {
        await fetchJson("/api/admin/departments", { method: "POST", body: JSON.stringify(body) });
        f.reset();
        reload();
      } catch (err) {
        alert(err.message || String(err));
      }
    },
    opt,
  );

  main.addEventListener(
    "click",
    async (e) => {
      const t = e.target;
      if (!(t instanceof HTMLElement) || !t.matches("[data-adm-dept-edit]")) return;
      const tr = t.closest("tr[data-dept-code]");
      const code = tr?.getAttribute("data-dept-code");
      if (!code) return;
      const name = prompt("Department display name", tr?.children[1]?.textContent?.trim() || "");
      if (name == null) return;
      const description = prompt("Description (optional)", "") ?? "";
      try {
        await fetchJson(`/api/admin/departments/${encodeURIComponent(code)}`, {
          method: "PATCH",
          body: JSON.stringify({ name, description: description || null }),
        });
        reload();
      } catch (err) {
        alert(err.message || String(err));
      }
    },
    opt,
  );

  reload();
}

function adminUserRoleOptions(isActorAdmin) {
  const roles = isActorAdmin
    ? ["USER", "ADMIN", "HR", "SALE", "STORAGE", "FINANCE", "DELIVERY"]
    : ["USER", "HR", "SALE", "STORAGE", "FINANCE", "DELIVERY"];
  return roles.map((r) => `<option value="${escapeHtml(r)}">${escapeHtml(r)}</option>`).join("");
}

let _admPolicyEditUserId = null;

function closeAdmPolicyModal() {
  const m = document.getElementById("adm-policy-modal");
  if (m) {
    m.hidden = true;
    m.setAttribute("aria-hidden", "true");
  }
  document.body.classList.remove("auth-modal-open");
  _admPolicyEditUserId = null;
}

async function openAdmPolicyModal(userId, userEmail) {
  const modal = document.getElementById("adm-policy-modal");
  const sub = document.getElementById("adm-policy-modal-sub");
  const list = document.getElementById("adm-policy-modal-checkboxes");
  const st = document.getElementById("adm-policy-modal-status");
  if (!modal || !list) return;
  _admPolicyEditUserId = userId;
  if (sub) sub.textContent = userEmail ? `Account: ${userEmail}` : "";
  if (st) st.textContent = "Loading…";
  list.innerHTML = "";
  modal.hidden = false;
  modal.setAttribute("aria-hidden", "false");
  document.body.classList.add("auth-modal-open");

  const wireClose = () => {
    modal.querySelectorAll("[data-adm-policy-modal-close]").forEach((el) => {
      el.onclick = () => closeAdmPolicyModal();
    });
  };

  try {
    const [all, assigned] = await Promise.all([
      fetchJson("/api/admin/policies"),
      fetchJson(`/api/admin/users/${encodeURIComponent(userId)}/policies`),
    ]);
    if (st) st.textContent = "";
    const ids = new Set((assigned.policies || []).map((p) => p.id));
    const pols = all.policies || [];
    list.innerHTML = pols.length
      ? pols
          .map((p) => {
            const pid = String(p.id || "");
            const checked = ids.has(pid) ? " checked" : "";
            const dn = escapeHtml(p.name || pid);
            const sum = p.scopeLabel ? escapeHtml(String(p.scopeLabel)) : "";
            const desc = p.description != null ? escapeHtml(String(p.description)) : "";
            const dc = [sum, desc].filter(Boolean).join(" — ") || "—";
            return `<label class="adm-policy-cb-label"><input type="checkbox" class="adm-policy-cb" value="${escapeHtml(pid)}"${checked} /><span class="adm-policy-cb-text"><span class="adm-policy-cb-title">${dn}</span><span class="adm-policy-cb-desc">${dc}</span></span></label>`;
          })
          .join("")
      : "<p class=\"admin-bulk-hint\">No policies in catalog.</p>";

    wireClose();
    const saveBtn = document.getElementById("adm-policy-modal-save");
    if (saveBtn) {
      saveBtn.onclick = async () => {
        if (!_admPolicyEditUserId) return;
        const policy_ids = Array.from(list.querySelectorAll(".adm-policy-cb:checked")).map((x) => x.value);
        if (st) st.textContent = "Saving…";
        try {
          await fetchJson(`/api/admin/users/${encodeURIComponent(_admPolicyEditUserId)}/policies`, {
            method: "PUT",
            body: JSON.stringify({ policy_ids }),
          });
          if (st) st.textContent = "Saved.";
          closeAdmPolicyModal();
        } catch (err) {
          if (st) st.textContent = err.message || String(err);
        }
      };
    }
  } catch (e) {
    if (st) st.textContent = e.message || String(e);
    wireClose();
  }
}

function bindAdminUsersDesk(main, actorUser, signal) {
  const opt = signal ? { signal } : undefined;
  const isActorAdmin = String(actorUser?.role || "").toUpperCase() === "ADMIN";

  const reload = async () => {
    try {
      const data = await fetchJson("/api/admin/users");
      const tbody = main.querySelector("#adm-user-tbody");
      if (!tbody) return;
      const rows = (data.users || [])
        .map((u) => {
          const dept = u.department != null && String(u.department).trim() !== "" ? String(u.department).trim() : "";
          const role = u.role != null ? String(u.role) : "";
          const hidePol = !isActorAdmin && role.toUpperCase() === "ADMIN";
          const polBtn = hidePol
            ? ""
            : `<button type="button" class="btn-admin-mini btn-admin-action" data-adm-user-policies title="Tick permission bundles for this person (see Policies desk for what each one means)">Policies</button> `;
          return `<tr data-user-id="${escapeHtml(u.id)}" data-user-dept="${escapeHtml(dept)}" data-user-role="${escapeHtml(role)}"><td>${escapeHtml(u.email)}</td><td>${escapeHtml(u.name || "")}</td><td><code>${escapeHtml(role)}</code></td><td>${dept ? `<code>${escapeHtml(dept)}</code>` : "—"}</td><td class="adm-user-actions">${polBtn}<button type="button" class="btn-admin-mini btn-admin-action" data-adm-user-dept title="Assign department code (e.g. hr, storage)">Change department</button> <button type="button" class="btn-admin-mini btn-admin-action" data-adm-user-role title="Change access role for this user">Change role</button></td></tr>`;
        })
        .join("");
      tbody.innerHTML = rows || "<tr><td colspan=\"5\">No users</td></tr>";
    } catch (e) {
      const tbody = main.querySelector("#adm-user-tbody");
      if (tbody) tbody.innerHTML = `<tr><td colspan="5">${escapeHtml(e.message || String(e))}</td></tr>`;
    }
  };

  main.querySelector("#adm-user-create")?.addEventListener(
    "submit",
    async (e) => {
      e.preventDefault();
      const f = e.target;
      const fd = new FormData(f);
      const body = {
        email: (fd.get("email") || "").toString().trim(),
        password: (fd.get("password") || "").toString(),
        name: (fd.get("name") || "").toString().trim() || undefined,
        role: (fd.get("role") || "USER").toString(),
        department: (fd.get("department") || "").toString().trim() || null,
        description: (fd.get("description") || "").toString().trim() || null,
      };
      try {
        await fetchJson("/api/admin/users", { method: "POST", body: JSON.stringify(body) });
        f.reset();
        reload();
      } catch (err) {
        alert(err.message || String(err));
      }
    },
    opt,
  );

  main.addEventListener(
    "click",
    async (e) => {
      const t = e.target;
      if (!(t instanceof HTMLElement)) return;
      const tr = t.closest("tr[data-user-id]");
      const uid = tr?.getAttribute("data-user-id");
      if (!uid) return;
      if (t.matches("[data-adm-user-policies]")) {
        const em = (tr?.querySelector("td")?.textContent || "").trim();
        void openAdmPolicyModal(uid, em);
        return;
      }
      if (t.matches("[data-adm-user-dept]")) {
        const cur = (tr?.getAttribute("data-user-dept") || "").trim();
        const hint = cur
          ? `Current department: "${cur}". Enter a new code from Admin → Department, or clear the field to remove.`
          : "No department set. Enter a code (e.g. hr, storage) from Admin → Department, or leave empty to keep none.";
        const v = prompt(hint, cur);
        if (v == null) return;
        try {
          await fetchJson(`/api/admin/users/${encodeURIComponent(uid)}`, {
            method: "PATCH",
            body: JSON.stringify({ department: v.trim() || null }),
          });
          reload();
        } catch (err) {
          alert(err.message || String(err));
        }
      }
      if (t.matches("[data-adm-user-role]")) {
        const curR = (tr?.getAttribute("data-user-role") || "").trim();
        const v = prompt(
          `Current role: "${curR || "—"}". New role (USER, HR, SALE, STORAGE, FINANCE, DELIVERY${isActorAdmin ? ", ADMIN" : ""}):`,
          curR,
        );
        if (v == null || !v.trim()) return;
        try {
          await fetchJson(`/api/admin/users/${encodeURIComponent(uid)}`, {
            method: "PATCH",
            body: JSON.stringify({ role: v.trim().toUpperCase() }),
          });
          reload();
        } catch (err) {
          alert(err.message || String(err));
        }
      }
    },
    opt,
  );

  reload();
}

function adminSideLinkHtml(s) {
  const badge = s.sensitive
    ? '<span class="admin-side-badge" title="Financial / cart amounts">Sensitive</span>'
    : "";
  const ingroup = s.group ? " admin-side-link--ingroup" : "";
  return `<a href="#${escapeHtml(s.key)}" class="admin-side-link${ingroup}" data-admin-section="${escapeHtml(s.key)}"><span class="admin-side-text">${escapeHtml(s.title)}</span>${badge}</a>`;
}

function buildAdminSidebarNavHtml(visible) {
  const parts = [];
  let i = 0;
  while (i < visible.length) {
    const s = visible[i];
    if (s.group) {
      const gname = s.group;
      const chunk = [];
      while (i < visible.length && visible[i].group === gname) {
        chunk.push(visible[i++]);
      }
      const links = chunk.map(adminSideLinkHtml).join("");
      parts.push(
        `<div class="admin-side-group" role="group" aria-label="${escapeHtml(gname)}"><div class="admin-side-group-label">${escapeHtml(gname)}</div><div class="admin-side-group-links">${links}</div></div>`,
      );
    } else {
      parts.push(adminSideLinkHtml(s));
      i++;
    }
  }
  return parts.join("");
}

function buildAdminShellHtml(user) {
  const role = user.role || "";
  const dept = user.department || "";
  const visible = ADMIN_MENU_SECTIONS.filter((s) => canAccessAdminSection(role, dept, s.key));
  const sidebar = buildAdminSidebarNavHtml(visible);

  const desc = user.description ? String(user.description) : "";
  const deptLine = dept ? ` · department <code>${escapeHtml(dept)}</code>` : "";

  return `
    <div class="admin-shell">
      <header class="admin-shell-head">
        <div class="admin-shell-head-row">
          <h2 class="page-title">Admin</h2>
          <p class="page-lead admin-shell-lead">
            Pick a desk on the left. Each screen tells you what it does in plain language; the server still enforces who may do what (ABAC). Technical detail: <code>docs/ABAC_PRODUCT_TEST_PLAN.md</code>.
          </p>
        </div>
        <div class="admin-session">
          <p class="admin-session-line">
            <strong>${escapeHtml(user.email || "")}</strong>
            · role <code>${escapeHtml(role)}</code>${deptLine}
          </p>
          ${desc ? `<p class="admin-session-desc">${escapeHtml(desc)}</p>` : ""}
        </div>
      </header>
      <div class="admin-shell-body">
        <aside class="admin-sidebar" aria-label="Department desks">
          <div class="admin-sidebar-label">Your desks</div>
          <nav class="admin-side-nav" role="menu">${sidebar}</nav>
        </aside>
        <section class="admin-main" aria-label="Desk content">
          <div id="admin-main-loading" class="admin-main-loading" hidden>Loading…</div>
          <div id="admin-main-body" class="admin-main-body"></div>
        </section>
      </div>
      <div id="adm-policy-modal" class="auth-modal" hidden aria-hidden="true" role="dialog" aria-modal="true" aria-labelledby="adm-policy-modal-title">
        <div class="auth-modal-backdrop" data-adm-policy-modal-close tabindex="-1"></div>
        <div class="auth-modal-panel adm-policy-modal-panel">
          <div class="auth-modal-toolbar">
            <h2 class="auth-modal-title" id="adm-policy-modal-title">Permission bundles</h2>
            <button type="button" class="auth-modal-close" data-adm-policy-modal-close aria-label="Close">×</button>
          </div>
          <p class="auth-modal-hint" id="adm-policy-modal-sub"></p>
          <p class="adm-policy-modal-explainer">Check the boxes for what this person should be allowed to do in the app. Uncheck to remove. Save applies immediately on the server.</p>
          <div id="adm-policy-modal-checkboxes" class="adm-policy-modal-list"></div>
          <p class="adm-policy-modal-status" id="adm-policy-modal-status" role="status" aria-live="polite"></p>
          <div class="adm-policy-modal-actions">
            <button type="button" class="btn-login-primary" id="adm-policy-modal-save">Save</button>
            <button type="button" class="btn-login-secondary" data-adm-policy-modal-close>Cancel</button>
          </div>
        </div>
      </div>
    </div>`;
}

function updateAdminSidebarActive(activeKey) {
  document.querySelectorAll(".admin-side-link").forEach((a) => {
    const k = a.getAttribute("data-admin-section");
    const on = k === activeKey;
    a.classList.toggle("is-active", on);
    if (on) a.setAttribute("aria-current", "page");
    else a.removeAttribute("aria-current");
  });
}

function applyAdminHash() {
  if (!_adminContext) return;
  const { visibleKeys } = _adminContext;
  if (!visibleKeys.length) return;

  let key = (location.hash || "").replace(/^#/, "").trim();
  if (key === "finance") {
    key = "finance_charts";
    history.replaceState(null, "", `${location.pathname}${location.search}#finance_charts`);
  }
  if (!key || !visibleKeys.includes(key)) {
    key = visibleKeys[0];
    history.replaceState(null, "", `${location.pathname}${location.search}#${key}`);
  }

  setAdminAccessContextFromDesk(key);
  updateAdminSidebarActive(key);
  loadAdminDeskContent(key, _adminContext.user);
}

async function loadAdminDeskContent(key, user) {
  const main = document.getElementById("admin-main-body");
  const loading = document.getElementById("admin-main-loading");
  if (!main) return;

  const meta = ADMIN_MENU_SECTIONS.find((s) => s.key === key);
  if (!meta) {
    main.innerHTML = "<p class=\"admin-main-error\">Unknown desk.</p>";
    return;
  }

  if (loading) {
    loading.hidden = false;
  }
  if (_adminDeskUiAbort) {
    _adminDeskUiAbort.abort();
    _adminDeskUiAbort = null;
  }
  main.innerHTML = "";
  _adminDeskUiAbort = new AbortController();
  const deskSig = _adminDeskUiAbort.signal;

  const wrapMeta = (inner) => `
    <header class="admin-main-header">
      <h3 class="admin-main-title">${escapeHtml(meta.title)}</h3>
      <p class="admin-main-desc">${escapeHtml(meta.help)}</p>
      <p class="admin-main-api"><span class="admin-main-api-label">API</span> ${escapeHtml(meta.api)}</p>
    </header>
    <div class="admin-main-content">${inner}</div>`;

  try {
    let inner = "";
    if (key === "users") {
      const isActorAdmin = String(user.role || "").toUpperCase() === "ADMIN";
      let deptOpts = "";
      try {
        const deptData = await fetchJson("/api/admin/departments");
        deptOpts = (deptData.departments || [])
          .map(
            (d) =>
              `<option value="${escapeHtml(d.code)}">${escapeHtml(d.name)} (${escapeHtml(d.code)})</option>`,
          )
          .join("");
      } catch {
        deptOpts = "";
      }
      inner = `
        <form id="adm-user-create" class="adm-user-form adm-prod-form">
          <input name="email" type="email" required class="adm-prod-inp" placeholder="Email" autocomplete="off" />
          <input name="password" type="password" required minlength="6" class="adm-prod-inp" placeholder="Password" autocomplete="new-password" />
          <input name="name" class="adm-prod-inp" placeholder="Display name" />
          <select name="role" class="store-filter-select" aria-label="Role">${adminUserRoleOptions(isActorAdmin)}</select>
          <select name="department" class="store-filter-select" aria-label="Department"><option value="">— Department —</option>${deptOpts}</select>
          <input name="description" class="adm-prod-inp adm-prod-inp--wide" placeholder="Description (optional)" />
          <button type="submit" class="btn-admin-mini btn-admin-action" title="Create account with selected role and department">Save new user</button>
        </form>
        <p class="admin-bulk-hint">HR cannot create or change <code>ADMIN</code> accounts (server enforced).</p>
        <p class="admin-bulk-hint">To set <strong>permissions</strong> for someone, click <strong>Policies</strong> on their row (tick the bundles they need, then Save). The <strong>Policies & ABAC</strong> desk lists every bundle and explains what each one means.</p>
        <div class="admin-table-wrap"><table class="admin-data-table"><thead><tr><th>Email</th><th>Name</th><th>Role</th><th>Department</th><th>Manage</th></tr></thead><tbody id="adm-user-tbody"><tr><td colspan="5">Loading…</td></tr></tbody></table></div>`;
    } else if (key === "policies") {
      inner = `<div class="admin-hr-guide" role="region" aria-label="How policies work">
          <h4 class="admin-storage-sub">How policies work (simple version)</h4>
          <ol class="admin-hr-steps">
            <li>A <strong>policy</strong> is a bundle of permissions — it answers “what is this person allowed to do in the system?”</li>
            <li>Use the <strong>Create policy</strong> form below to add a new bundle (saved on the server). The table is the live catalog.</li>
            <li>To <strong>give someone</strong> a bundle: open <button type="button" class="btn-admin-mini btn-admin-action" data-admin-jump="users">Users</button>, click <strong>Policies</strong> on their row, tick boxes, <strong>Save</strong>.</li>
            <li><strong>Enforcement</strong>: many APIs still check <strong>role</strong> only; attaching a policy records intent for ABAC — wire <code>policy_store</code> into routes if you want it to gate behavior.</li>
          </ol>
        </div>
        <h4 class="admin-storage-sub">Create policy</h4>
        <p class="admin-bulk-hint">Use <strong>Guided</strong> to pick <em>Platform › Module › Feature</em>. The system fills a short <strong>id</strong> and a readable <strong>name</strong>. Optional <strong>order statuses</strong> and <strong>departments</strong> are stored under <code>conditions</code> until APIs evaluate them.</p>
        <form id="adm-pol-create" class="adm-pol-create-form">
          <div class="adm-pol-mode-row">
            <label class="adm-pol-mode"><input type="radio" name="adm_pol_mode" value="builder" checked /> Guided (dropdowns)</label>
            <label class="adm-pol-mode"><input type="radio" name="adm_pol_mode" value="manual" /> Advanced (type id &amp; resource yourself)</label>
          </div>
          <div id="adm-pol-panel-builder" class="adm-pol-panel">
            <div class="adm-pol-builder">
              <label class="adm-pol-field">Platform
                <select id="adm-pol-b-plat" class="store-filter-select"><option value="">Loading…</option></select>
              </label>
              <label class="adm-pol-field">Module
                <select id="adm-pol-b-mod" class="store-filter-select"><option value="">— Module —</option></select>
              </label>
              <label class="adm-pol-field">Feature / screen
                <select id="adm-pol-b-feat" class="store-filter-select adm-pol-feat-select"><option value="">— Feature —</option></select>
              </label>
            </div>
            <label class="adm-pol-field adm-pol-field--full">Policy id <span class="admin-bulk-hint">(auto; unlock to change if duplicate)</span>
              <div class="adm-pol-id-row">
                <input type="text" id="adm-pol-b-id-visible" class="adm-prod-inp adm-pol-id-visible" readonly autocomplete="off" maxlength="63" placeholder="Pick a feature below — id fills in automatically" />
                <button type="button" class="btn-admin-mini btn-admin-action" id="adm-pol-b-id-unlock">Edit id</button>
              </div>
            </label>
            <input type="hidden" id="adm-pol-h-id" />
            <input type="hidden" id="adm-pol-h-name" />
            <input type="hidden" id="adm-pol-h-res" />
            <input type="hidden" id="adm-pol-h-actions" />
            <p class="admin-bulk-hint"><strong>Operations</strong> (for this bundle):</p>
            <div id="adm-pol-b-actions-wrap" class="adm-pol-actions-wrap"></div>
            <div id="adm-pol-b-order-wrap" class="adm-pol-order-wrap" hidden>
              <p class="admin-bulk-hint"><strong>Order status scope</strong> (optional — only these statuses; leave empty = not limited here):</p>
              <div id="adm-pol-b-order-statuses" class="adm-pol-order-statuses"></div>
            </div>
            <label class="adm-pol-field adm-pol-field--full">Notes (optional)
              <textarea id="adm-pol-b-desc" class="adm-prod-bulk adm-pol-desc" rows="2" maxlength="512" placeholder="e.g. Temporary while finance is short-staffed"></textarea>
            </label>
          </div>
          <div id="adm-pol-panel-manual" class="adm-pol-panel" hidden>
            <div class="adm-prod-form">
              <input name="manual_policy_id" class="adm-prod-inp adm-pol-manual-id" placeholder="id (e.g. pol_reports_read)" maxlength="63" pattern="[a-zA-Z][a-zA-Z0-9_-]*" title="Letter first" autocomplete="off" />
              <input name="manual_name" class="adm-prod-inp" placeholder="Technical name" maxlength="128" />
              <input name="manual_resource" class="adm-prod-inp" placeholder="resource (e.g. order, cart)" maxlength="128" />
              <input name="manual_actions" class="adm-prod-inp adm-prod-inp--wide" placeholder="actions — comma-separated" />
              <input name="manual_description" class="adm-prod-inp adm-prod-inp--wide" placeholder="Description (optional)" maxlength="512" />
            </div>
          </div>
          <div id="adm-pol-b-dept-wrap" class="adm-pol-order-wrap" hidden>
            <p class="admin-bulk-hint"><strong>Department scope</strong> (optional — e.g. this bundle applies when supporting <em>another</em> org unit: tick <code>finance</code> + <code>hr</code>). Saved as <code>conditions.department_any_of</code>. Enforcement still needs route-level policy checks.</p>
            <div id="adm-pol-b-departments" class="adm-pol-order-statuses"></div>
          </div>
          <button type="submit" class="btn-login-primary adm-pol-save" title="POST /api/admin/policies">Save policy</button>
        </form>
        <p class="admin-bulk-hint" id="adm-pol-create-status" role="status" aria-live="polite"></p>
        <div class="admin-table-wrap"><table class="admin-data-table"><thead><tr><th>Id</th><th>Name</th><th>Summary</th><th>Notes</th><th>Resource</th><th>Operations</th></tr></thead><tbody id="adm-pol-tbody"><tr><td colspan="6">Loading…</td></tr></tbody></table></div>`;
    } else if (key === "bot_c_lib") {
      inner = adminBotCLibDeskInnerHtml();
    } else if (key === "department") {
      inner = `
        <form id="adm-dept-create" class="adm-prod-form">
          <input name="code" class="adm-prod-inp" placeholder="code (e.g. logistics)" required maxlength="32" pattern="[a-z][a-z0-9_-]*" title="lowercase, digits, hyphen" />
          <input name="name" class="adm-prod-inp" placeholder="Display name" required />
          <input name="description" class="adm-prod-inp adm-prod-inp--wide" placeholder="Description (optional)" />
          <button type="submit" class="btn-admin-mini btn-admin-action" title="Register a new department code for user assignments">Create department</button>
        </form>
        <p class="admin-bulk-hint">Codes are lowercase (e.g. <code>hr</code>, <code>storage</code>). User records reference these codes.</p>
        <div class="admin-table-wrap"><table class="admin-data-table"><thead><tr><th>Code</th><th>Name</th><th>Description</th><th></th></tr></thead><tbody id="adm-dept-tbody"><tr><td colspan="4">Loading…</td></tr></tbody></table></div>
        <button type="button" class="btn-admin-jump" data-admin-jump="users">Open <strong>Users</strong> desk</button>`;
    } else if (key === "storage") {
      inner = `
        <div class="admin-storage-toolbar">
          <label class="admin-storage-field">Category
            <select id="adm-prod-cat" class="store-filter-select"><option value="">All</option></select>
          </label>
          <label class="admin-storage-field">Published
            <select id="adm-prod-pub" class="store-filter-select">
              <option value="">All</option>
              <option value="true">Yes</option>
              <option value="false">No</option>
            </select>
          </label>
          <button type="button" class="btn-store-reload" id="adm-prod-reload" title="Reload product list with current filters">Refresh products</button>
        </div>
        <div class="admin-table-wrap">
          <table class="admin-data-table" id="adm-prod-table">
            <thead>
              <tr>
                <th>SKU</th><th>Name</th><th>Category</th><th>Price</th><th>Stock</th><th>In store</th><th>Img</th>
                <th data-adm-write>Product actions</th>
              </tr>
            </thead>
            <tbody id="adm-prod-tbody"></tbody>
          </table>
        </div>
        <div class="admin-storage-forms" data-adm-write>
          <h4 class="admin-storage-sub">New product</h4>
          <form id="adm-prod-create" class="adm-prod-form">
            <input name="sku" placeholder="SKU" required class="adm-prod-inp" />
            <input name="name" placeholder="Name" class="adm-prod-inp" />
            <input name="price" type="number" step="0.01" placeholder="Price" class="adm-prod-inp" />
            <input name="quantity_available" type="number" placeholder="Stock qty" class="adm-prod-inp" />
            <label class="admin-storage-field">Product category
              <select name="category" id="adm-prod-create-category" class="store-filter-select"><option value="general">general</option></select>
            </label>
            <input name="image_url" placeholder="Image URL (optional)" class="adm-prod-inp adm-prod-inp--wide" />
            <label class="adm-prod-check"><input type="checkbox" name="published" /> Published</label>
            <button type="submit" class="btn-admin-mini btn-admin-action" title="Add this SKU to the catalog">Save product</button>
          </form>
          <h4 class="admin-storage-sub">Product category registry</h4>
          <p class="admin-bulk-hint">Storefront filters use active categories. Add codes here before assigning products.</p>
          <div class="admin-table-wrap"><table class="admin-data-table"><thead><tr><th>Code</th><th>Name</th><th>Active</th><th data-adm-write></th></tr></thead><tbody id="adm-pcat-tbody"><tr><td colspan="4">…</td></tr></tbody></table></div>
          <form id="adm-pcat-create" class="adm-prod-form" data-adm-write>
            <input name="code" class="adm-prod-inp" placeholder="code" required maxlength="32" pattern="[a-z][a-z0-9_-]*" />
            <input name="name" class="adm-prod-inp" placeholder="Display name" required />
            <button type="submit" class="btn-admin-mini btn-admin-action" title="Register a category code for products and filters">Save category</button>
          </form>
          <h4 class="admin-storage-sub">Bulk JSON</h4>
          <p class="admin-bulk-hint">Array of objects: sku, name, price, quantity_available, category, image_url (optional), published.</p>
          <textarea id="adm-prod-bulk" class="adm-prod-bulk" rows="7" placeholder='[{"sku":"SKU-9","name":"Item","price":10,"quantity_available":5,"category":"general","published":true}]'></textarea>
          <button type="button" class="btn-store-reload" id="adm-prod-bulk-go" title="Upsert products from the JSON array below">Import JSON</button>
        </div>`;
    } else if (key === "finance_charts") {
      inner = adminFinanceChartsOnlyInnerHtml();
    } else if (key === "finance_orders") {
      inner = `<p class="admin-bulk-hint fin-cross-links"><button type="button" class="btn-admin-mini btn-admin-action" data-admin-jump="finance_charts">← Charts & export</button> for dashboards and CSV.</p>${adminOrdersDeskInnerHtml()}`;
    } else if (key === "orders" || key === "delivery") {
      inner = adminOrdersDeskInnerHtml();
    } else if (key === "sale") {
      const sData = await fetchJson("/api/admin/products");
      const sRows = (sData.products || [])
        .map(
          (p) =>
            `<tr><td><code>${escapeHtml(p.sku)}</code></td><td>${escapeHtml(p.name)}</td><td>${escapeHtml(p.category || "")}</td><td>${escapeHtml(Number(p.price).toFixed(2))}</td><td>${escapeHtml(String(p.quantity_available))}</td><td>${p.published ? "yes" : "no"}</td></tr>`,
        )
        .join("");
      inner = `<div class="admin-table-wrap"><table class="admin-data-table"><thead><tr><th>SKU</th><th>Name</th><th>Category</th><th>Price</th><th>Stock</th><th>In store</th></tr></thead><tbody>${sRows || "<tr><td colspan=\"6\">No products</td></tr>"}</tbody></table></div>`;
    }

    main.innerHTML = wrapMeta(inner);
    if (key === "storage") {
      bindAdminStorageDesk(main, user, deskSig);
    }
    if (key === "orders") {
      bindAdminOrdersDesk(main, user, deskSig);
    }
    if (key === "finance_charts") {
      bindFinanceChartsDesk(main, user, deskSig);
    }
    if (key === "finance_orders") {
      bindAdminOrdersDesk(main, user, deskSig, { defaultStatus: "pending_confirmation" });
    }
    if (key === "delivery") {
      bindAdminOrdersDesk(main, user, deskSig, { defaultStatus: "packed" });
    }
    if (key === "department") {
      bindAdminDepartmentDesk(main, deskSig);
    }
    if (key === "users") {
      bindAdminUsersDesk(main, user, deskSig);
    }
    if (key === "policies") {
      bindAdminPoliciesDesk(main, deskSig);
    }
    if (key === "bot_c_lib") {
      bindBotCLibDesk(main, deskSig);
    }
    main.querySelectorAll("[data-admin-jump]").forEach((btn) => {
      btn.addEventListener("click", () => {
        const target = btn.getAttribute("data-admin-jump");
        if (target && _adminContext?.visibleKeys.includes(target)) {
          location.hash = target;
        }
      });
    });
  } catch (err) {
    main.innerHTML = wrapMeta(
      `<p class="admin-main-error">${escapeHtml(err.message || String(err))}</p>`,
    );
  } finally {
    if (loading) loading.hidden = true;
  }
}

window.addEventListener("hashchange", () => {
  if (normalizePath(window.location.pathname) !== "/admin") return;
  if (!_adminContext) return;
  applyAdminHash();
});

function fmtBytes(n) {
  const x = Number(n) || 0;
  if (x < 1024) return `${x} B`;
  if (x < 1048576) return `${(x / 1024).toFixed(1)} KB`;
  return `${(x / 1048576).toFixed(2)} MB`;
}

function renderStats(st) {
  statsEl.replaceChildren();
  statsEl.className = "stats-grid";

  const addRow = (label, valueText, good) => {
    const row = document.createElement("div");
    row.className = "stats-row";
    const lab = document.createElement("span");
    lab.className = "stats-label";
    lab.textContent = label;
    const val = document.createElement("span");
    val.className = "stats-value";
    if (good === true) val.classList.add("stats-ok");
    if (good === false) val.classList.add("stats-bad");
    val.textContent = valueText;
    row.appendChild(lab);
    row.appendChild(val);
    statsEl.appendChild(row);
  };

  {
    const hasMongoc =
      st.mongoc_linked === true || st.mongoc_linked === 1;
    const mongocUnknown =
      st.mongoc_linked === undefined || st.mongoc_linked === null;
    let mongoText;
    let mongoGood;
    if (mongocUnknown) {
      mongoText = st.mongo_connected ? "Connected" : "Offline";
      mongoGood = !!st.mongo_connected;
    } else if (!hasMongoc) {
      mongoText = "No mongoc in lib — rebuild: make lib USE_MONGOC=1";
      mongoGood = false;
    } else {
      mongoText = st.mongo_connected ? "Connected" : "Offline";
      mongoGood = !!st.mongo_connected;
    }
    addRow("MongoDB", mongoText, mongoGood);
  }
  addRow("Redis", st.redis_connected ? "Connected" : "Offline", !!st.redis_connected);
  addRow("ELK configured", st.elk_enabled ? "Yes" : "No", null);
  addRow(
    "ELK reachable",
    !st.elk_enabled ? "—" : st.elk_connected ? "Yes" : "No",
    st.elk_enabled ? !!st.elk_connected : undefined
  );
  addRow("Ollama", st.ollama_connected ? "Running" : "Unreachable", !!st.ollama_connected);
  addRow("Processed turns", String(st.processed ?? 0), null);
  addRow("Engine errors", String(st.errors ?? 0), (st.errors ?? 0) === 0);
  addRow("Error count", String(st.error_count ?? 0), null);
  addRow("Warning count", String(st.warning_count ?? 0), null);
  addRow("Context buffer (est.)", fmtBytes(st.memory_bytes), null);
}

async function refreshStatsPanel() {
  try {
    const st = await fetchJson("/api/stats");
    renderStats(st);
  } catch (e) {
    statsEl.replaceChildren();
    statsEl.textContent = String(e.message);
    statsEl.className = "stats-grid stats-error";
  }
}

async function fetchChatHistoryMessages() {
  const q = new URLSearchParams({ reload: "1" });
  if (!getToken().trim()) {
    q.set("tenant_id", CHAT_TENANT_ID);
    q.set("user", CHAT_USER_ID);
  }
  const h = await fetchJson(`/api/history?${q.toString()}`);
  return h.messages ?? [];
}

async function refreshHistoryAndChatInto(logContainer = logEl) {
  try {
    const msgs = await fetchChatHistoryMessages();
    renderTurns(msgs, logContainer);
    return msgs;
  } catch (e) {
    renderTurns([], logContainer);
    throw e;
  }
}

async function refreshHistoryAndChat() {
  return refreshHistoryAndChatInto(logEl);
}

/**
 * Stream a user message into a chat log (shared by full-page bot composer and floating assistant).
 * @param {{ refreshStats?: boolean }} opts
 */
async function submitChatStream(text, logContainer, opts = {}) {
  if (!getToken().trim()) {
    setStatus("Sign in to chat (JWT required)", "error");
    return;
  }
  if (!canUseBotChatUi(_sessionPolicies, _sessionUser)) {
    setStatus("Chat is not enabled for your account (missing policy)", "error");
    return;
  }
  const refreshStats = opts.refreshStats !== false;
  const btn = logContainer?.closest(".assistant-panel-chat")?.querySelector(".btn-send") || form?.querySelector(".btn-send");
  if (btn) btn.disabled = true;
  const tempMessageId = newTempMessageId();
  setStatus("Generating response…", "thinking");

  const empty = logContainer?.querySelector(".chat-empty");
  if (empty) empty.remove();

  const userTs = nowHms();
  const userRow = buildMessageRow("user", text, userTs, null);
  userRow.wrap.dataset.tempMessageId = tempMessageId;
  logContainer.appendChild(userRow.wrap);

  const botTs = nowHms();
  const botRow = buildMessageRow("assistant", "", botTs, CHAT_STREAMING_SOURCE_LABEL);
  botRow.body.classList.add("streaming");
  botRow.wrap.dataset.tempMessageId = tempMessageId;
  logContainer.appendChild(botRow.wrap);
  logContainer.scrollTop = logContainer.scrollHeight;

  try {
    const r = await fetch(`${API_BASE}/api/chat/stream`, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        Accept: "text/event-stream",
        ...authHeaders(),
      },
      body: JSON.stringify(
        getToken().trim()
          ? { message: text, temp_message_id: tempMessageId }
          : {
              message: text,
              tenant_id: CHAT_TENANT_ID,
              user: CHAT_USER_ID,
              temp_message_id: tempMessageId,
            },
      ),
    });

    if (!r.ok) {
      const errData = await r.json().catch(() => ({}));
      throw new Error(errData.error || r.statusText || "Stream failed");
    }

    let streamErr = null;
    for await (const ev of parseSSEStream(r.body)) {
      if (ev.assistant_meta) {
        const badge = botRow.wrap.querySelector(".source-badge");
        if (badge) {
          const lbl = ev.completion_source || ev.source;
          if (lbl) badge.textContent = normalizeSource(lbl);
          const parts = [ev.llm_model, ev.chat_wire].filter(Boolean);
          if (parts.length) badge.title = parts.join(" · ");
        }
        break;
      }
      if (ev.error) {
        streamErr = ev.error;
        botRow.body.classList.remove("streaming");
        botRow.body.textContent = ev.error;
        botRow.wrap.classList.add("msg-error");
        break;
      }
      if (ev.token) botRow.body.textContent += ev.token;
      if (ev.done) {
        botRow.body.classList.remove("streaming");
        const badge = botRow.wrap.querySelector(".source-badge");
        if (badge && ev.source) {
          badge.textContent = normalizeSource(ev.source);
          const parts = [ev.llm_model, ev.chat_wire].filter(Boolean);
          badge.title = parts.length ? parts.join(" · ") : ev.chat_wire ? `wire: ${String(ev.chat_wire)}` : "";
        }
        if (ev.source) break;
        continue;
      }
      logContainer.scrollTop = logContainer.scrollHeight;
    }

    if (streamErr) {
      setStatus(streamErr, "error");
    } else {
      setStatus("Ready", "ready");
      if (refreshStats) await refreshStatsPanel();
      /* Keep streamed rows in place; source badge comes from final SSE (server), not GET /api/history. */
    }
  } catch (err) {
    setStatus(err.message || "Request failed", "error");
    botRow.body.classList.remove("streaming");
    if (!botRow.body.textContent.trim()) {
      botRow.body.textContent = err.message || "Request failed";
      botRow.wrap.classList.add("msg-error");
    }
    try {
      await refreshHistoryAndChatInto(logContainer);
    } catch {
      /* ignore */
    }
  } finally {
    if (btn) btn.disabled = false;
  }
}

input.addEventListener("keydown", (e) => {
  if (e.key !== "Enter" || e.isComposing) return;
  if (e.ctrlKey || e.metaKey || e.shiftKey) return;
  e.preventDefault();
  form.requestSubmit();
});

form.addEventListener("submit", async (e) => {
  e.preventDefault();
  const text = input.value.trim();
  if (!text) return;
  input.value = "";
  await submitChatStream(text, logEl, { refreshStats: true });
});

function setupAssistantDock() {
  if (!ASSISTANT_ENABLED) return;
  if (!assistantFabEl || !assistantPanelEl || !assistantLogEl || !assistantFormEl || !assistantInputEl) return;

  const closeAssistantPanel = () => {
    assistantPanelEl.hidden = true;
    assistantFabEl.setAttribute("aria-expanded", "false");
    document.body.classList.remove("assistant-open");
    assistantFabEl.focus();
  };
  const openAssistantPanel = () => {
    syncAssistantPanelAuth();
    assistantPanelEl.hidden = false;
    assistantFabEl.setAttribute("aria-expanded", "true");
    document.body.classList.add("assistant-open");
    if (!_assistantPanelHistoryPrimed && getToken().trim()) {
      _assistantPanelHistoryPrimed = true;
      void refreshHistoryAndChatInto(assistantLogEl).catch(() => {});
    }
    if (getToken().trim()) assistantInputEl?.focus();
  };

  assistantFabEl.addEventListener("click", () => {
    if (assistantPanelEl.hidden) openAssistantPanel();
    else closeAssistantPanel();
  });
  document.getElementById("assistant-close")?.addEventListener("click", () => closeAssistantPanel());
  document.addEventListener("keydown", (e) => {
    if (e.key !== "Escape" || assistantPanelEl.hidden) return;
    if (document.getElementById("auth-modal") && !document.getElementById("auth-modal").hidden) return;
    closeAssistantPanel();
  });

  assistantInputEl.addEventListener("keydown", (e) => {
    if (e.key !== "Enter" || e.isComposing) return;
    if (e.ctrlKey || e.metaKey || e.shiftKey) return;
    e.preventDefault();
    assistantFormEl.requestSubmit();
  });

  assistantFormEl.addEventListener("submit", async (e) => {
    e.preventDefault();
    const text = assistantInputEl.value.trim();
    if (!text) return;
    assistantInputEl.value = "";
    await submitChatStream(text, assistantLogEl, { refreshStats: false });
  });
}

geoImportForm.addEventListener("submit", async (e) => {
  e.preventDefault();
  const file = geoFileInput.files?.[0];
  if (!file) {
    geoImportResultEl.textContent = "Choose a .csv file first.";
    geoImportResultEl.className = "geo-import-result geo-import-result--error";
    return;
  }

  const fd = new FormData();
  fd.append("file", file);
  const geoTenant =
    getToken().trim() && _sessionUser && _sessionUser.id
      ? String(_sessionUser.id).trim()
      : CHAT_TENANT_ID;
  fd.append("tenant_id", geoTenant || CHAT_TENANT_ID);

  const mapStr = geoMappingInput.value.trim();
  if (mapStr) {
    try {
      JSON.parse(mapStr);
    } catch {
      geoImportResultEl.textContent = "Mapping must be valid JSON.";
      geoImportResultEl.className = "geo-import-result geo-import-result--error";
      return;
    }
    fd.append("mapping", mapStr);
  }

  const qs = new URLSearchParams();
  // API default is no Ollama; pass embed=1 only when user wants embeddings.
  if (!geoNoEmbedInput.checked) qs.set("embed", "1");

  const headers = {};
  if (GEO_IMPORT_KEY) headers["X-Geo-Import-Key"] = GEO_IMPORT_KEY;

  const url = `${API_BASE}/api/geo/import${qs.toString() ? `?${qs.toString()}` : ""}`;
  geoUploadBtn.disabled = true;
  geoImportResultEl.textContent = "Uploading…";
  geoImportResultEl.className = "geo-import-result";

  try {
    const r = await fetch(url, { method: "POST", headers, body: fd });
    const data = await r.json().catch(() => ({}));
    if (!r.ok) {
      throw new Error(data.error || data.hint || r.statusText);
    }
    const pe = (data.parse_errors || []).length;
    const fl = (data.failed || []).length;
    geoImportResultEl.textContent = `Imported ${data.imported ?? 0} row(s). parse_errors: ${pe}, failed: ${fl}.`;
    geoImportResultEl.className = "geo-import-result geo-import-result--ok";
    if (fl > 0 || pe > 0) {
      geoImportResultEl.textContent += ` Details: ${JSON.stringify({ parse_errors: data.parse_errors, failed: data.failed })}`;
    }
    geoFileInput.value = "";
    geoFileNameEl.textContent = "No file chosen";
  } catch (err) {
    geoImportResultEl.textContent = err.message || "Upload failed";
    geoImportResultEl.className = "geo-import-result geo-import-result--error";
  } finally {
    geoUploadBtn.disabled = false;
  }
});

void (async () => {
  try {
    await fetchJson("/api/health");
    setStatus("Ready", "ready");
  } catch (e) {
    setStatus("API unreachable — start server on :5000", "error");
  }
  await refreshAuthStatus();
  await hydrateAccessModelFromApi(API_BASE, { includeAdmin: true });
  syncAssistantPanelAuth();
  refreshAssistantDockChrome();
  if (_sessionUser) updateUserNavBar(_sessionUser);
  refreshCustomerNavVisibility();
  setupAssistantDock();
  initRouter(applyRoute);
  if (getPath() === "/bot") void renderBotPage();
})();
