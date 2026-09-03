// shell.zip/client/panels/loader.js
//
// Panel loader — ICD-0.6.3 §4.2.
//
// `loadPanel(extName, panelId, container, opts?) → { panelApi, instance }`
// drives the full lifecycle: fetch panels.json from the asset server,
// resolve the panel module URL, dynamic-import the module, build the
// panelApi via `makePanelApi`, call the module's default export to
// construct the Preact component, render into the container, and fire
// onActivate.
//
// In v0.6.3 the only consumer is the test fixture (manual FE smoke);
// 0.6.4 wires the topbar to call `loadPanel` on user navigation. The
// per-panel keyboard-shortcut dispatcher is wired here so v0.6.3
// shortcuts work end-to-end (K.* manual smoke).
//
// Implementation deviations recorded in §17:
//   - panels.json fetched from `/ext/{name}/{version}/panels.json` via
//     same-origin fetch (asset server serves the path per ICD-0.6.1 §4).
//   - The "find active package row" step uses a per-name fetch; a
//     proper kernel `plinth.panels.list` API is deferred to ICD-0.6.4
//     per `feedback_icd_horizon.md`.

import { h, render } from '../vendor/preact.module.js';
import { makePanelApi, normaliseCombo } from './panel_api.js';

// Single shell-wide active panel handle. 0.6.3 only mounts one primary
// panel at a time; 0.6.5 will extend this for floats.
let activePanel = null;
let keydownInstalled = false;

function installKeydownDispatcher() {
    if (keydownInstalled) { return; }
    keydownInstalled = true;
    document.addEventListener('keydown', (e) => {
        if (!activePanel) { return; }
        const mods = [];
        if (e.altKey)   { mods.push('Alt'); }
        if (e.ctrlKey)  { mods.push('Ctrl'); }
        if (e.metaKey)  { mods.push('Meta'); }
        if (e.shiftKey) { mods.push('Shift'); }
        const key   = e.key.length === 1 ? e.key.toUpperCase() : e.key;
        let combo;
        try { combo = normaliseCombo([...mods, key].join('+')); }
        catch { return; }
        const cb = activePanel.api.__shell_internal.getShortcuts().get(combo);
        if (typeof cb === 'function') {
            e.preventDefault();
            try { cb(e); }
            catch (err) {
                // eslint-disable-next-line no-console
                console.error('[plinth.shortcut handler]', err);
                throw err;  // bubble to top-level boundary per §3.5 chain abort
            }
        }
    });
}

// Resolve the active package row by name. The shell's own
// `register_active_frontend_routes` handler reads this from
// plinth.packages — we mirror the lookup via a fetch to a
// well-known manifest path. Test fixtures install via real
// POST /api/packages so the route exists.
async function fetchPanelsJson(extName, extVersion) {
    const url  = `/ext/${encodeURIComponent(extName)}/${encodeURIComponent(extVersion)}/panels.json`;
    const resp = await fetch(url, { credentials: 'include' });
    if (!resp.ok) {
        throw new Error(`panels.json fetch failed: ${resp.status} ${resp.statusText} (${url})`);
    }
    return resp.json();
}

// In 0.6.3 the caller passes both extension name + version; the proper
// kernel lookup that fills both from the active row is deferred to
// 0.6.4 (panels-query API per OQ4). Test fixtures know the version
// because they installed the fixture themselves.
//
// `opts.panel` lets callers pass the panel struct directly (skipping the
// panels.json fetch). 0.6.3 manual FE smoke uses this — the asset
// server only serves files under `client/`, so panels.json (at package
// root) isn't reachable until either 0.6.4 ships the panels-query API
// or 0.6.0.N relaxes the asset server scope. Until then, the architect
// hand-passes the panel struct.
export async function loadPanel(extName, extVersion, panelId, container, opts) {
    opts = opts || {};
    installKeydownDispatcher();

    let panel = opts.panel;
    if (!panel) {
        const panelsJson = await fetchPanelsJson(extName, extVersion);
        panel = (panelsJson.panels || []).find((p) => p.id === panelId);
    }
    if (!panel) {
        throw new Error(`panel not found: ${extName}#${panelId}`);
    }
    if (!panel.client_path) {
        throw new Error(`panel ${extName}#${panelId} missing client_path`);
    }

    // panels.json `client_path` is relative to `client/panels/` per the
    // v0.4.4 panels-manifest validator. The asset server's `/ext/{name}/
    // {version}/(.*)` regex strips the `client/` prefix automatically
    // (`client_root` resolves to `<data_dir>/extensions/<name>/<version>/
    // client/`), so the URL we build is `/ext/{name}/{version}/panels/{path}`.
    const moduleUrl = `/ext/${encodeURIComponent(extName)}/${encodeURIComponent(extVersion)}/panels/${panel.client_path}`;
    const mod       = await import(moduleUrl);
    if (typeof mod.default !== 'function') {
        throw new Error(`panel ${extName}#${panelId} module has no default export factory`);
    }

    const panelApi = makePanelApi({
        shell:      opts.shell || { notifyDirtyChange() {} },
        panel,
        context:    opts.context || {},
        packageRow: { name: extName, version: extVersion },
    });
    const Component = mod.default(panelApi);

    // Tear down any previously active panel before mounting the new one.
    if (activePanel) {
        try { activePanel.api.__shell_internal.fireDeactivate(); }
        catch (e) {
            // eslint-disable-next-line no-console
            console.error('[loader] previous panel onDeactivate threw:', e);
        }
        try { render(null, activePanel.container); }
        catch { /* noop */ }
        activePanel.api.__shell_internal.unbind();
    }
    activePanel = { api: panelApi, container };

    render(h(Component, {}), container);
    panelApi.__shell_internal.fireActivate();

    return { panelApi, panel };
}

// Test seam — exposes the active panel handle so manual FE smoke +
// future browser-harness tests can introspect lifecycle state.
export function __activePanelForTest() {
    return activePanel;
}
