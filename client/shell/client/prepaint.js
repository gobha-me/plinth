// shell.zip/client/prepaint.js
// ICD-0.6.2 §4.5 + §5.3 — pre-paint resolver. Runs synchronously before
// any module loads or first paint so the document picks up the right
// theme palette and rem scaling without a visible flash.
//
// Source: localStorage.shellPrefs (a JSON object, e.g.
//   {"shell.theme":"light","shell.scale_pct":125}).
//
// Per ICD-0.6.2 §11 SC5, localStorage is the synchronous-startup
// bridge only; the post-init hydrate from cap.call("shell.preferences.
// get_all") will overwrite with DB-authoritative values once the
// JS-dispatch path lands (deferred to 0.6.1.N follow-up — see §17
// amendment block).
//
// Implementation deviation from ICD-0.6.2 §11 SC4 path (a):
// the SC4 prescription was an inline `<script>` block protected by
// a CSP `'sha256-…'` hash computed at build time. This ICD ships
// the resolver as an external sync script (`<script src=…>` without
// `defer`/`async`) in `<head>`. Plain sync external scripts are
// blocking by default and run before first paint reliably; the
// alternative §SC4 (c) it was paired with was the `<link rel="preload">`
// shape that *is* async. The external-sync shape preserves the
// strict `script-src 'self'` CSP without requiring a build-time
// hash recompute on every shell change. Recorded in §17 amendment
// block.

(function () {
  let prefs = {};
  try {
    const raw = localStorage.getItem('shellPrefs');
    if (raw) { prefs = JSON.parse(raw); }
  } catch (_) { /* malformed JSON → fall through to defaults */ }

  // Theme: stored value is one of 'light' / 'dark' / 'system'. Absent
  // or anything else falls back to 'system'. The resolved attribute
  // value is always 'light' or 'dark' — never 'system'.
  const stored = prefs['shell.theme'];
  const wantSystem = (stored !== 'light' && stored !== 'dark');
  const resolved = wantSystem
    ? (window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light')
    : stored;
  document.documentElement.dataset.theme = resolved;

  // Scale: stored integer 80..175; default 100. Out-of-range / wrong
  // shape → snap to 100 (defense-in-depth; the server-side validator
  // in preferences.set.js also rejects out-of-range writes).
  const raw = prefs['shell.scale_pct'];
  const pct = (Number.isInteger(raw) && raw >= 80 && raw <= 175) ? raw : 100;
  // 1 rem == 13.5 px at 100% per ICD-0.6.0 §6.3 baseline; mechanism
  // per ICD-0.6.2 §5.3.
  document.documentElement.style.fontSize = (pct * 0.135) + 'px';
})();
