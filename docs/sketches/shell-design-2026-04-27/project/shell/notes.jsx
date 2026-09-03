// Notes extension — main panel (folder tree + editor) + sidebar (backlinks).
const NotesPanel = ({ annotate }) => {
  const [selected, setSelected] = React.useState('care-binder');
  const tree = [
    { id: 'personal', kind: 'folder', name: 'Personal', children: [
      { id: 'daily', kind: 'note', name: 'Daily log' },
      { id: 'reading', kind: 'note', name: 'Reading — 2026' },
    ]},
    { id: 'work', kind: 'folder', name: 'Work', open: true, children: [
      { id: 'care-binder', kind: 'note', name: 'Care binder / intake', active: true },
      { id: 'handover', kind: 'note', name: 'Tuesday handover' },
      { id: 'kowalski', kind: 'note', name: 'Kowalski — medication notes' },
    ]},
    { id: 'shared', kind: 'folder', name: 'Shared with partner', children: [
      { id: 'weekly', kind: 'note', name: 'Weekly roster' },
    ]},
  ];

  const Row = ({ item, depth = 0 }) => {
    const pad = 10 + depth * 14;
    if (item.kind === 'folder') {
      const open = item.open;
      return (
        <>
          <div className="hover-bg flex items-center gap-2"
               style={{ padding: `4px ${pad}px 4px ${pad}px`, cursor: 'pointer', color: 'var(--text-1)', fontSize: 13 }}>
            <Icon name={open ? 'chev-down' : 'chev-right'} size={11} style={{ color: 'var(--text-3)' }} />
            <Icon name={open ? 'folder-open' : 'folder'} size={14} style={{ color: 'var(--text-3)' }} />
            <span>{item.name}</span>
          </div>
          {open && item.children && item.children.map(c => <Row key={c.id} item={c} depth={depth + 1} />)}
        </>
      );
    }
    const active = item.id === selected;
    return (
      <div className={active ? 'bg-2' : 'hover-bg'}
           onClick={() => setSelected(item.id)}
           style={{ padding: `4px ${pad + 20}px 4px ${pad + 20}px`, cursor: 'pointer',
                    color: active ? 'var(--text-0)' : 'var(--text-1)', fontSize: 13,
                    borderLeft: active ? '2px solid var(--accent)' : '2px solid transparent' }}>
        {item.name}
      </div>
    );
  };

  return (
    <div className="flex" style={{ height: '100%' }}>
      {/* Folder tree (extension-internal, not shell) */}
      <div style={{ width: 240, background: 'var(--bg-1)', borderRight: '1px solid var(--border)', display: 'flex', flexDirection: 'column' }}
           data-ipoint="ext.notes.primaryPane">
        <div className="flex items-center justify-between px-3 py-2 hair">
          <div className="text-3 mono" style={{ fontSize: 11, letterSpacing: '0.08em', textTransform: 'uppercase' }}>ext_notes</div>
          <Icon name="plus" size={13} style={{ color: 'var(--text-3)', cursor: 'pointer' }} />
        </div>
        <div style={{ flex: 1, overflowY: 'auto', paddingTop: 4 }}>
          {tree.map(t => <Row key={t.id} item={t} />)}
        </div>
        <div className="hair-t px-3 py-2 text-3 mono" style={{ fontSize: 10.5 }}>
          storage://notes/work/care-binder.md
        </div>
      </div>

      {/* Editor */}
      <div style={{ flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column' }}
           data-ipoint="ext.notes.workspace">
        <div className="flex items-center gap-3 px-5 py-2 hair">
          <div className="mono text-3" style={{ fontSize: 11 }}>work / care-binder.md</div>
          <span className="pill muted"><span className="dot" style={{ background: 'var(--warn)' }}/> unsaved</span>
          <div className="ml-auto flex items-center gap-3 text-3" style={{ fontSize: 11 }}>
            <span>3 backlinks</span>
            <span>·</span>
            <span>shared with 1</span>
          </div>
        </div>

        <div style={{ flex: 1, overflowY: 'auto', padding: '28px 48px 80px' }}>
          <div className="editor-body" style={{ maxWidth: 720, margin: '0 auto' }}>
            <h1>Care binder / intake</h1>
            <p className="text-2" style={{ fontSize: 13 }}>Draft — for review with Jordan before Tuesday.</p>

            <h2>Clients this week</h2>
            <p>
              <a href="#">Mrs. Patel</a> — Tuesday 14:00 and Thursday 10:00. Medication reminder moved to <code>08:30</code>.
              See also <a href="#">Kowalski — medication notes</a>.
            </p>
            <p>
              <a href="#">Mr. Kowalski</a> — Tuesday 14:30 visit (see <span className="pill warn">scheduling conflict</span>). Insulin schedule attached in Files.
            </p>

            <h2>Handover checklist</h2>
            <p>The intake form lives at <code>storage://notes/shared/weekly.md</code> and is re-shared weekly via <code>cap.call("sharing.grant")</code>.<span className="caret"/></p>
          </div>
        </div>

        {/* Status bar */}
        <div className="flex items-center gap-4 px-5 py-1 hair-t mono text-3" style={{ fontSize: 11, background: 'var(--bg-1)' }}
             data-ipoint="ext.notes.statusBar">
          <span>md</span>
          <span>ln 14 · col 42</span>
          <span>utf-8</span>
          <span className="ml-auto">autosaved 14s ago · ext_notes.autosave</span>
        </div>
      </div>

      {/* Sidebar panel: backlinks (a shell-managed sidebar panel) */}
      <div style={{ width: 280, background: 'var(--bg-1)', borderLeft: '1px solid var(--border)', display: 'flex', flexDirection: 'column' }}
           data-ipoint="shell.rightPanel:notes.backlinks" data-ipoint-layer="shell">
        <div className="flex items-center justify-between px-3 py-2 hair">
          <div className="text-0" style={{ fontSize: 12.5, fontWeight: 600 }}>Backlinks</div>
          <div className="mono text-3" style={{ fontSize: 10 }}>notes:backlinks</div>
        </div>
        <div style={{ flex: 1, overflowY: 'auto' }}>
          {[
            { t: 'Tuesday handover', e: '…Tuesday 14:00 → see [[care-binder]]…' },
            { t: 'Weekly roster',    e: '…share from [[care-binder]] on Mondays…' },
            { t: 'Kowalski — medication notes', e: '…backlink to [[care-binder]]…' },
          ].map((b, i) => (
            <div key={i} className="hover-bg px-3 py-3" style={{ borderBottom: '1px solid var(--border-soft)', cursor: 'pointer' }}>
              <div className="text-0" style={{ fontSize: 12.5, fontWeight: 500, marginBottom: 3 }}>{b.t}</div>
              <div className="text-2" style={{ fontSize: 11.5, lineHeight: 1.5 }}>{b.e}</div>
            </div>
          ))}
        </div>
        <div className="hair-t px-3 py-2 text-3 mono" style={{ fontSize: 10.5 }}>
          subscribes: ext_notes.link_index
        </div>
      </div>
    </div>
  );
};
Object.assign(window, { NotesPanel });
