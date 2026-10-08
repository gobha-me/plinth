// Plinth shell entry — ICD-0.6.0 §4.4 boot sequence + §5 login + §6
// four-zone topbar + §7 top-level error boundary + ICD-0.6.2 §7
// avatar-popover theme + scale controls. Single-file ES module;
// Preact + htm vendored under ./vendor/ per OQ1 (B). No build step.

import { h, render, Component } from 'preact';
import htm from 'htm';
import {
  call as plinthCall,
  activateRealtimeSession,
  retireRealtimeSession,
  withCsrf,
} from '@plinth/frontend/sdk';
import { Launcher } from './launcher/launcher.js';
import { createFloatReservations } from './panels/float-reservations.js';
import { DocumentInteractionOwner } from './panels/interaction-owner.js';
import { FloatPreferenceIoOwner } from './panels/float-preferences.js';
const html = htm.bind(h);

// ICD-0.6.3 §6.5 — sanitize boundary detail before audit emission.
// Length-cap per ICD §6.5: error_message 1024, error_stack 8192,
// component_path 8192. Production builds omit error_stack per OQ6
// (gated on `window.__PLINTH_PRODUCTION__` configured by runtime-config.js).
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
    if (window.__PLINTH_PRODUCTION__ === false) {
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
// The local mirror is only the synchronous first-paint bridge. Authenticated
// get_all hydration is authoritative; user actions apply and mirror only after
// a successful server write. Each mounted frame owns its async work.
const PREF_KEYS = Object.freeze({ THEME: 'shell.theme',
                                  SCALE: 'shell.scale_pct' });
const SCALE_PRESETS = Object.freeze([80, 90, 100, 110, 125, 150, 175]);

function readPrefs() {
  try {
    const prefs = JSON.parse(localStorage.getItem('shellPrefs') || '{}');
    return prefs && typeof prefs === 'object' && !Array.isArray(prefs) ? prefs : {};
  }
  catch (_) { return {}; }
}
let currentTheme = readPrefs()[PREF_KEYS.THEME];
function preferenceValues(prefs) {
  return {
    [PREF_KEYS.THEME]: ['light', 'dark', 'system'].includes(prefs[PREF_KEYS.THEME])
      ? prefs[PREF_KEYS.THEME] : 'system',
    [PREF_KEYS.SCALE]: Number.isInteger(prefs[PREF_KEYS.SCALE]) &&
      prefs[PREF_KEYS.SCALE] >= 80 && prefs[PREF_KEYS.SCALE] <= 175
      ? prefs[PREF_KEYS.SCALE] : 100,
  };
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
  currentTheme = want;
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
    const stored = currentTheme;
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
  username_too_short:      'Username must be at least 3 characters.',
  username_too_long:       'Username must be at most 64 characters.',
  username_invalid_chars:  'Username may only contain letters, numbers, underscores, and hyphens.',
  password_too_short:      'Password is too short.',
  password_too_long:       'Password must be at most 1024 bytes.',
  invalid_request:         'The submitted account details are invalid.',
  registration_unavailable:'Registration is not available.',
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
let sessionGeneration = 0;
// These owners survive login/logout; native imports and preference I/O are not
// cancelled merely by replacing a frame. No admission/storage port is installed.
const floatReservations = createFloatReservations();
const floatPreferenceIoOwner = new FloatPreferenceIoOwner();
const floatInteractionOwner = new DocumentInteractionOwner({ reservations: floatReservations });
let currentFloatFrame = null;
function retireFloatFrame() {
  currentFloatFrame?.retire();
  currentFloatFrame = null;
}
function endSession(code) {
  retireFloatFrame();
  sessionGeneration++;
  retireRealtimeSession(code || 'session_ended');
  setState({ route: 'login', user: null, errorCode: code || null, retryAfter: 0 });
}
function beginSession(user) {
  retireFloatFrame();
  sessionGeneration++;
  activateRealtimeSession();
  setState({ route: 'authenticated', user, errorCode: null });
}
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
  const generation = sessionGeneration;
  const r = await fetch(url, withCsrf(url, {
    ...(opts ?? {}),
    credentials: 'include',
  }));
  if (r.status === 401 && url !== '/api/auth/login') {
    if (generation !== sessionGeneration) throw new Error('superseded session response');
    let code = 'session_expired';
    try {
      const body = await r.clone().json();
      if (body && typeof body.error === 'string') code = body.error;
    } catch (_) { /* ignore body parse errors */ }
    if (generation !== sessionGeneration) throw new Error('superseded session response');
    endSession(code);
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
      registrationMode: 'disabled',
      registering: false,
      inviteToken: '',
      registrationProcessed: false,
    };
    this.lockoutTimer = null;
    this.retired = false;
    this.generation = sessionGeneration;
  }
  isCurrent() { return !this.retired && this.generation === sessionGeneration; }
  componentDidMount() {
    plinthFetch('/api/auth/registration')
      .then(async (r) => {
        if (!this.isCurrent()) return;
        if (r.status !== 200) return;
        const body = await r.json();
        if (!this.isCurrent()) return;
        if (body.mode === 'invite' || body.mode === 'open') {
          this.setState({ registrationMode: body.mode });
        }
      })
      .catch(() => {});
  }
  componentWillUnmount() {
    this.retired = true;
    if (this.lockoutTimer) clearInterval(this.lockoutTimer);
  }
  startLockout(seconds) {
    if (!this.isCurrent()) return;
    this.setState({ lockoutSeconds: seconds });
    if (this.lockoutTimer) clearInterval(this.lockoutTimer);
    this.lockoutTimer = setInterval(() => {
      if (!this.isCurrent()) return;
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
    if (!this.isCurrent() || this.state.submitting || this.state.lockoutSeconds > 0) return;
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
      if (!this.isCurrent()) return;
      if (r.status === 200) {
        const session = await plinthFetch('/api/auth/session');
        if (!this.isCurrent()) return;
        if (session.status === 200) {
          const sessionBody = await session.json();
          if (!this.isCurrent()) return;
          beginSession(sessionBody.user ?? sessionBody);
          return;
        }
        this.setState({ submitting: false, error: 'not_authenticated' });
        return;
      }
      const body = await r.json().catch(() => ({}));
      if (!this.isCurrent()) return;
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
      if (!this.isCurrent()) return;
      // Server unreachable / non-JSON / network — generic state.
      this.setState({ submitting: false, error: 'server_unreachable', password: '' });
    }
  }
  async submitRegistration(ev) {
    ev.preventDefault();
    if (!this.isCurrent() || this.state.submitting || this.state.lockoutSeconds > 0) return;
    this.setState({ submitting: true, error: null, registrationProcessed: false });
    const body = {
      username: this.state.username,
      password: this.state.password,
    };
    if (this.state.registrationMode === 'invite') {
      body.invite_token = this.state.inviteToken;
    }
    try {
      const r = await plinthFetch('/api/auth/register', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      });
      if (!this.isCurrent()) return;
      if (r.status === 202) {
        await r.text();
        if (!this.isCurrent()) return;
        this.setState({
          submitting: false,
          registering: false,
          registrationProcessed: true,
          password: '',
          inviteToken: '',
        });
        return;
      }
      const responseBody = await r.json().catch(() => ({}));
      if (!this.isCurrent()) return;
      const code = responseBody.error ?? `http_${r.status}`;
      if (r.status === 429) {
        const retryAfter = Number(r.headers.get('Retry-After')) || 60;
        this.startLockout(retryAfter);
      }
      this.setState({
        submitting: false,
        error: code,
        password: '',
        retryAfter: Number(r.headers.get('Retry-After')) || 0,
      });
    } catch (_) {
      if (!this.isCurrent()) return;
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
        <form class="login-card"
              onSubmit=${(e) => st.registering
                ? this.submitRegistration(e)
                : this.submit(e)}>
          <h1>${st.registering ? 'Create a Plinth account' : 'Sign in to Plinth'}</h1>
          ${st.registrationProcessed && !st.registering ? html`
            <p class="login-status" role="status">
              Registration was processed. Sign in if the account was created.
            </p>` : null}
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
              type="password" name="password" required
              autocomplete=${st.registering ? 'new-password' : 'current-password'}
              value=${st.password}
              onInput=${(e) => this.setState({ password: e.target.value })}
              disabled=${st.submitting} />
          </label>
          ${st.registering && st.registrationMode === 'invite' ? html`
            <label>
              Invite token
              <input
                type="text" name="invite_token" required autocomplete="off"
                value=${st.inviteToken}
                onInput=${(e) => this.setState({ inviteToken: e.target.value })}
                disabled=${st.submitting} />
            </label>` : null}
          <button type="submit" disabled=${st.submitting || locked}>
            ${locked
              ? `Try again in ${st.lockoutSeconds}s`
              : (st.submitting
                ? (st.registering ? 'Submitting…' : 'Signing in…')
                : (st.registering ? 'Create Account' : 'Sign In'))}
          </button>
          <div class="login-error">${errorText}</div>
          ${st.registrationMode !== 'disabled' ? html`
            <button class="auth-mode-toggle" type="button"
                    disabled=${st.submitting}
                    onClick=${() => this.setState({
                      registering: !st.registering,
                      error: null,
                      password: '',
                      inviteToken: '',
                      registrationProcessed: false,
                    })}>
              ${st.registering ? 'Back to Sign In' : 'Create an Account'}
            </button>` : null}
        </form>
      </main>`;
  }
}

// ── Authenticated frame (ICD-0.6.0 §6) ──────────────────────────────
class AuthFrame extends Component {
  constructor(props) {
    super(props);
    this.state = { popoverOpen: false, floatModalActive: false,
      prefs: preferenceValues(readPrefs()), preferenceError: null };
    this.retired = false;
    this.generation = sessionGeneration;
    this.floatInteraction = floatInteractionOwner.beginFrame({ isCurrent: () => this.isCurrent() });
    currentFloatFrame = this.floatInteraction;
    this.preferenceVersions = new Map(Object.values(PREF_KEYS).map(key => [key, 0]));
    this.appliedVersions = new Map(Object.values(PREF_KEYS).map(key => [key, 0]));
    this.preferenceWrites = new Map();
    this.onDocClick = (ev) => {
      if (!this.isCurrent() || this.state.floatModalActive || !this.avatarRef) return;
      if (this.avatarRef.contains(ev.target)) return;
      if (this.state.popoverOpen) this.setState({ popoverOpen: false });
    };
  }
  isCurrent() { return !this.retired && this.generation === sessionGeneration; }
  componentDidMount() {
    document.addEventListener('click', this.onDocClick);
    this.hydratePreferences();
  }
  componentWillUnmount() {
    this.retired = true;
    this.floatInteraction.retire();
    if (currentFloatFrame === this.floatInteraction) currentFloatFrame = null;
    document.removeEventListener('click', this.onDocClick);
  }
  preferenceFailed() {
    if (!this.isCurrent()) return;
    this.setState(() => this.isCurrent()
      ? { preferenceError: 'Preferences could not be saved or loaded.' } : null);
  }
  floatModalChanged(blocked) {
    if (!this.isCurrent() || this.state.floatModalActive === blocked) return;
    this.setState({ floatModalActive: blocked, popoverOpen: false });
  }
  applyPreference(key, value, version) {
    if (!this.isCurrent()) return;
    this.appliedVersions.set(key, version);
    setPref(key, value);
    if (key === PREF_KEYS.THEME) applyTheme(value);
    else applyScale(value);
    this.setState(previous => this.isCurrent() && this.appliedVersions.get(key) === version
      ? { prefs: { ...previous.prefs, [key]: value }, preferenceError: null } : null);
  }
  async hydratePreferences() {
    const versions = new Map(this.appliedVersions);
    try {
      const result = await plinthCall('shell.preferences.get_all');
      if (!this.isCurrent()) return;
      if (!Array.isArray(result?.entries)) throw new Error('invalid preference response');
      const prefs = preferenceValues(Object.fromEntries(result.entries
        .filter(entry => entry && typeof entry.key === 'string')
        .map(entry => [entry.key, entry.value])));
      for (const key of Object.values(PREF_KEYS)) {
        if (this.appliedVersions.get(key) === versions.get(key)) {
          this.applyPreference(key, prefs[key], versions.get(key));
        }
      }
    } catch { this.preferenceFailed(); }
  }
  persistPreference(key, value) {
    if (!this.isCurrent()) return;
    const version = this.preferenceVersions.get(key) + 1;
    this.preferenceVersions.set(key, version);
    const prior = this.preferenceWrites.get(key) || Promise.resolve();
    const write = prior.catch(() => {}).then(async () => {
      if (!this.isCurrent()) return;
      await plinthCall('shell.preferences.set', { key, value });
      if (this.isCurrent()) this.applyPreference(key, value, version);
    }).catch(() => this.preferenceFailed());
    this.preferenceWrites.set(key, write);
  }
  async signOut() {
    if (!this.isCurrent()) return;
    try {
      const response = await plinthFetch('/api/auth/logout', { method: 'POST' });
      if (response.ok && this.isCurrent()) {
        endSession(null);
      }
    } catch (_) {
      // A terminal 401 already moved the shell to login in plinthFetch. Keep
      // the authenticated frame for CSRF rejection and transport failure: the
      // server may still own a valid session and must not be misrepresented as
      // signed out.
    }
  }
  sessionEnded(code) {
    if (this.isCurrent()) endSession(code);
  }
  setTheme(value) {
    if (value !== 'light' && value !== 'dark' && value !== 'system') return;
    this.persistPreference(PREF_KEYS.THEME, value);
  }
  setScale(value) {
    const pct = Number(value);
    if (!Number.isInteger(pct) || pct < 80 || pct > 175) return;
    this.persistPreference(PREF_KEYS.SCALE, pct);
  }
  render(props) {
    const username = props.user?.username ?? '';
    const initial = username ? username[0].toUpperCase() : '?';
    const prefs = this.state.prefs;
    const theme = (prefs[PREF_KEYS.THEME] === 'light'
                   || prefs[PREF_KEYS.THEME] === 'dark'
                   || prefs[PREF_KEYS.THEME] === 'system')
                  ? prefs[PREF_KEYS.THEME] : 'system';
    const scale = (Number.isInteger(prefs[PREF_KEYS.SCALE])
                   && prefs[PREF_KEYS.SCALE] >= 80
                   && prefs[PREF_KEYS.SCALE] <= 175)
                  ? prefs[PREF_KEYS.SCALE] : 100;
    const userControls = html`
      <div class="zone zone-avatar"
           inert=${this.state.floatModalActive ? true : undefined}
           ref=${(el) => { this.avatarRef = el; }}
           style="position: relative;">
          <button onClick=${() => {
            if (this.isCurrent() && !this.state.floatModalActive) {
              this.setState({ popoverOpen: !this.state.popoverOpen });
            }
          }} disabled=${this.state.floatModalActive}>
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
              ${this.state.preferenceError ? html`<p role="status">${this.state.preferenceError}</p>` : null}
              <button role="menuitem" onClick=${() => this.signOut()}>Sign Out</button>
            </div>` : null}
      </div>`;
    return html`<${Launcher}
      user=${props.user}
      floatInteraction=${this.floatInteraction}
      floatReservations=${floatReservations}
      floatPreferenceIoOwner=${floatPreferenceIoOwner}
      onModalChange=${(blocked) => this.floatModalChanged(blocked)}
      userControls=${userControls}
      onSessionEnd=${(code) => this.sessionEnded(code)} />`;
  }
}

// ── Root component (ICD-0.6.0 §4.4 boot sequence) ───────────────────
class App extends Component {
  componentDidMount() {
    this.retired = false;
    const generation = sessionGeneration;
    const isCurrent = () => !this.retired && generation === sessionGeneration && state.route === 'loading';
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
        if (!isCurrent()) return;
        if (r.status === 200) {
          const sessionBody = await r.json();
          if (!isCurrent()) return;
          beginSession(sessionBody.user ?? sessionBody);
        } else if (r.status === 401) {
          await r.text();
          if (!isCurrent()) return;
          endSession(null);
        } else {
          await r.text();
          if (!isCurrent()) return;
          setState({ route: 'login', user: null, errorCode: 'server_unreachable' });
        }
      })
      .catch(() => {
        if (isCurrent()) {
          setState({ route: 'login', user: null, errorCode: 'server_unreachable' });
        }
      });
  }
  componentWillUnmount() {
    this.retired = true;
    if (this.unsub) this.unsub();
  }
  render() {
    if (state.route === 'force-throw') return html`<${ForceThrow} />`;
    if (state.route === 'loading') {
      return html`<main>Loading…</main>`;
    }
    if (state.route === 'authenticated' && state.user) {
      return html`<${AuthFrame} key=${sessionGeneration} user=${state.user} />`;
    }
    return html`<${LoginForm} key=${sessionGeneration} initialErrorCode=${state.errorCode} />`;
  }
}

render(html`<${Boundary}><${App} /></${Boundary}>`,
       document.getElementById('root'));
