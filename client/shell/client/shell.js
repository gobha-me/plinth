// Plinth shell entry — ICD-0.6.0 §4.4 boot sequence + §5 login + §6
// four-zone topbar + §7 top-level error boundary + ICD-0.6.2 §7
// avatar-popover theme + scale controls. Single-file ES module;
// Preact + htm vendored under ./vendor/ per OQ1 (B). No build step.

import { h, render, Component } from './vendor/preact.module.js';
import htm from './vendor/htm.module.js';
import { call as plinthCall } from './sdk.js';
const html = htm.bind(h);

// ICD-0.6.3 §6.5 — sanitize boundary detail before audit emission.
// Length-cap per ICD §6.5: error_message 1024, error_stack 8192,
// component_path 8192. Production builds omit error_stack per OQ6
// (gated on `window.__PLINTH_PRODUCTION__` injected from index.html).
const STACK_LIMIT      = 8192;
const MESSAGE_LIMIT    = 1024;
const COMPONENT_LIMIT  = 8192;

function trim(s, n) {
    if (typeof s !== 'string') { return null; }
    return s.length > n ? s.slice(0, n) : s;
}

function sanitizeBoundaryPayload(error, info, panelId) {
    const detail = {
        panel_id:      panelId ?? null,
        error_message: trim(error?.message ?? String(error), MESSAGE_LIMIT),
    };
    const componentPath = info?.componentStack;
    if (componentPath) {
        detail.component_path = trim(componentPath, COMPONENT_LIMIT);
    }
    if (!window.__PLINTH_PRODUCTION__) {
        const stack = error?.stack;
        if (stack) { detail.error_stack = trim(stack, STACK_LIMIT); }
    }
    return detail;
}

// ── Preferences (ICD-0.6.2 §4.4 + §5.3 + §7) ─────────────────────────
//
// localStorage.shellPrefs is a JSON object keyed by well-known keys
// `shell.theme` (string in {light,dark,system}) and `shell.scale_pct`
// (integer 80..175). Pre-paint resolver in prepaint.js applies the
// stored values synchronously before first paint; this module re-applies
// on user action (popover select).
//
// Implementation deviation from ICD-0.6.2 §4.4 + §7.2 + §7.3:
// kernel-side persistence via `cap.call("shell.preferences.set", …)`
// is deferred to the 0.6.1.N JS-dispatch follow-up that closes
// ICD-0.6.1's P.\* / I.\* deferral (the same path
// `project_test_fixture_inflight.md` session 9 noted needs the
// `init_registry` teardown bug resolved first). v0.6.2 ships
// localStorage-only persistence — per-browser, per-device. The
// SCHEMA validator in `server/handlers/preferences.set.js` ships
// in this PR so the wiring is ready when the 0.6.1.N follow-up
// connects browser → kernel. Recorded in §17 amendment block.
const PREF_KEYS = Object.freeze({ THEME: 'shell.theme',
                                  SCALE: 'shell.scale_pct' });
const SCALE_PRESETS = Object.freeze([80, 90, 100, 110, 125, 150, 175]);

function readPrefs() {
  try { return JSON.parse(localStorage.getItem('shellPrefs') || '{}'); }
  catch (_) { return {}; }
}
function writePrefs(prefs) {
  try { localStorage.setItem('shellPrefs', JSON.stringify(prefs)); }
  catch (_) { /* quota exceeded / disabled — silently no-op */ }
}
function setPref(key, value) {
  const prefs = readPrefs();
  if (value === undefined) { delete prefs[key]; } else { prefs[key] = value; }
  writePrefs(prefs);
}
function applyTheme(stored) {
  const want = (stored === 'light' || stored === 'dark') ? stored : 'system';
  const resolved = want === 'system'
    ? (window.matchMedia('(prefers-color-scheme: dark)').matches
        ? 'dark' : 'light')
    : want;
  document.documentElement.dataset.theme = resolved;
}
function applyScale(pct) {
  const n = (Number.isInteger(pct) && pct >= 80 && pct <= 175) ? pct : 100;
  document.documentElement.style.fontSize = (n * 0.135) + 'px';
}

// ICD-0.6.2 §4.3 — system-theme tracking. Listener flips the resolved
// `data-theme` when OS preference changes mid-session, but only when
// the stored value is `system` (or absent) — explicit `light` / `dark`
// stay pinned. Installed once per page load.
(function installMqlListener() {
  if (typeof window === 'undefined' || !window.matchMedia) { return; }
  const mql = window.matchMedia('(prefers-color-scheme: dark)');
  const handler = () => {
    const stored = readPrefs()[PREF_KEYS.THEME];
    if (stored !== 'light' && stored !== 'dark') {
      applyTheme(stored);
    }
  };
  if (typeof mql.addEventListener === 'function') {
    mql.addEventListener('change', handler);
  } else if (typeof mql.addListener === 'function') {
    mql.addListener(handler);  // older Safari
  }
})();

// ── Error-code → user-string mapping (ICD-0.6.0 §5.4) ───────────────
const ERR_STRINGS = {
  missing_username:        'Username is required.',
  missing_password:        'Password is required.',
  invalid_credentials:     'Username or password is incorrect.',
  account_disabled:        'This account is disabled.',
  username_too_short:      'Username must be at least 3 characters.',
  username_invalid_chars:  'Username may only contain letters, numbers, underscores, and hyphens.',
  password_too_short:      'Password is too short.',
  username_taken:          'That username is already taken.',
  registration_disabled:   'Registration is disabled on this server.',
  session_expired:         'Your session has expired. Please sign in again.',
  session_revoked:         'Your session has expired. Please sign in again.',
  not_authenticated:       'Your session has expired. Please sign in again.',
};
function errString(code, retryAfter) {
  if (code === 'rate_limited') {
    return `Too many attempts. Try again in ${retryAfter ?? '?'} seconds.`;
  }
  return ERR_STRINGS[code] ?? `Sign-in failed. (${code})`;
}

// ── App state singleton (avoids a full state-management framework) ──
const listeners = new Set();
const state = { route: 'loading', user: null, errorCode: null, retryAfter: 0 };
function setState(patch) {
  Object.assign(state, patch);
  listeners.forEach((fn) => fn());
}
function subscribe(fn) {
  listeners.add(fn);
  return () => listeners.delete(fn);
}

// ── Fetch wrapper (ICD-0.6.0 §5.3 + §5.6 redirect-on-401) ───────────
async function plinthFetch(url, opts) {
  const r = await fetch(url, { ...(opts ?? {}), credentials: 'include' });
  if (r.status === 401 && url !== '/api/auth/login') {
    let code = 'session_expired';
    try {
      const body = await r.clone().json();
      if (body && typeof body.error === 'string') code = body.error;
    } catch (_) { /* ignore body parse errors */ }
    setState({ route: 'login', user: null, errorCode: code, retryAfter: 0 });
    throw new Error('redirect-on-401');
  }
  return r;
}

// ── Top-level error boundary (ICD-0.6.0 §7 + ICD-0.6.3 §6) ───────────
class Boundary extends Component {
  constructor(props) { super(props); this.state = { thrown: null }; }
  componentDidCatch(error, info) {
    // ICD-0.6.3 §6 — kernel-side audit emission. Single-purpose
    // capability `audit.emit_boundary` ignores client-supplied action
    // names and pins the literal `ext.shell.frontend.boundary.caught`
    // (per Phase 0 verification: `audit.log()` JS binding requires
    // `ext.` prefix; recorded as deviation in §17). Fire-and-forget;
    // emission failures are swallowed to prevent recursive boundary
    // catches.
    // eslint-disable-next-line no-console
    console.error('[shell] boundary caught', { error, info, route: state.route });
    const detail = sanitizeBoundaryPayload(error, info, /*panelId=*/null);
    // Capability name `shell.audit.emit` deviates from ICD §A.4's
    // `audit.emit_boundary` — see §17 deviations (CF7 forces cap
    // namespace to manifest.name=`shell`; rule regex disallows
    // underscore segments).
    plinthCall('shell.audit.emit', detail).catch((e) => {
      // eslint-disable-next-line no-console
      console.error('[shell] shell.audit.emit failed', e);
    });
    this.setState({ thrown: error });
  }
  render(props, st) {
    if (st.thrown) {
      return html`
        <main>
          <div class="boundary-fallback">
            <p>Something went wrong.</p>
            <button onClick=${() => window.location.reload()}>Reload</button>
          </div>
        </main>`;
    }
    return props.children;
  }
}

// ── Test-only deliberate-throw seam (ICD-0.6.0 §13 E.01) ─────────────
function ForceThrow() {
  throw new Error('shell boundary test throw');
}

// ── Login form (ICD-0.6.0 §5.1 + §5.2 + §5.4 + OQ3 countdown) ───────
class LoginForm extends Component {
  constructor(props) {
    super(props);
    this.state = {
      username: '',
      password: '',
      error: props.initialErrorCode ?? null,
      submitting: false,
      lockoutSeconds: 0,
    };
    this.lockoutTimer = null;
  }
  componentWillUnmount() {
    if (this.lockoutTimer) clearInterval(this.lockoutTimer);
  }
  startLockout(seconds) {
    this.setState({ lockoutSeconds: seconds });
    if (this.lockoutTimer) clearInterval(this.lockoutTimer);
    this.lockoutTimer = setInterval(() => {
      const next = this.state.lockoutSeconds - 1;
      if (next <= 0) {
        clearInterval(this.lockoutTimer);
        this.lockoutTimer = null;
        this.setState({ lockoutSeconds: 0 });
      } else {
        this.setState({ lockoutSeconds: next });
      }
    }, 1000);
  }
  async submit(ev) {
    ev.preventDefault();
    if (this.state.submitting || this.state.lockoutSeconds > 0) return;
    this.setState({ submitting: true, error: null });
    try {
      const r = await plinthFetch('/api/auth/login', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          username: this.state.username,
          password: this.state.password,
        }),
      });
      if (r.status === 200) {
        const session = await plinthFetch('/api/auth/session');
        if (session.status === 200) {
          const sessionBody = await session.json();
          setState({ route: 'authenticated',
                     user: sessionBody.user ?? sessionBody,
                     errorCode: null });
          return;
        }
        this.setState({ submitting: false, error: 'not_authenticated' });
        return;
      }
      const body = await r.json().catch(() => ({}));
      const code = body.error ?? `http_${r.status}`;
      // OQ3: rate-limit lockout disables submit + shows countdown.
      if (r.status === 429) {
        const retryAfter = Number(body.retry_after) || 60;
        this.startLockout(retryAfter);
      }
      this.setState({
        submitting: false,
        error: code,
        password: '',
        retryAfter: Number(body.retry_after) || 0,
      });
    } catch (err) {
      // Server unreachable / non-JSON / network — generic state.
      this.setState({ submitting: false, error: 'server_unreachable', password: '' });
    }
  }
  render(_props, st) {
    const locked = st.lockoutSeconds > 0;
    const errorText = st.error
      ? (locked ? errString('rate_limited', st.lockoutSeconds) : errString(st.error, st.retryAfter))
      : '';
    return html`
      <main>
        <form class="login-card" onSubmit=${(e) => this.submit(e)}>
          <h1>Sign in to Plinth</h1>
          <label>
            Username
            <input
              type="text" name="username" required autocomplete="username"
              value=${st.username}
              onInput=${(e) => this.setState({ username: e.target.value })}
              disabled=${st.submitting} />
          </label>
          <label>
            Password
            <input
              type="password" name="password" required autocomplete="current-password"
              value=${st.password}
              onInput=${(e) => this.setState({ password: e.target.value })}
              disabled=${st.submitting} />
          </label>
          <button type="submit" disabled=${st.submitting || locked}>
            ${locked
              ? `Try again in ${st.lockoutSeconds}s`
              : (st.submitting ? 'Signing in…' : 'Sign In')}
          </button>
          <div class="login-error">${errorText}</div>
        </form>
      </main>`;
  }
}

// ── Authenticated frame (ICD-0.6.0 §6) ──────────────────────────────
class AuthFrame extends Component {
  constructor(props) {
    super(props);
    this.state = { popoverOpen: false };
    this.onDocClick = (ev) => {
      if (!this.avatarRef) return;
      if (this.avatarRef.contains(ev.target)) return;
      if (this.state.popoverOpen) this.setState({ popoverOpen: false });
    };
  }
  componentDidMount() { document.addEventListener('click', this.onDocClick); }
  componentWillUnmount() { document.removeEventListener('click', this.onDocClick); }
  async signOut() {
    try {
      await plinthFetch('/api/auth/logout', { method: 'POST' });
    } catch (_) { /* surfaces in boundary if it throws */ }
    setState({ route: 'login', user: null, errorCode: null });
  }
  setTheme(value) {
    if (value !== 'light' && value !== 'dark' && value !== 'system') return;
    setPref(PREF_KEYS.THEME, value);
    applyTheme(value);
    this.forceUpdate();  // re-render popover with new selected value
  }
  setScale(value) {
    const pct = parseInt(value, 10);
    if (!Number.isInteger(pct) || pct < 80 || pct > 175) return;
    setPref(PREF_KEYS.SCALE, pct);
    applyScale(pct);
    this.forceUpdate();
  }
  render(props) {
    const username = props.user?.username ?? '';
    const initial = username ? username[0].toUpperCase() : '?';
    const prefs = readPrefs();
    const theme = (prefs[PREF_KEYS.THEME] === 'light'
                   || prefs[PREF_KEYS.THEME] === 'dark'
                   || prefs[PREF_KEYS.THEME] === 'system')
                  ? prefs[PREF_KEYS.THEME] : 'system';
    const scale = (Number.isInteger(prefs[PREF_KEYS.SCALE])
                   && prefs[PREF_KEYS.SCALE] >= 80
                   && prefs[PREF_KEYS.SCALE] <= 175)
                  ? prefs[PREF_KEYS.SCALE] : 100;
    return html`
      <header class="topbar">
        <div class="zone zone-home" aria-label="Home">
          <svg class="chev" viewBox="0 0 16 16" aria-hidden="true">
            <path d="M2 8 L8 2 L14 8 V14 H10 V10 H6 V14 H2 Z"
                  fill="none" stroke="currentColor" stroke-width="1.4"/>
          </svg>
        </div>
        <div class="zone zone-app-name">
          <span class="mark" aria-hidden="true"></span>
          <span>Plinth</span>
          <svg class="chev" viewBox="0 0 16 16" aria-hidden="true">
            <path d="M4 6 L8 10 L12 6" fill="none"
                  stroke="currentColor" stroke-width="1.4"/>
          </svg>
        </div>
        <div class="zone zone-tray"></div>
        <div class="zone zone-avatar"
             ref=${(el) => { this.avatarRef = el; }}
             style="position: relative;">
          <button onClick=${() => this.setState({ popoverOpen: !this.state.popoverOpen })}>
            <span class="avatar-circle">${initial}</span>
            <svg class="chev" viewBox="0 0 16 16" aria-hidden="true">
              <path d="M4 6 L8 10 L12 6" fill="none"
                    stroke="currentColor" stroke-width="1.4"/>
            </svg>
          </button>
          ${this.state.popoverOpen ? html`
            <div class="popover" role="menu">
              <div class="popover-row">
                <label for="shell-theme-select">Theme</label>
                <select id="shell-theme-select"
                        value=${theme}
                        onChange=${(e) => this.setTheme(e.target.value)}>
                  <option value="system">System</option>
                  <option value="light">Light</option>
                  <option value="dark">Dark</option>
                </select>
              </div>
              <div class="popover-row">
                <label for="shell-scale-select">Scale</label>
                <select id="shell-scale-select"
                        value=${String(scale)}
                        onChange=${(e) => this.setScale(e.target.value)}>
                  ${SCALE_PRESETS.map((p) => html`
                    <option value=${String(p)}>${p}%</option>`)}
                </select>
              </div>
              <hr class="popover-sep" />
              <button role="menuitem" onClick=${() => this.signOut()}>Sign Out</button>
            </div>` : null}
        </div>
      </header>
      <main>Hello, ${username}</main>`;
  }
}

// ── Root component (ICD-0.6.0 §4.4 boot sequence) ───────────────────
class App extends Component {
  componentDidMount() {
    this.unsub = subscribe(() => this.forceUpdate());
    // E.01 test-only seam — query string toggles a deliberate-throw component
    // so the boundary fallback can be exercised in browser smoke tests.
    if (window.location.search.includes('force-throw=1')) {
      setState({ route: 'force-throw', user: null });
      return;
    }
    // Initial session probe uses raw fetch (not plinthFetch) so the
    // redirect-on-401 wrapper does not fire on the very first visit —
    // a missing cookie is the expected new-visitor state, not a
    // "session expired" condition.
    fetch('/api/auth/session', { credentials: 'include' })
      .then(async (r) => {
        if (r.status === 200) {
          const sessionBody = await r.json();
          setState({ route: 'authenticated',
                     user: sessionBody.user ?? sessionBody,
                     errorCode: null });
        } else if (r.status === 401) {
          setState({ route: 'login', user: null, errorCode: null });
        } else {
          setState({ route: 'login', user: null, errorCode: 'server_unreachable' });
        }
      })
      .catch(() => {
        if (state.route === 'loading') {
          setState({ route: 'login', user: null, errorCode: 'server_unreachable' });
        }
      });
  }
  componentWillUnmount() { if (this.unsub) this.unsub(); }
  render() {
    if (state.route === 'force-throw') return html`<${ForceThrow} />`;
    if (state.route === 'loading') {
      return html`<main>Loading…</main>`;
    }
    if (state.route === 'authenticated' && state.user) {
      return html`<${AuthFrame} user=${state.user} />`;
    }
    return html`<${LoginForm} initialErrorCode=${state.errorCode} />`;
  }
}

render(html`<${Boundary}><${App} /></${Boundary}>`,
       document.getElementById('root'));
