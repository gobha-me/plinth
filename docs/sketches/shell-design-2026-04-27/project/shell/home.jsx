// Home launcher — no active app. RBAC-gated icon grid + recent items.
const HomeLauncher = ({ onPickExt }) => {
  const exts = [
    { id: 'notes', name: 'Notes', desc: 'Markdown notebooks, backlinks, shared folders.', version: '1.4.0', cap: 'notes.open', glyph: 'N' },
    { id: 'homecare', name: 'Homecare', desc: 'Client roster, care schedules, visit logs.', version: '1.3.1', cap: 'homecare.open', glyph: 'H', recent: true },
    { id: 'files', name: 'Files', desc: 'Storage browser with extension-namespaced trees.', version: '0.9.2', cap: 'files.open', glyph: 'F' },
    { id: 'chat', name: 'Chat', desc: 'Per-group realtime pubsub rooms.', version: '0.8.0', cap: 'chat.open', glyph: 'C' },
    { id: 'admin', name: 'Admin', desc: 'Packages, RBAC, capability audit, group membership.', version: '0.6.4', cap: 'admin.open', glyph: 'A', gated: 'kernel.admin' },
  ];

  const recents = [
    { kind: 'note', title: 'Care binder / intake', ext: 'notes', at: '08:14', glyph: 'N' },
    { kind: 'client', title: 'Mrs. Patel — weekly schedule', ext: 'homecare', at: '07:52', glyph: 'H' },
    { kind: 'note', title: 'Tuesday handover', ext: 'notes', at: 'Mon', glyph: 'N' },
    { kind: 'package', title: 'homecare 1.3.2 available', ext: 'admin', at: 'Mon', glyph: 'A' },
  ];

  return (
    <div className="flex" style={{ height: '100%' }}>
      <div style={{ flex: 1, minWidth: 0, padding: '32px 40px', overflowY: 'auto' }}
           data-ipoint="shell.home.launcher" data-ipoint-layer="shell">
        {/* Header strip */}
        <div className="flex items-baseline gap-3 mb-1">
          <div className="text-0" style={{ fontSize: 20, fontWeight: 600, letterSpacing: '-0.01em' }}>Home</div>
          <div className="text-3 mono" style={{ fontSize: 11 }}>plinth/shell@0.6.6</div>
        </div>
        <div className="text-2" style={{ fontSize: 13, marginBottom: 28, maxWidth: 560 }}>
          Five extensions installed for <span className="mono text-1">@alex</span>. Pick one, or use <span className="kbd">⌘</span> <span className="kbd">K</span> to jump by capability.
        </div>

        {/* Extension grid */}
        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(224px, 1fr))', gap: 12, marginBottom: 36 }}>
          {exts.map(e => (
            <div key={e.id} className="tile" onClick={() => onPickExt(e.id)}>
              <div className="ti-head">
                <span className="ti-glyph" style={{ background: EXT_COLORS[e.id] }}>{e.glyph}</span>
                <div style={{ flex: 1, minWidth: 0 }}>
                  <div className="ti-name">{e.name}</div>
                  <div className="mono text-3" style={{ fontSize: 11 }}>{e.id}@{e.version}</div>
                </div>
                {e.recent && <span className="pill info">active 4m ago</span>}
              </div>
              <div className="ti-desc">{e.desc}</div>
              <div className="ti-foot">
                <span className="mono">{e.cap}</span>
                {e.gated && <span style={{ display: 'inline-flex', gap: 4, alignItems: 'center' }}>
                  <Icon name="lock" size={11} /> <span className="mono">{e.gated}</span>
                </span>}
              </div>
            </div>
          ))}

          {/* Install tile */}
          <div className="tile" style={{ borderStyle: 'dashed', background: 'transparent' }}>
            <div className="ti-head">
              <span className="ti-glyph" style={{ background: 'var(--bg-3)', color: 'var(--text-2)' }}>
                <Icon name="plus" size={14} />
              </span>
              <div style={{ flex: 1 }}>
                <div className="ti-name text-2">Install extension</div>
                <div className="mono text-3" style={{ fontSize: 11 }}>admin → packages</div>
              </div>
            </div>
            <div className="ti-desc">Add more capability to this Plinth. Extensions go through the standard package lifecycle.</div>
          </div>
        </div>
      </div>

      {/* Right rail: recent items */}
      <div style={{ width: 300, borderLeft: '1px solid var(--border)', background: 'var(--bg-1)', overflowY: 'auto' }}
           data-ipoint="shell.home.recent" data-ipoint-layer="shell">
        <div className="px-4 py-3 hair">
          <div className="text-3 mono" style={{ fontSize: 11, letterSpacing: '0.08em', textTransform: 'uppercase' }}>Recent</div>
        </div>
        {recents.map((r, i) => (
          <div key={i} className="hover-bg px-4 py-3 flex items-start gap-3" style={{ cursor: 'pointer', borderBottom: '1px solid var(--border-soft)' }}>
            <span style={{ width: 22, height: 22, borderRadius: 4, background: EXT_COLORS[r.ext], color: 'white',
                            display: 'inline-flex', alignItems: 'center', justifyContent: 'center',
                            fontFamily: 'JetBrains Mono', fontSize: 10, fontWeight: 700, flexShrink: 0 }}>{r.glyph}</span>
            <div className="min-w-0 flex-1">
              <div className="text-0" style={{ fontSize: 13, fontWeight: 500 }}>{r.title}</div>
              <div className="text-3 mono" style={{ fontSize: 10.5, marginTop: 2 }}>{r.ext} · {r.at}</div>
            </div>
          </div>
        ))}
        <div className="px-4 py-3 text-3" style={{ fontSize: 11 }}>
          From <span className="mono">plinth.events</span> — the same pub/sub used by every extension.
        </div>
      </div>
    </div>
  );
};
Object.assign(window, { HomeLauncher });
