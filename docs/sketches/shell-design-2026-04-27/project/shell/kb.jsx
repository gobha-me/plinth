// Knowledge Base — stress case for the 7-subtab topbar. Simple placeholder content
// per subtab; the real story is the topbar.
const KBPanel = ({ activeSubtab }) => {
  const rows = {
    search: [
      { q: 'what hours is jordan on this week?',       src: 'homecare · schedule',  time: '2m ago' },
      { q: 'how do I update a medication dose?',       src: 'docs · homecare',       time: '18m ago' },
      { q: 'tuesday handover draft',                   src: 'notes',                 time: 'yesterday' },
    ],
    chat: [
      { who: '@alex',  msg: 'summarise the patel file and flag anything overdue',           time: '07:41' },
      { who: 'kb',     msg: 'Two visits logged this week. Meds review on 14 Oct is overdue by 3 days.', time: '07:41' },
    ],
    sources: [
      { name: 'docs/homecare',         kind: 'folder', items: 42, status: 'indexed' },
      { name: 'notes/handovers',       kind: 'folder', items: 18, status: 'indexed' },
      { name: 'homecare.visits',       kind: 'schema', items: 312, status: 'live' },
      { name: 'homecare.meds',         kind: 'schema', items: 48, status: 'live' },
    ],
    sync: [
      { at: '08:12:04', msg: 'notes/handovers/tuesday.md · chunked · 4 vectors',  lv: 'ok' },
      { at: '08:12:01', msg: 'homecare.visits · 12 new rows · backfill',          lv: 'ok' },
      { at: '08:11:58', msg: 'docs/homecare/meds.md · skipped · unchanged',       lv: 'muted' },
      { at: '08:11:52', msg: 'pull from notes extension',                         lv: 'info' },
    ],
    health:   { queue: 0, lag: '34ms', vecstore: '1.4 GB / 8 GB', model: 'mini-3b @ local', uptime: '11d 04h' },
    analytics:{ queries24h: 42, avgLat: '480ms', topQ: 'jordan schedule', fallbacks: 1 },
    settings: null,
  };

  return (
    <div style={{ height: '100%', overflowY: 'auto', padding: '20px 24px' }}>
      <div className="flex items-baseline gap-3 mb-4">
        <div className="text-0" style={{ fontSize: 17, fontWeight: 600 }}>Knowledge Base</div>
        <div className="mono text-3" style={{ fontSize: 11 }}>kb@1.1.0 · rag extension</div>
        <div className="ml-auto flex items-center gap-2">
          <span className="pill ok"><span className="dot" style={{ background: 'var(--success)' }}/>live</span>
          <span className="mono text-3" style={{ fontSize: 11 }}>312 docs · 4,812 chunks</span>
        </div>
      </div>

      {activeSubtab === 'search' && (
        <div>
          <div style={{ position: 'relative', marginBottom: 14 }}>
            <input className="w-full px-3 py-2 mono" style={{ background: 'var(--bg-1)', border: '1px solid var(--border)', borderRadius: 5,
                     color: 'var(--text-0)', fontSize: 13 }}
              defaultValue="what hours is jordan on this week?" />
          </div>
          <div className="surface-1" style={{ borderRadius: 6 }}>
            {rows.search.map((r, i) => (
              <div key={i} className="hover-bg flex items-center gap-3 px-3 py-2" style={{ borderBottom: '1px solid var(--border-soft)', cursor: 'pointer' }}>
                <Icon name="search" size={14} className="text-3"/>
                <span className="text-1 flex-1" style={{ fontSize: 13 }}>{r.q}</span>
                <span className="mono text-3" style={{ fontSize: 11 }}>{r.src}</span>
                <span className="mono text-3" style={{ fontSize: 11 }}>{r.time}</span>
              </div>
            ))}
          </div>
        </div>
      )}

      {activeSubtab === 'chat' && (
        <div className="surface-1" style={{ borderRadius: 6, maxWidth: 760 }}>
          {rows.chat.map((m, i) => (
            <div key={i} className="flex gap-3 px-4 py-3" style={{ borderBottom: '1px solid var(--border-soft)' }}>
              <span className="mono" style={{ color: m.who === 'kb' ? 'var(--accent)' : 'var(--text-0)', fontSize: 11.5, width: 60 }}>{m.who}</span>
              <div className="flex-1">
                <div className="text-1" style={{ fontSize: 13, lineHeight: 1.55 }}>{m.msg}</div>
                <div className="mono text-3" style={{ fontSize: 10.5, marginTop: 3 }}>{m.time}</div>
              </div>
            </div>
          ))}
        </div>
      )}

      {activeSubtab === 'sources' && (
        <div className="surface-1" style={{ borderRadius: 6 }}>
          <table className="tbl">
            <thead><tr><th>Source</th><th>Kind</th><th>Items</th><th>Status</th></tr></thead>
            <tbody>
              {rows.sources.map((s, i) => (
                <tr key={i}><td className="mono">{s.name}</td><td className="text-2">{s.kind}</td><td className="mono text-2">{s.items}</td><td><span className="pill ok">{s.status}</span></td></tr>
              ))}
            </tbody>
          </table>
        </div>
      )}

      {activeSubtab === 'sync' && (
        <div className="surface-1" style={{ borderRadius: 6 }}>
          {rows.sync.map((l, i) => (
            <div key={i} className="flex items-center gap-3 px-3 py-2" style={{ borderBottom: '1px solid var(--border-soft)', fontSize: 12.5 }}>
              <span className="mono text-3" style={{ fontSize: 11, width: 72 }}>{l.at}</span>
              <span className={l.lv === 'ok' ? 'text-1' : 'text-3'} style={{ flex: 1 }}>{l.msg}</span>
            </div>
          ))}
        </div>
      )}

      {activeSubtab === 'health' && (
        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(180px, 1fr))', gap: 12, maxWidth: 720 }}>
          {Object.entries(rows.health).map(([k, v]) => (
            <div key={k} className="surface-1" style={{ borderRadius: 6, padding: 12 }}>
              <div className="mono text-3" style={{ fontSize: 10.5, textTransform: 'uppercase', letterSpacing: '0.06em' }}>{k}</div>
              <div className="text-0 mono" style={{ fontSize: 16, fontWeight: 600, marginTop: 4 }}>{v}</div>
            </div>
          ))}
        </div>
      )}

      {activeSubtab === 'analytics' && (
        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(180px, 1fr))', gap: 12, maxWidth: 720 }}>
          {Object.entries(rows.analytics).map(([k, v]) => (
            <div key={k} className="surface-1" style={{ borderRadius: 6, padding: 12 }}>
              <div className="mono text-3" style={{ fontSize: 10.5, textTransform: 'uppercase', letterSpacing: '0.06em' }}>{k}</div>
              <div className="text-0 mono" style={{ fontSize: 16, fontWeight: 600, marginTop: 4 }}>{v}</div>
            </div>
          ))}
        </div>
      )}

      {activeSubtab === 'settings' && (
        <div className="surface-1" style={{ borderRadius: 6, padding: 20, maxWidth: 560 }}>
          <div className="text-0" style={{ fontWeight: 600, marginBottom: 10 }}>Model</div>
          <div className="mono text-2" style={{ fontSize: 12 }}>mini-3b · local · 4096 ctx</div>
          <div className="text-0" style={{ fontWeight: 600, marginTop: 18, marginBottom: 10 }}>Capability requirements</div>
          <div className="mono text-2" style={{ fontSize: 12, lineHeight: 1.8 }}>
            notes.index.read<br/>
            homecare.index.read<br/>
            plinth.vector.write
          </div>
        </div>
      )}
    </div>
  );
};
Object.assign(window, { KBPanel });
