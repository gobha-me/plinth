// Actual shipping float mechanism and vendored Preact. Admission, panel modules
// and authenticated frames below are explicitly local fixtures, NOT installed
// API/openFloat/resolver/Jump or native authorization proof.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { dirname, extname, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';

const client = resolve(dirname(fileURLToPath(import.meta.url)), '../../client/shell/client');
const sources = ['panels/float-model.js', 'panels/float-reservations.js', 'panels/float-manager.js',
    'panels/interaction-owner.js', 'panels/float-chrome.js', 'panels/panel_api.js', 'css/floats.css',
    'sdk.js', 'data-query.js', 'data-controller.js', 'vendor/preact.module.js', 'vendor/preact-hooks.module.js',
    'launcher/launcher.js', 'launcher/model.js', 'panels/loader.js', 'panels/float-preferences.js',
    'css/launcher.css', 'css/tokens.css'];
const identity = async () => Object.fromEntries(await Promise.all(sources.map(async path =>
    [path, createHash('sha256').update(await readFile(resolve(client, path))).digest('hex')])));
async function bounded(operation, label, milliseconds = 5000) {
    let timer;
    try {
        return await Promise.race([Promise.resolve().then(operation), new Promise((_, reject) => {
            timer = setTimeout(() => reject(new Error(`${label}: deadline ${milliseconds}ms`)), milliseconds);
        })]);
    } finally { clearTimeout(timer); }
}
const html = `<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<link rel="stylesheet" href="/css/tokens.css"><link rel="stylesheet" href="/css/floats.css">
<style>#topbar { height:4rem; } #primary { padding:1rem; } .float-panel-content { padding:.5rem; }</style>
<div id="topbar"><button id="home">Home</button><button id="avatar">Avatar</button></div>
<main id="primary"><h1 tabindex="-1">Primary fixture</h1><button>Primary action</button></main><div id="host"></div>
<script type="importmap">{"imports":{"preact":"/vendor/preact.module.js","preact/hooks":"/vendor/preact-hooks.module.js"}}</script>
<script type="module" src="/fixture-harness.js"></script>`;
const panel = name => `import { h } from 'preact'; import { useLayoutEffect } from 'preact/hooks';
export default function(api) {
  const metric = window.__floatMetrics[${JSON.stringify(name)}] ||= { factories:0,activations:0,deactivations:0,mounts:0,unmounts:0,shortcuts:0 };
  metric.factories++; window.__floatApis.set(${JSON.stringify(name)},api);
  ${name === 'broken' ? "throw new Error('local fixture factory failure');" : ''}
  api.onActivate(() => metric.activations++); api.onDeactivate(() => metric.deactivations++);
  api.registerShortcut('Ctrl+Y', () => metric.shortcuts++);
  return function FixtureFloat() {
    useLayoutEffect(() => { metric.mounts++; return () => metric.unmounts++; }, []);
    return h('article', { 'data-fixture-panel':${JSON.stringify(name)} },
      h('label',null,${JSON.stringify(`Draft ${name}`)},h('input',{ 'aria-label':${JSON.stringify(`Draft ${name}`)}, defaultValue:'untouched' })),
      h('button',{ onClick:()=>api.setDirty(true) },'Mark dirty'),
      h('button',{ onClick:()=>api.setDirty(false) },'Mark clean'));
  };
}`;
const harness = `
import { h, render } from 'preact';
import { FloatManager } from '/panels/float-manager.js';
import { FloatLayer } from '/panels/float-chrome.js';
import { createFloatReservations } from '/panels/float-reservations.js';
import { DocumentInteractionOwner } from '/panels/interaction-owner.js';
import { retireRealtimeSession } from '/sdk.js';
const host = document.getElementById('host'), topbar = document.getElementById('topbar'), primary = document.getElementById('primary');
const reservations = createFloatReservations(), owner = new DocumentInteractionOwner({ reservations });
const admissions = new Map(), held = new Map(), retained = new Map();
let manager, scope, frame = 0, alive = true, capture = null, primaryShortcuts = 0, faultedLayer = null;
window.__floatMetrics = {}; window.__floatApis = new Map();
const descriptor = name => ({ application_id:'fixture',generation:'fixture-generation',panel_id:name,
  capability:'fixture:1:preview',context_key:'collision-key',context:{ record_id:name } });
const adapter = { resolve(input) { return { descriptor:input,target:{ applicationId:'fixture',generation:'fixture-generation',
  version:'1.0.0',applicationTitle:'Authorized fixture app',panel:{ id:input.panel_id,title:'Panel '+input.panel_id,
  module_url:'/fixture/'+input.panel_id+'.js' } } }; } };
function importer(url) {
  const name = new URL(url).pathname.slice('/fixture/'.length,-3);
  if (name === 'held' || name.startsWith('stall-')) return new Promise(resolve => held.set(name,{resolve,url}));
  return import(url);
}
function createFrame(port = adapter) {
  render(null,host); alive = true; admissions.clear(); retained.clear(); window.__floatApis.clear();
  scope = owner.beginFrame({isCurrent:()=>alive});
  scope.registerPrimary({ isDirty:()=>false,isEligible:()=>true,dispatch:()=>primaryShortcuts++ });
  manager = new FloatManager({reservations,interaction:scope,adapter:port,importModule:importer});
  render(h(FloatLayer,{key:++frame,manager,interaction:scope,topbarElement:()=>topbar,
    backgroundElements:()=>[topbar,primary],focusFallback:()=>document.getElementById('home')}),host);
}
createFrame();
document.addEventListener('gotpointercapture',event=>{capture={element:event.target,id:event.pointerId};});
window.__floatFixture = {
  async open(name,options={}) {
    const {wait=true,...settings}=options;
    const admitted=manager.admit(descriptor(name),{trigger:document.getElementById('home'),...settings});
    if(admitted.token) admissions.set(name,admitted);
    return {status:admitted.status,id:admitted.id||null,
      ready:wait&&admitted.ready ? (await admitted.ready).status : null};
  },
  async release(name) { const pending=held.get(name); if(!pending)throw new Error('missing held import');
    held.delete(name); pending.resolve(await import(pending.url)); await new Promise(resolve=>setTimeout(resolve,0)); },
  async wait(name) {return (await admissions.get(name).ready).status;},
  minimize(name){manager.minimize(admissions.get(name).token);},
  close(name){manager.requestClose(admissions.get(name).token);},
  select(name){manager.restore(admissions.get(name).token);},
  markDirty(name){window.__floatApis.get(name).setDirty(true);},
  stamp(name){const input=manager.records.get(admissions.get(name).token).container.querySelector('input');
    retained.set(name,input);return !!input;},
  sameInput(name){return retained.get(name)===manager.records.get(admissions.get(name)?.token)?.container.querySelector('input');},
  state(){return {records:manager.snapshot().map(record=>({id:record.id,title:record.title,
    readiness:record.readiness,presentation:record.presentation,rank:record.rank,geometry:record.geometry,maximized:record.maximized,
    dirty:record.dirty,retryAvailable:record.retryAvailable,retiring:record.retiring})),
    metrics:window.__floatMetrics,budget:reservations.status(),mode:manager.workArea.mode,
    primaryShortcuts,confirmation:scope.confirmation?.kind||null,modalKind:scope.modal?.kind||null,geometryEditing:!!scope.geometry,
    capture:!!capture&&capture.element.hasPointerCapture(capture.id),
    topbarInert:topbar.inert,primaryInert:primary.inert,
    area:{top:parseFloat(document.querySelector('.float-layer')?.style.top||0),
      width:parseFloat(document.querySelector('.float-layer')?.style.width||0),height:parseFloat(document.querySelector('.float-layer')?.style.height||0)}};},
  cancelPointer(){if(!capture)throw new Error('missing pointer capture');capture.element.dispatchEvent(new PointerEvent('pointercancel',
    {bubbles:true,cancelable:true,pointerId:capture.id}));},
  beforeUnload(){const event=new Event('beforeunload',{cancelable:true});window.dispatchEvent(event);return event.defaultPrevented;},
  newFrame(available=true){alive=false;scope.retire();createFrame(available?adapter:null);},
  switcher(active){if(active)scope.setModal('switcher','fixture-switcher');else scope.clearModal('fixture-switcher');},
  async closeAtCapacity(name,replacement){let proof=null,result=null,observerError=null;
    const old=manager.records.get(admissions.get(name).token),oldChrome=old.container.closest('.float-window');
    const unsubscribe=reservations.subscribe(()=>{if(proof||reservations.status().count!==4)return;
      proof={capture:!!capture&&capture.element.hasPointerCapture(capture.id),oldChromeConnected:oldChrome.isConnected,
        oldContentConnected:old.container.isConnected};
      try { result=manager.admit(descriptor(replacement),{trigger:document.getElementById('home')}); }
      catch(error){observerError=error.message;}
      if(result.token)admissions.set(replacement,result);});
    manager.close(admissions.get(name).token);unsubscribe();
    return {proof,status:result?.status||null,ready:result?.ready?(await result.ready).status:null,observerError};},
  failChromeCleanup(){faultedLayer=document.querySelector('.float-layer');faultedLayer.remove=()=>{};
    primary.removeAttribute=name=>{if(name!=='aria-hidden')Element.prototype.removeAttribute.call(primary,name);};
    alive=false;scope.retire();return {count:reservations.status().count,poisoned:reservations.status().poisoned,
      unsaved:reservations.status().unsaved,layerConnected:faultedLayer.isConnected,layerHidden:faultedLayer.hidden,
      layerInert:faultedLayer.inert,primaryInert:primary.inert,primaryAria:primary.getAttribute('aria-hidden'),
      topbarInert:topbar.inert,probes:document.querySelectorAll('.float-safe-area').length};},
  compensateFixtureFault(){delete primary.removeAttribute;primary.removeAttribute('aria-hidden');
    if(faultedLayer){delete faultedLayer.remove;faultedLayer.remove();faultedLayer=null;}},
  retireSynchronously(){alive=false;scope.retire();return {oldChrome:document.querySelectorAll('.float-window').length,
    oldLayers:document.querySelectorAll('.float-layer').length,probes:document.querySelectorAll('.float-safe-area').length,
    capture:!!capture&&capture.element.hasPointerCapture(capture.id),topbarInert:topbar.inert,primaryInert:primary.inert};},
  async cleanup(){alive=false;scope.retire();render(null,host);for(const pending of held.values())pending.resolve(null);held.clear();
    await new Promise(resolve=>setTimeout(resolve,0));owner.dispose();retireRealtimeSession('fixture_cleanup');
    admissions.clear();retained.clear();window.__floatApis.clear();return {count:reservations.status().count,
      pending:reservations.status().pending,poisoned:reservations.status().poisoned,hostEmpty:!host.childNodes.length,
      probes:document.querySelectorAll('.float-safe-area').length,unloadRemoved:!owner.beforeUnloadInstalled,
      topbarInert:topbar.inert,primaryInert:primary.inert,capture:!!capture&&capture.element.hasPointerCapture(capture.id)};},
};
`;

const integratedHtml = `<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<link rel="stylesheet" href="/css/tokens.css"><link rel="stylesheet" href="/css/launcher.css"><link rel="stylesheet" href="/css/floats.css">
<div id="integrated-host"></div>
<script type="importmap">{"imports":{"preact":"/vendor/preact.module.js","preact/hooks":"/vendor/preact-hooks.module.js","@plinth/frontend/sdk":"/sdk.js"}}</script>
<script type="module" src="/integrated-launcher-harness.js"></script>`;
const integratedHarness = `
import {h,render} from 'preact';import {Launcher} from '/launcher/launcher.js';
import {retireRealtimeSession,getRealtimeState} from '/sdk.js';
const host=document.getElementById('integrated-host');let launcher;
window.__floatMetrics={};window.__floatApis=new Map();
render(h(Launcher,{ref:value=>{launcher=value;}}),host);
window.__integratedFloatFixture={
  async open(){const manager=launcher.floatManager;
    // Explicit fixture-local current-authority port, never a production SDK seam.
    manager.adapter={resolve:descriptor=>({descriptor,target:{applicationId:'fixture',generation:'fixture-generation',
      version:'1.0.0',applicationTitle:'Authorized fixture app',panel:{id:'integrated-float',title:'Integrated float',
      module_url:'/fixture/integrated-float.js'}}})};
    const result=manager.admit({application_id:'fixture',generation:'fixture-generation',panel_id:'integrated-float',
      capability:'fixture:1:preview',context_key:'integrated',context:{record_id:'integrated'}},{trigger:launcher.homeButton});
    return {status:result.status,ready:result.ready?(await result.ready).status:null};},
  state(){return {mode:launcher.floatManager.workArea.mode,modal:launcher.interaction.modal?.kind||null,
    focused:launcher.interaction.focused.kind,confirmation:launcher.interaction.confirmation?.kind||null,
    primaryInert:launcher.primaryMain.inert,primaryDirty:launcher.panelManager.activeDirty,
    metrics:window.__floatMetrics,budget:launcher.floatReservations.status()};},
  cleanup(){const current=launcher,primary=current.primaryMain,bar=current.topbar;
    current.interaction.retire();render(null,host);retireRealtimeSession('integrated_fixture_cleanup');
    window.__floatApis.clear();return {count:current.floatReservations.status().count,
      pending:current.floatReservations.status().pending,hostEmpty:!host.childNodes.length,
      probes:document.querySelectorAll('.float-safe-area').length,primaryInert:primary.inert,topbarInert:bar.inert,
      unloadRemoved:!current.documentInteraction.beforeUnloadInstalled,realtime:getRealtimeState().status};}
};`;
const integratedCatalog={schema_version:1,applications:[{id:'fixture',generation:'fixture-generation',version:'1.0.0',
    title:'Integrated app',panels:[{id:'primary',title:'Primary',module_url:'/ext/fixture/1.0.0/panels/primary.js'}]}]};

const before = await identity();
let browser, context, page, server, failure, cleanupPoisonExpected = false;
const connections = new Set(), cleanupErrors = [], fixtureErrors = [], pageErrors = [], completed = [];
const record = async name => (await page.evaluate(() => window.__floatFixture.state())).records
    .find(item => item.title === `Panel ${name}`);
const windowFor = name => page.locator(`.float-window[aria-labelledby]`).filter({ has: page.locator('h2', { hasText: new RegExp(`^Panel ${name}$`) }) });
const waitMode = mode => page.waitForFunction(value => window.__floatFixture.state().mode === value, mode);
async function run(name, operation) {
    try { await bounded(operation, name, 20000); }
    catch (error) { throw new Error(name, { cause: error }); }
    completed.push(name);
}
try {
    server = createServer(async (request, response) => {
        try {
            const path = new URL(request.url, 'http://localhost').pathname;
            if (path === '/favicon.ico') return response.writeHead(204).end();
            if (path === '/') return response.writeHead(200, { 'Content-Type': 'text/html' }).end(html);
            if (path === '/integrated-launcher') return response.writeHead(200, { 'Content-Type': 'text/html' }).end(integratedHtml);
            if (path === '/integrated-launcher-harness.js') return response.writeHead(200, { 'Content-Type': 'application/javascript' }).end(integratedHarness);
            if (path === '/api/frontend/applications') return response.writeHead(200, { 'Content-Type': 'application/json' }).end(JSON.stringify(integratedCatalog));
            if (path.startsWith('/api/cap/')) {
                for await (const chunk of request) { /* Consume only this owned fixture request. */ }
                const value=path.endsWith('/shell.preferences.get')?{value:null}:{ok:true};
                return response.writeHead(200, { 'Content-Type': 'application/json' }).end(JSON.stringify({ok:true,value}));
            }
            if (path === '/ext/fixture/1.0.0/panels/primary.js') return response.writeHead(200, { 'Content-Type': 'application/javascript' }).end(panel('integrated-primary'));
            if (path === '/fixture-harness.js') return response.writeHead(200, { 'Content-Type': 'application/javascript' }).end(harness);
            if (path.startsWith('/fixture/')) {
                const name = path.slice('/fixture/'.length, -3);
                assert(path.endsWith('.js') && /^[a-z][a-z0-9-]{0,63}$/.test(name));
                return response.writeHead(200, { 'Content-Type': 'application/javascript' }).end(panel(name));
            }
            const file = resolve(client, `.${path}`); assert(file.startsWith(client + sep));
            assert(sources.includes(path.slice(1)) || path === '/css/tokens.css');
            response.writeHead(200, { 'Content-Type': extname(file) === '.css' ? 'text/css' : 'application/javascript' }).end(await readFile(file));
        } catch (error) {
            fixtureErrors.push(error.message); if (!response.headersSent) response.writeHead(500); response.end();
        }
    });
    server.on('connection', socket => { connections.add(socket); socket.on('close', () => connections.delete(socket)); });
    await bounded(() => new Promise((done, reject) => { server.once('error', reject); server.listen(0, '127.0.0.1', done); }), 'fixture listen');
    browser = await bounded(() => chromium.launch({ executablePath: process.env.PLINTH_BROWSER || undefined,
        args: process.env.PLINTH_BROWSER_NO_SANDBOX === '1' ? ['--no-sandbox'] : [] }), 'Chromium launch', 15000);
    context = await browser.newContext({ viewport: { width: 1200, height: 900 } });
    page = await context.newPage(); page.setDefaultTimeout(5000);
    page.on('pageerror', error => pageErrors.push(error.message));
    await page.goto(`http://127.0.0.1:${server.address().port}/`);
    await page.waitForFunction(() => !!window.__floatFixture);
    await waitMode('desktop');

    await run('F44.01 real Preact tree, authorized text and disabled Jump', async () => {
        const duplicate = await page.evaluate(() => Promise.all([window.__floatFixture.open('one'),window.__floatFixture.open('one')]));
        assert.deepEqual(duplicate.map(item => item.status), ['admitted', 'deduplicated']);
        assert.equal(duplicate[0].id, duplicate[1].id);
        assert.deepEqual(duplicate.map(item => item.ready), ['ready', 'ready']);
        await page.getByRole('textbox', { name: 'Draft one' }).fill('unsaved retained text');
        assert.equal(await page.evaluate(() => window.__floatFixture.stamp('one')), true);
        const chrome = windowFor('one');
        assert.equal(await chrome.getByRole('button', { name: 'Jump to app' }).isDisabled(), true);
        assert.match(await chrome.locator('[id$="-float-jump-reason"]').textContent(), /unavailable.*reviewed navigation adapter/);
        assert.equal(await chrome.getByRole('heading', { name: 'Panel one' }).textContent(), 'Panel one');
        assert.equal(await chrome.locator('.float-badge').textContent(), 'Authorized fixture app');
        assert.deepEqual((await page.evaluate(() => window.__floatFixture.state())).metrics.one,
            { factories: 1, activations: 1, deactivations: 0, mounts: 1, unmounts: 0, shortcuts: 0 });
    });
    await run('F44.02 minimized state and same-instance restore without lifecycle replay', async () => {
        await windowFor('one').getByRole('button', { name: 'Minimize', exact: true }).click();
        assert.equal((await record('one')).presentation, 'minimized');
        assert.equal(await windowFor('one').isVisible(), false);
        await page.getByRole('button', { name: 'Restore Panel one — Authorized fixture app', exact: true }).click();
        assert.equal(await page.evaluate(() => window.__floatFixture.sameInput('one')), true);
        assert.equal(await page.getByRole('textbox', { name: 'Draft one' }).inputValue(), 'unsaved retained text');
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).metrics.one.activations, 1);
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).metrics.one.deactivations, 0);
        await windowFor('one').getByRole('button', { name: 'Resize', exact: true }).focus();
        await page.keyboard.press('Tab');
        assert.equal(await page.evaluate(() => document.activeElement.closest('.float-window') === null), true,
            'desktop floats must not contain ordinary Tab');
    });
    await run('F44.03 keyboard geometry 10/1 steps, accepted-only commits and Escape revert', async () => {
        const beforeGeometry = (await record('one')).geometry;
        await windowFor('one').getByRole('button', { name: 'Move', exact: true }).click();
        await page.keyboard.press('ArrowRight'); await page.keyboard.press('Shift+ArrowUp');
        assert.deepEqual((await record('one')).geometry, beforeGeometry);
        await page.keyboard.press('Escape');
        assert.deepEqual((await record('one')).geometry, beforeGeometry);
        await windowFor('one').getByRole('button', { name: 'Move', exact: true }).click();
        await page.keyboard.press('ArrowRight'); await page.keyboard.press('Enter');
        assert.deepEqual((await record('one')).geometry, { ...beforeGeometry, x: beforeGeometry.x + 10 });
        const resize = windowFor('one').getByRole('button', { name: 'Resize', exact: true });
        await resize.focus(); await page.keyboard.press('Enter');
        await page.keyboard.press('ArrowDown'); await page.keyboard.press('Shift+ArrowRight'); await page.keyboard.press('Enter');
        assert.deepEqual((await record('one')).geometry,
            { ...beforeGeometry, x: beforeGeometry.x + 10, width: beforeGeometry.width + 1, height: beforeGeometry.height + 10 });
    });
    await run('F44.04 actual pointer capture, accepted drag and cancelled geometry', async () => {
        const titlebar = windowFor('one').locator('.float-titlebar');
        const box = await titlebar.boundingBox(), beforeGeometry = (await record('one')).geometry;
        await page.mouse.move(box.x + 90, box.y + 20); await page.mouse.down();
        await page.mouse.move(box.x + 115, box.y + 38);
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).capture, true);
        assert.deepEqual((await record('one')).geometry, beforeGeometry);
        await page.mouse.up();
        assert.deepEqual((await record('one')).geometry, { ...beforeGeometry, x: beforeGeometry.x + 25, y: beforeGeometry.y + 18 });
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).capture, false);
        const after = (await record('one')).geometry, nextBox = await titlebar.boundingBox();
        await page.mouse.move(nextBox.x + 90, nextBox.y + 20); await page.mouse.down(); await page.mouse.move(nextBox.x + 120, nextBox.y + 30);
        await page.evaluate(() => window.__floatFixture.cancelPointer()); await page.mouse.up();
        assert.deepEqual((await record('one')).geometry, after);
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).capture, false);
    });
    await run('F44.05 maximize and every responsive edge retain component and normal geometry', async () => {
        const normal = (await record('one')).geometry;
        await windowFor('one').getByRole('button', { name: 'Maximize', exact: true }).click();
        assert.equal(await windowFor('one').evaluate(element => Math.round(element.getBoundingClientRect().width)), 1200);
        assert.equal((await record('one')).maximized, true);
        for (const [width, mode] of [[1025, 'desktop'], [1024, 'slide-over'], [768, 'slide-over'], [767, 'full-modal'], [1200, 'desktop']]) {
            await page.setViewportSize({ width, height: 900 }); await waitMode(mode);
            await page.waitForFunction(() => document.querySelector('.float-window')?.dataset.mode === window.__floatFixture.state().mode);
            assert.equal(await page.evaluate(() => window.__floatFixture.sameInput('one')), true);
            assert.deepEqual((await record('one')).geometry, normal);
            assert.equal((await record('one')).maximized, true);
            assert.equal(await page.locator('[aria-modal="true"]').count(), mode === 'desktop' ? 0 : 1);
            if (mode !== 'desktop') assert.equal(await windowFor('one').getByRole('button', { name: 'Restore size', exact: true }).isDisabled(), true);
        }
        await windowFor('one').getByRole('button', { name: 'Restore size', exact: true }).click();
        assert.equal((await record('one')).maximized, false);
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).metrics.one.mounts, 1);
    });
    await run('F44.06 single modal, bounded switcher, background inertness and Tab containment', async () => {
        await page.evaluate(() => window.__floatFixture.open('two'));
        await page.setViewportSize({ width: 800, height: 900 }); await waitMode('slide-over');
        assert.equal(await page.locator('[aria-modal="true"]').count(), 1);
        assert.equal(await windowFor('one').isVisible(), false);
        assert.equal((await record('one')).presentation, 'shown');
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).topbarInert, true);
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).primaryInert, true);
        await page.getByRole('button', { name: 'Focus Panel one — Authorized fixture app', exact: true }).click();
        assert.equal(await windowFor('one').isVisible(), true);
        assert.equal(await windowFor('two').isVisible(), false);
        for (let index = 0; index < 20; index++) {
            await page.keyboard.press(index % 3 ? 'Tab' : 'Shift+Tab');
            assert.equal(await page.evaluate(() => document.activeElement.closest('[aria-modal="true"]') !== null), true);
        }
    });
    await run('F44.07 dirty Cancel/Discard single confirmation and one unload guard', async () => {
        await windowFor('one').getByRole('button', { name: 'Mark dirty', exact: true }).click();
        assert.equal(await page.evaluate(() => window.__floatFixture.beforeUnload()), true);
        const close = windowFor('one').getByRole('button', { name: 'Close', exact: true });
        await close.click();
        assert.equal(await page.locator('[aria-modal="true"]').count(), 1);
        assert.equal(await windowFor('one').getAttribute('aria-modal'), null);
        assert.equal(await windowFor('one').evaluate(element => element.inert), true);
        await page.keyboard.press('Tab');
        assert.equal(await page.evaluate(() => document.activeElement.closest('.float-confirmation') !== null), true);
        await page.getByRole('button', { name: 'Cancel', exact: true }).click();
        assert.equal(await page.evaluate(() => window.__floatFixture.sameInput('one')), true);
        assert.equal((await record('one')).dirty, true);
        assert.equal(await page.evaluate(() => document.activeElement.textContent), 'Close');
        assert.equal(await page.locator('[aria-modal="true"]').count(), 1);
    });
    await run('F44.08 five owners include loading, minimized and failed; late close retains quota', async () => {
        await page.setViewportSize({ width: 1200, height: 900 }); await waitMode('desktop');
        assert.equal((await page.evaluate(() => window.__floatFixture.open('held', { wait: false }))).status, 'admitted');
        await page.keyboard.press('Control+y');
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).primaryShortcuts, 0);
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).metrics.one.shortcuts, 0);
        assert.equal((await page.evaluate(() => window.__floatFixture.open('broken'))).ready, 'load-failed');
        await page.evaluate(() => window.__floatFixture.open('three', { presentation: 'minimized' }));
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).budget.count, 5);
        assert.equal((await page.evaluate(() => window.__floatFixture.open('six'))).status, 'limit-refused');
        await page.evaluate(() => window.__floatFixture.close('held'));
        assert.equal((await record('held')).retiring, true);
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).budget.count, 5);
        assert.equal((await page.evaluate(() => window.__floatFixture.open('six'))).status, 'limit-refused');
        await page.evaluate(() => window.__floatFixture.release('held'));
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).budget.count, 4);
        assert.equal((await page.evaluate(() => window.__floatFixture.open('six'))).ready, 'ready');
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).metrics.held, undefined);
    });
    await run('F44.09 failed/loading/minimized/geometry exclude shortcuts; dirty Cancel retains capacity', async () => {
        await page.evaluate(() => window.__floatFixture.select('broken'));
        await page.keyboard.press('Control+y');
        let state = await page.evaluate(() => window.__floatFixture.state());
        assert.equal(state.primaryShortcuts, 0);
        assert.equal(Object.values(state.metrics).reduce((sum, metric) => sum + metric.shortcuts, 0), 0);
        await page.evaluate(() => window.__floatFixture.select('one'));
        await windowFor('one').getByRole('button', { name: 'Move', exact: true }).click();
        await page.keyboard.press('Control+y'); await page.keyboard.press('Escape');
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).metrics.one.shortcuts, 0);
        await windowFor('one').getByRole('button', { name: 'Close', exact: true }).click();
        await page.getByRole('button', { name: 'Cancel', exact: true }).click();
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).budget.count, 5);
        assert.equal((await page.evaluate(() => window.__floatFixture.open('seven'))).status, 'limit-refused');
        await windowFor('one').getByRole('button', { name: 'Close', exact: true }).click();
        await page.getByRole('button', { name: 'Discard', exact: true }).click();
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).budget.count, 4);
        assert.equal((await page.evaluate(() => window.__floatFixture.open('seven'))).ready, 'ready');
        state = await page.evaluate(() => window.__floatFixture.state());
        assert.equal(state.metrics.one.deactivations, 1); assert.equal(state.metrics.one.unmounts, 1);
        assert.equal(await page.evaluate(() => window.__floatFixture.beforeUnload()), false);
    });
    await run('F44.10 never-shown minimized readiness activates only on first restore', async () => {
        await page.evaluate(() => window.__floatFixture.newFrame());
        await waitMode('desktop');
        await page.evaluate(() => window.__floatFixture.open('held', { wait: false, presentation: 'minimized' }));
        await page.evaluate(() => window.__floatFixture.release('held'));
        assert.equal(await page.evaluate(() => window.__floatFixture.wait('held')), 'ready');
        let metric = (await page.evaluate(() => window.__floatFixture.state())).metrics.held;
        assert.equal(metric.factories, 1); assert.equal(metric.activations, 0);
        await page.getByRole('button', { name: 'Restore Panel held — Authorized fixture app', exact: true }).click();
        await page.keyboard.press('Control+y');
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).metrics.held.shortcuts, 1);
        await windowFor('held').getByRole('button', { name: 'Minimize', exact: true }).click();
        await page.keyboard.press('Control+y');
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).metrics.held.shortcuts, 1);
        await page.getByRole('button', { name: 'Restore Panel held — Authorized fixture app', exact: true }).click();
        metric = (await page.evaluate(() => window.__floatFixture.state())).metrics.held;
        assert.equal(metric.activations, 1); assert.equal(metric.deactivations, 0);
    });
    await run('F44.11 synchronous epoch retirement releases capture, removes old DOM and restores background', async () => {
        await page.setViewportSize({ width: 800, height: 900 }); await waitMode('slide-over');
        await page.setViewportSize({ width: 1200, height: 900 }); await waitMode('desktop');
        const box = await windowFor('held').locator('.float-titlebar').boundingBox();
        await page.mouse.move(box.x + 90, box.y + 20); await page.mouse.down(); await page.mouse.move(box.x + 95, box.y + 25);
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).capture, true);
        assert.deepEqual(await page.evaluate(() => window.__floatFixture.retireSynchronously()),
            { oldChrome: 0, oldLayers: 0, probes: 0, capture: false, topbarInert: false, primaryInert: false });
        await page.mouse.up();
        await page.evaluate(() => window.__floatFixture.newFrame()); await waitMode('desktop');
    });
    await run('F44.12 five stalled imports survive frame changes without old metadata or sixth admission', async () => {
        for (let index = 0; index < 5; index++) await page.evaluate(name => window.__floatFixture.open(name, { wait: false }), `stall-${index}`);
        await page.evaluate(() => window.__floatFixture.newFrame()); await waitMode('desktop');
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).records.length, 0);
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).budget.count, 5);
        assert.equal(await page.locator('.float-layer').getByText(/cleanup.*pending/i).count() > 0, true);
        assert.equal(await page.locator('.float-layer').getByText(/Panel stall-/).count(), 0);
        assert.equal((await page.evaluate(() => window.__floatFixture.open('eight'))).status, 'limit-refused');
        await page.evaluate(() => window.__floatFixture.newFrame()); await waitMode('desktop');
        assert.equal((await page.evaluate(() => window.__floatFixture.open('eight'))).status, 'limit-refused');
        for (let index = 0; index < 5; index++) await page.evaluate(name => window.__floatFixture.release(name), `stall-${index}`);
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).budget.count, 0);
        assert.equal((await page.evaluate(() => window.__floatFixture.open('eight'))).ready, 'ready');
        assert.equal(Object.keys((await page.evaluate(() => window.__floatFixture.state())).metrics).some(name => name.startsWith('stall-')), false);
    });
    await run('F44.13 tiny/zero work area, text scaling, contrast and reduced motion keep controls reachable', async () => {
        await page.evaluate(() => window.__floatFixture.stamp('eight'));
        await page.setViewportSize({ width: 320, height: 180 }); await waitMode('full-modal');
        await page.emulateMedia({ forcedColors: 'active', reducedMotion: 'reduce' });
        await page.evaluate(() => { document.documentElement.style.fontSize = '27px'; });
        await page.waitForFunction(() => window.__floatFixture.state().area.top === 108);
        const chrome = windowFor('eight');
        const close = chrome.getByRole('button', { name: 'Close', exact: true });
        await close.scrollIntoViewIfNeeded(); const closeBox = await close.boundingBox();
        assert(closeBox.width >= 44 && closeBox.height >= 44 && closeBox.y < 180 && closeBox.y + closeBox.height > 108);
        assert.equal(await chrome.evaluate(element => getComputedStyle(element).transitionDuration), '0s');
        await page.evaluate(() => { document.getElementById('topbar').style.height = '1000px'; }); await waitMode('deferred');
        assert.equal(await chrome.isVisible(), false);
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).budget.count, 1);
        await page.evaluate(() => { document.getElementById('topbar').style.height = ''; document.documentElement.style.fontSize = ''; });
        await page.setViewportSize({ width: 1200, height: 900 }); await waitMode('desktop');
        assert.equal(await page.evaluate(() => window.__floatFixture.sameInput('eight')), true);
    });
    await run('F44.14 absent admission adapter refuses without import or reservation', async () => {
        await page.evaluate(() => window.__floatFixture.newFrame(false)); await waitMode('desktop');
        const result = await page.evaluate(() => window.__floatFixture.open('unsupported'));
        assert.equal(result.status, 'target-unavailable');
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).budget.count, 0);
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).metrics.unsupported, undefined);
    });
    await run('F44.15 another modal lease is not overwritten; trigger-first minimize returns focus', async () => {
        await page.evaluate(() => window.__floatFixture.newFrame()); await waitMode('desktop');
        await page.evaluate(() => window.__floatFixture.open('lease'));
        await page.evaluate(() => window.__floatFixture.open('other'));
        await windowFor('other').getByRole('button', { name: 'Minimize', exact: true }).click();
        assert.equal(await page.evaluate(() => document.activeElement.id), 'home');
        await page.evaluate(() => window.__floatFixture.switcher(true));
        await page.waitForFunction(() => window.__floatFixture.state().modalKind === 'switcher');
        await page.setViewportSize({ width: 800, height: 900 }); await waitMode('slide-over');
        assert.equal(await page.locator('.float-window[aria-modal="true"]').count(), 0);
        assert.equal(await windowFor('lease').evaluate(element => element.inert), true);
        await page.evaluate(() => window.__floatFixture.switcher(false));
        await page.waitForFunction(() => window.__floatFixture.state().modalKind === 'float');
        assert.equal(await page.locator('.float-window[aria-modal="true"]').count(), 1);
    });
    await run('F44.16 direct close releases pointer and chrome before capacity observers admit sixth', async () => {
        await page.evaluate(() => window.__floatFixture.newFrame());
        await page.setViewportSize({ width: 1200, height: 900 }); await waitMode('desktop');
        for (let index=0;index<5;index++) await page.evaluate(name=>window.__floatFixture.open(name),`capacity-${index}`);
        await page.evaluate(() => window.__floatFixture.select('capacity-0'));
        const titlebar=windowFor('capacity-0').locator('.float-titlebar'),box=await titlebar.boundingBox();
        await page.mouse.move(box.x+90,box.y+20);await page.mouse.down();await page.mouse.move(box.x+100,box.y+25);
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).capture,true);
        const result=await page.evaluate(() => window.__floatFixture.closeAtCapacity('capacity-0','capacity-six'));
        assert.deepEqual(result,{proof:{capture:false,oldChromeConnected:false,oldContentConnected:false},status:'admitted',ready:'ready',observerError:null});
        await page.mouse.up();
        assert.equal((await page.evaluate(() => window.__floatFixture.state())).budget.count,5);
        await page.evaluate(() => window.__floatFixture.newFrame());
        await page.evaluate(() => window.__floatFixture.open('poison'));
        await page.setViewportSize({width:800,height:900});await waitMode('slide-over');
    });
    await run('F44.17 actual Launcher primary dirty Cancel after responsive change focuses eligible float', async () => {
        const integrated=await context.newPage();integrated.setDefaultTimeout(5000);
        let cleanupResult;
        const sockets=new Set();
        integrated.on('pageerror',error=>pageErrors.push(error.message));
        await integrated.routeWebSocket(/\/ws\/events$/,socket=>{
            sockets.add(socket);socket.onClose(()=>sockets.delete(socket));
            socket.onMessage(message=>{const frame=JSON.parse(message);
                if(frame.type==='subscribe'||frame.type==='unsubscribe')socket.send(JSON.stringify({type:frame.type+'d',channels:frame.channels}));});
            socket.send(JSON.stringify({type:'connected'}));
        });
        try {
            await integrated.setViewportSize({width:1200,height:900});
            await integrated.goto(`http://127.0.0.1:${server.address().port}/integrated-launcher`);
            await integrated.getByRole('button',{name:'Integrated app',exact:true}).click();
            await integrated.getByRole('textbox',{name:'Draft integrated-primary'}).waitFor();
            await integrated.locator('.panel-host').getByRole('button',{name:'Mark dirty',exact:true}).click();
            assert.equal((await integrated.evaluate(()=>window.__integratedFloatFixture.open())).ready,'ready');
            await integrated.getByRole('button',{name:'Home',exact:true}).click();
            await integrated.getByRole('button',{name:'Cancel',exact:true}).waitFor();
            assert.equal((await integrated.evaluate(()=>window.__integratedFloatFixture.state())).confirmation,'primary');
            await integrated.setViewportSize({width:800,height:900});
            await integrated.waitForFunction(()=>window.__integratedFloatFixture.state().mode==='slide-over');
            assert.equal(await integrated.locator('[aria-modal="true"]').count(),1);
            await integrated.getByRole('button',{name:'Cancel',exact:true}).click();
            await integrated.waitForFunction(()=>document.activeElement?.closest('.float-window')!==null);
            let state=await integrated.evaluate(()=>window.__integratedFloatFixture.state());
            assert.equal(state.confirmation,null);assert.equal(state.modal,'float');assert.equal(state.focused,'float');
            assert.equal(state.primaryInert,true);assert.equal(state.primaryDirty,true);
            assert.equal(await integrated.locator('[aria-modal="true"]').count(),1);
            assert.equal(await integrated.locator('.float-window').evaluate(element=>!element.inert&&!element.hidden),true);
            await integrated.keyboard.press('Control+y');
            state=await integrated.evaluate(()=>window.__integratedFloatFixture.state());
            assert.equal(state.metrics['integrated-float'].shortcuts,1);
            assert.equal(state.metrics['integrated-primary'].shortcuts,0,'inert primary never receives float-modal shortcuts');
            await integrated.keyboard.press('Tab');
            assert.equal(await integrated.evaluate(()=>document.activeElement.closest('.float-window')!==null),true);
        } finally {
            try {
                cleanupResult=await bounded(()=>integrated.evaluate(()=>window.__integratedFloatFixture?.cleanup()),'integrated fixture cleanup');
                assert.deepEqual(cleanupResult,{count:0,pending:0,hostEmpty:true,probes:0,primaryInert:false,
                    topbarInert:false,unloadRemoved:true,realtime:'failed'});
            } finally {await bounded(()=>integrated.close(),'integrated page close');}
            assert.equal(sockets.size,0,'integrated fixture realtime socket is closed');
        }
    });
    await run('F44.18 unverifiable chrome/background cleanup poisons even an empty document budget', async () => {
        cleanupPoisonExpected = true;
        const failed = await page.evaluate(() => window.__floatFixture.failChromeCleanup());
        assert.deepEqual(failed, { count: 0, poisoned: true, unsaved: true, layerConnected: true, layerHidden: true,
            layerInert: true, primaryInert: false, primaryAria: 'true', topbarInert: false, probes: 0 });
        assert.equal(await page.evaluate(() => window.__floatFixture.beforeUnload()), true);
        // Compensate only the exact fixture-injected faults, never clear poison or
        // reinterpret the preceding failed retirement as successful cleanup.
        await page.evaluate(() => window.__floatFixture.compensateFixtureFault());
        await page.evaluate(() => window.__floatFixture.newFrame()); await waitMode('slide-over');
        assert.equal((await page.evaluate(() => window.__floatFixture.open('refused-after-cleanup'))).status, 'cleanup-failed');
        assert.equal(await page.getByRole('button', { name: 'Reload shell', exact: true }).isVisible(), true);
    });
    assert.deepEqual(pageErrors, []); assert.deepEqual(fixtureErrors, []);
    assert.deepEqual(await identity(), before, 'production source identity changed during fixture');
} catch (error) { failure = error; }
finally {
    if (page && !page.isClosed()) {
        try {
            const result = await bounded(() => page.evaluate(() => window.__floatFixture?.cleanup()), 'fixture ownership cleanup');
            assert.deepEqual(result, { count: 0, pending: 0, poisoned: cleanupPoisonExpected, hostEmpty: true, probes: 0, unloadRemoved: true,
                topbarInert: false, primaryInert: false, capture: false });
        } catch (error) { cleanupErrors.push(error.message); }
    }
    if (context) try { await bounded(() => context.close(), 'fixture context close'); } catch (error) { cleanupErrors.push(error.message); }
    if (browser) try { await bounded(() => browser.close(), 'fixture browser close'); } catch (error) { cleanupErrors.push(error.message); }
    if (server?.listening) {
        for (const connection of connections) connection.destroy();
        try { await bounded(() => new Promise((done, reject) => server.close(error => error ? reject(error) : done())), 'fixture server close'); }
        catch (error) { cleanupErrors.push(error.message); }
    }
}
if (cleanupErrors.length) throw new AggregateError([...(failure ? [failure] : []), ...cleanupErrors.map(message => new Error(message))], 'float fixture cleanup failed');
if (failure) throw failure;
assert.equal(completed.length, 18);
for (const name of completed) console.log(`PASS ${name}`);
console.log(JSON.stringify({ floatMechanismCases: completed.length, expectedCases: 18, cleanup: 'PASS', cleanupFailurePoisonVerified: true, sourceIdentity: before,
    scope: 'actual shipping FloatManager/Chrome/interaction/reservations/model plus integrated Launcher/primary loader/preferences and vendored Preact; local catalog, capability HTTP, realtime acknowledgements, synchronous float admission and panel fixtures only; no installed API/native backend/openFloat/resolver/Jump proof' }));
