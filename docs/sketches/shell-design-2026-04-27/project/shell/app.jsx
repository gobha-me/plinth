// App root — routes between Home/Notes/Homecare/Admin, manages trays + tweaks.

const EXTENSIONS = {
  notes:    { id: 'notes',    name: 'Notes',    glyph: 'N', subtabs: [
    { id: 'editor', label: 'Editor' },
    { id: 'graph',  label: 'Graph' },
    { id: 'shared', label: 'Shared' },
  ]},
  homecare: { id: 'homecare', name: 'Homecare', glyph: 'H', subtabs: [
    { id: 'clients',  label: 'Clients' },
    { id: 'schedule', label: 'Schedule' },
    { id: 'visits',   label: 'Visit log' },
    { id: 'meds',     label: 'Medications' },
  ]},
  kb:       { id: 'kb',       name: 'Knowledge Base', glyph: 'K', subtabs: [
    { id: 'search',    label: 'Search' },
    { id: 'chat',      label: 'Chat' },
    { id: 'sources',   label: 'Sources' },
    { id: 'sync',      label: 'Sync log' },
    { id: 'health',    label: 'Health' },
    { id: 'analytics', label: 'Analytics' },
    { id: 'settings',  label: 'Settings' },
  ]},
  admin:    { id: 'admin',    name: 'Admin',    glyph: 'A', subtabs: [
    { id: 'packages',  label: 'Packages' },
    { id: 'rbac',      label: 'RBAC' },
    { id: 'schedules', label: 'Schedules' },
    { id: 'groups',    label: 'Groups' },
    { id: 'audit',     label: 'Audit log' },
  ]},
  automations: { id: 'automations', name: 'Automations', glyph: 'A', subtabs: [
    { id: 'my-schedules', label: 'My schedules' },
    { id: 'scripts',      label: 'Scripts' },
    { id: 'triggers',     label: 'Deferred actions' },
  ]},
};

const DEFAULTS = /*EDITMODE-BEGIN*/{
  "theme": "dark",
  "identityTreatment": "mark",
  "trayWidth": 360,
  "uiScale": 100,
  "showAnnotation": false,
  "fallbackAvatar": false,
  "showIntegrationPoints": false,
  "showShellHoles": false,
  "showNarrow": false
}/*EDITMODE-END*/;

function App() {
  const [tweaks, setTweaks] = useTweaks(DEFAULTS);
  const [activeExt, setActiveExt] = React.useState(null); // null = Home
  const [activeSubtab, setActiveSubtab] = React.useState('editor');
  const [switcherOpen, setSwitcherOpen] = React.useState(false);
  const [tray, setTray] = React.useState(null); // 'bell' | 'avatar' | 'status' | 'media' | null
  const [mediaPlaying, setMediaPlaying] = React.useState(true);
  const fw = useFloatingWindows();

  // Apply UI scale via CSS zoom on the scale root — this scales every px
  // dimension, font, icon, gap uniformly without requiring every value to
  // be expressed in rem. (§6.3 of the shell design doc.)
  React.useEffect(() => {
    const root = document.getElementById('scale-root');
    if (root) root.style.zoom = (tweaks.uiScale || 100) / 100;
  }, [tweaks.uiScale]);

  // Integration-point overlay toggle
  React.useEffect(() => {
    if (tweaks.showIntegrationPoints) {
      document.documentElement.setAttribute('data-ipoints', 'on');
    } else {
      document.documentElement.removeAttribute('data-ipoints');
    }
  }, [tweaks.showIntegrationPoints]);

  // Apply theme
  React.useEffect(() => {
    if (tweaks.theme === 'system') {
      const prefersDark = window.matchMedia('(prefers-color-scheme: dark)').matches;
      document.documentElement.setAttribute('data-theme', prefersDark ? 'dark' : 'light');
    } else {
      document.documentElement.setAttribute('data-theme', tweaks.theme);
    }
  }, [tweaks.theme]);

  // Close popovers on outside click
  React.useEffect(() => {
    const h = (e) => {
      if (!e.target.closest || !e.target.closest('.popover, .tray-icon, .tb-btn, .appid-mark, .appid-chip, .appid-tab')) {
        setTray(null);
        setSwitcherOpen(false);
      }
    };
    document.addEventListener('mousedown', h);
    return () => document.removeEventListener('mousedown', h);
  }, []);

  const pickExt = (id) => {
    setActiveExt(id);
    const e = EXTENSIONS[id];
    if (e && e.subtabs) setActiveSubtab(e.subtabs[0].id);
    setSwitcherOpen(false);
  };

  const ext = activeExt ? EXTENSIONS[activeExt] : null;

  // Per-extension right-hand actions (the app-owned topbar actions, which is exactly the point)
  let actions = null;
  if (activeExt === 'notes') {
    actions = (
      <>
        <button className="tb-btn" title="New note"><Icon name="plus" size={13}/>New</button>
        <button className="tb-btn">Share</button>
      </>
    );
  } else if (activeExt === 'homecare') {
    actions = (
      <>
        <button className="tb-btn"><Icon name="plus" size={13}/>Intake client</button>
        <button className="tb-btn">Today's rota</button>
        <button className="tb-btn" title="Open client card in floating window"
                onClick={() => fw.open({ id: 'patel-card', kind: 'client', title: 'Patel, Anaya', ext: 'homecare', capability: 'homecare.roster.read', x: 360, y: 130, w: 420, h: 460 })}>
          <Icon name="box" size={12}/> Detach
        </button>
      </>
    );
  } else if (activeExt === 'admin') {
    actions = (
      <>
        <button className="tb-btn"><Icon name="package" size={13}/>Install</button>
        <button className="tb-btn">Reconcile</button>
      </>
    );
  }

  return (
    <div style={{ height: '100vh', display: 'flex', flexDirection: 'column', background: 'var(--bg-0)', position: 'relative' }}>
      <Topbar
        ext={ext}
        subtabs={ext ? ext.subtabs : null}
        activeSubtab={activeSubtab}
        onSubtab={setActiveSubtab}
        onHome={() => { setActiveExt(null); setSwitcherOpen(false); }}
        homeActive={activeExt === null}
        identityTreatment={tweaks.identityTreatment}
        actions={actions}
        onOpenSwitcher={(id) => {
          if (typeof id === 'string') { pickExt(id); return; }
          setSwitcherOpen(v => !v);
        }}
        switcherOpen={switcherOpen}
        trayWidth={tweaks.trayWidth}
        trays={{
          bellOpen: tray === 'bell',
          avatarOpen: tray === 'avatar',
          statusOpen: tray === 'status',
          mediaOpen: tray === 'media',
          mediaPlaying,
          unread: 3,
          fallback: tweaks.fallbackAvatar,
        }}
        onTraySet={(t) => setTray(cur => cur === t ? null : t)}
        onToggleMediaPlay={() => setMediaPlaying(p => !p)}
      />

      {/* Comment annotation pin (inline comment on Notes topbar identity) */}
      {tweaks.showAnnotation && ext && ext.id === 'notes' && (
        <div style={{ position: 'absolute', left: 78, top: 7, display: 'flex', gap: 8, alignItems: 'flex-start' }}>
          <span className="anno-pin">1</span>
          <div style={{ marginLeft: 30, marginTop: -2, background: 'var(--bg-2)', border: '1px solid var(--border-strong)',
                        borderRadius: 6, padding: '8px 10px', maxWidth: 360, fontSize: 12, color: 'var(--text-1)',
                        boxShadow: '0 6px 16px rgba(0,0,0,0.3)' }}>
            <div className="mono text-3" style={{ fontSize: 10, marginBottom: 4, textTransform: 'uppercase', letterSpacing: '0.06em' }}>Inline comment · @alex</div>
            The app-identity treatment reads like a browser tab. It shouldn't — this is a persistent frame, not a document container.
          </div>
        </div>
      )}

      {/* Content */}
      <div style={{ flex: 1, minHeight: 0, position: 'relative' }}>
        {activeExt === null && <HomeLauncher onPickExt={pickExt} />}
        {activeExt === 'notes' && <NotesPanel />}
        {activeExt === 'homecare' && <HomecarePanel onOpenFloating={fw.open} />}
        {activeExt === 'admin' && <AdminPanel activeSubtab={activeSubtab} />}
        {activeExt === 'kb' && <KBPanel activeSubtab={activeSubtab} />}
        {activeExt === 'automations' && <AutomationsPanel activeSubtab={activeSubtab} />}
        {activeExt && !['notes','homecare','admin','kb','automations'].includes(activeExt) && (
          <div style={{ padding: 48 }} className="text-2">
            <div className="text-0" style={{ fontSize: 16, fontWeight: 600, marginBottom: 8 }}>{EXTENSIONS[activeExt]?.name || activeExt}</div>
            <div className="mono text-3" style={{ fontSize: 12 }}>Not part of this prototype. The shell renders whatever panel the extension declares.</div>
          </div>
        )}
      </div>

      {/* Floating windows — shell-rendered, extension-bodied */}
      {fw.wins.map(w => (
        <FloatingWindow key={w.id}
          id={w.id} title={w.title} ext={w.ext} capability={w.capability}
          x={w.x} y={w.y} w={w.w} h={w.h} z={w.z}
          onFocus={() => fw.focus(w.id)}
          onMove={(pos) => fw.move(w.id, pos)}
          onClose={() => fw.close(w.id)}>
          {w.kind === 'client' && <FloatingClientCard />}
          {w.kind === 'meds' && <FloatingMedsInspector />}
        </FloatingWindow>
      ))}

      {/* Shell-holes overlay */}
      {tweaks.showShellHoles && <ShellHolesOverlay onClose={() => setTweaks({ showShellHoles: false })} />}

      {/* Narrow viewport demo overlay */}
      {tweaks.showNarrow && <NarrowViewportOverlay onClose={() => setTweaks({ showNarrow: false })} />}

      {/* Integration-point legend */}
      {tweaks.showIntegrationPoints && (
        <div className="ipoint-legend">
          <div className="lg-title">Integration points · overlay on</div>
          <div className="text-2" style={{ fontSize: 11 }}>
            Outlined surfaces are contracted render slots. Package coders implement only the extension-owned ones.
          </div>
          <div className="lg-row" style={{ marginTop: 8 }}>
            <span className="lg-chip" style={{ background: 'var(--accent)' }}></span>
            <span className="mono" style={{ fontSize: 10.5 }}>shell</span>
            <span className="text-3" style={{ fontSize: 11 }}>— provided by Plinth, do not re-render</span>
          </div>
          <div className="lg-row">
            <span className="lg-chip" style={{ background: 'var(--warn)' }}></span>
            <span className="mono" style={{ fontSize: 10.5 }}>extension</span>
            <span className="text-3" style={{ fontSize: 11 }}>— package must declare</span>
          </div>
        </div>
      )}

      {/* Tweaks panel */}
      <TweaksPanel title="Tweaks">
        <TweakSection title="Theme">
          <TweakRadio label="Mode" value={tweaks.theme}
            options={[
              { value: 'dark', label: 'Dark' },
              { value: 'light', label: 'Light' },
              { value: 'system', label: 'System' },
            ]}
            onChange={(v) => setTweaks({ theme: v })} />
        </TweakSection>

        <TweakSection title="App identity treatment" subtitle="How the topbar reads the active extension">
          <TweakRadio label="Style" value={tweaks.identityTreatment}
            options={[
              { value: 'mark', label: 'Mark (frame)' },
              { value: 'chip', label: 'Chip' },
              { value: 'tab', label: 'Tab (counter-example)' },
            ]}
            onChange={(v) => setTweaks({ identityTreatment: v })} />
        </TweakSection>

        <TweakSection title="UI scale" subtitle="Scales every px/font/gap uniformly via CSS zoom — host-level, not app-level">
          <TweakSlider label="Scale" min={75} max={175} step={5}
            value={tweaks.uiScale} unit="%"
            onChange={(v) => setTweaks({ uiScale: v })} />
        </TweakSection>

        <TweakSection title="Tray panel width">
          <TweakSlider label="Width" min={280} max={440} step={20}
            value={tweaks.trayWidth} unit="px"
            onChange={(v) => setTweaks({ trayWidth: v })} />
        </TweakSection>

        <TweakSection title="Demo toggles">
          <TweakToggle label="Show inline comment on Notes topbar"
            value={tweaks.showAnnotation}
            onChange={(v) => setTweaks({ showAnnotation: v })} />
          <TweakToggle label="Avatar tray: chrome_essential fallback"
            value={tweaks.fallbackAvatar}
            onChange={(v) => setTweaks({ fallbackAvatar: v })} />
          <TweakToggle label="Highlight integration points (for package coders)"
            value={tweaks.showIntegrationPoints}
            onChange={(v) => setTweaks({ showIntegrationPoints: v })} />
          <TweakToggle label="Show shell visual holes (easy-to-miss traps)"
            value={tweaks.showShellHoles}
            onChange={(v) => setTweaks({ showShellHoles: v })} />
        </TweakSection>

        <TweakSection title="Floating windows" subtitle="Shell-managed detached panels — extension body inside shell chrome">
          <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 6 }}>
            <TweakButton onClick={() => fw.open({ id: 'patel-card', kind: 'client', title: 'Patel, Anaya', ext: 'homecare', capability: 'homecare.roster.read', x: 360, y: 130, w: 420, h: 460 })}>Client card</TweakButton>
            <TweakButton onClick={() => fw.open({ id: 'patel-meds', kind: 'meds', title: 'Patel — Medications', ext: 'homecare', capability: 'homecare.meds.read', x: 820, y: 180, w: 440, h: 340 })}>Meds inspector</TweakButton>
          </div>
        </TweakSection>

        <TweakSection title="Jump to screen">
          <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 6 }}>
            <TweakButton onClick={() => { setActiveExt(null); setTray(null); }}>Home</TweakButton>
            <TweakButton onClick={() => pickExt('notes')}>Notes</TweakButton>
            <TweakButton onClick={() => pickExt('homecare')}>Homecare</TweakButton>
            <TweakButton onClick={() => pickExt('admin')}>Admin</TweakButton>
            <TweakButton onClick={() => pickExt('kb')}>Knowledge Base (7 subtabs)</TweakButton>
            <TweakButton onClick={() => pickExt('automations')}>Automations (user scripting)</TweakButton>
            <TweakButton onClick={() => { pickExt('admin'); setActiveSubtab('schedules'); }}>Admin · Schedules</TweakButton>
            <TweakButton onClick={() => setTray('status')}>Open status</TweakButton>
            <TweakButton onClick={() => setTray('media')}>Open media</TweakButton>
            <TweakButton onClick={() => setTray('bell')}>Open bell</TweakButton>
            <TweakButton onClick={() => setTray('avatar')}>Open avatar</TweakButton>
          </div>
        </TweakSection>

        <TweakSection title="Narrow viewport" subtitle="Mobile treatment: bottom-sheet popovers, scroll-within sub-tabs, collapsed tray">
          <TweakButton onClick={() => setTweaks({ showNarrow: true })}>
            <span style={{ display: 'inline-flex', alignItems: 'center', gap: 6 }}><Icon name="smartphone" size={12}/>Show narrow-viewport sketch</span>
          </TweakButton>
        </TweakSection>
      </TweaksPanel>
    </div>
  );
}

ReactDOM.createRoot(document.getElementById('app')).render(<App />);
