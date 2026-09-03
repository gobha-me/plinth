// Homecare — client roster table w/ care-schedule column.
const HomecarePanel = () => {
  const rows = [
    { id: 'C-0014', name: 'Patel, Anaya', alias: 'Mrs. Patel', primary: 'Alex', group: 'Example family', visits: 'Tue 14:00 · Thu 10:00', conflict: true, meds: 4, last: '08:14 today', status: 'active' },
    { id: 'C-0012', name: 'Kowalski, Tomasz', alias: 'Mr. Kowalski', primary: 'Jordan', group: 'Example family', visits: 'Tue 14:30 · Fri 09:00', conflict: true, meds: 7, last: 'yesterday', status: 'active' },
    { id: 'C-0019', name: 'Obi-Okafor, Maryam', alias: '', primary: 'Alex', group: 'Community rota', visits: 'Wed 11:00', conflict: false, meds: 2, last: 'Mon 09:42', status: 'active' },
    { id: 'C-0009', name: 'Alderson, Graham', alias: '', primary: 'Jordan', group: 'Example family', visits: 'Mon 16:00 · Fri 16:00', conflict: false, meds: 3, last: 'Fri 16:48', status: 'active' },
    { id: 'C-0022', name: 'Nwosu, Chinaza', alias: '', primary: 'Alex', group: 'Community rota', visits: 'Thu 14:00', conflict: false, meds: 1, last: 'last week', status: 'paused' },
    { id: 'C-0031', name: 'Hemraj, Deepa', alias: '', primary: 'Alex', group: 'Example family', visits: '—', conflict: false, meds: 0, last: 'intake pending', status: 'intake' },
  ];

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%' }}
         data-ipoint="ext.homecare.workspace">
      {/* Filter bar */}
      <div className="flex items-center gap-2 px-5 py-2 hair" style={{ background: 'var(--bg-1)' }}
           data-ipoint="ext.homecare.filterBar">
        <div className="flex items-center gap-2 px-2 bg-0" style={{ border: '1px solid var(--border)', borderRadius: 5, height: 28, minWidth: 260 }}>
          <Icon name="search" size={13} style={{ color: 'var(--text-3)' }} />
          <input placeholder="Filter clients… (name, group, schedule)" className="mono"
                 style={{ background: 'transparent', border: 'none', outline: 'none', color: 'var(--text-1)', fontSize: 12, width: '100%' }} />
          <span className="kbd">/</span>
        </div>
        <button className="tb-btn">Primary <span className="mono text-3 ml-1">: any</span><Icon name="chev-down" size={11} style={{ color: 'var(--text-3)' }}/></button>
        <button className="tb-btn">Group <span className="mono text-3 ml-1">: any</span><Icon name="chev-down" size={11} style={{ color: 'var(--text-3)' }}/></button>
        <button className="tb-btn">Status <span className="mono text-3 ml-1">: active</span><Icon name="chev-down" size={11} style={{ color: 'var(--text-3)' }}/></button>
        <div className="ml-auto flex items-center gap-2 text-3 mono" style={{ fontSize: 11 }}>
          <span>{rows.length} clients · 2 conflicts this week</span>
        </div>
      </div>

      {/* Table */}
      <div style={{ flex: 1, overflow: 'auto' }}>
        <table className="tbl">
          <thead>
            <tr>
              <th style={{ width: 84 }}>ID</th>
              <th>Client</th>
              <th>Primary</th>
              <th>Group</th>
              <th>Care schedule</th>
              <th style={{ width: 70, textAlign: 'right' }}>Meds</th>
              <th>Last contact</th>
              <th style={{ width: 80 }}>Status</th>
              <th style={{ width: 36 }}></th>
            </tr>
          </thead>
          <tbody>
            {rows.map((r, i) => (
              <tr key={r.id} style={{ cursor: 'pointer' }}>
                <td className="mono text-3" style={{ fontSize: 12 }}>{r.id}</td>
                <td>
                  <div className="text-0" style={{ fontWeight: 500 }}>{r.name}</div>
                  {r.alias && <div className="sub">aka {r.alias}</div>}
                </td>
                <td>{r.primary}</td>
                <td className="text-2">{r.group}</td>
                <td>
                  <div className="flex items-center gap-2">
                    <Icon name="calendar" size={13} style={{ color: 'var(--text-3)' }} />
                    <span className="mono text-1" style={{ fontSize: 12 }}>{r.visits}</span>
                    {r.conflict && <span className="pill warn">conflict</span>}
                  </div>
                </td>
                <td className="mono text-1" style={{ textAlign: 'right', fontSize: 12 }}>{r.meds}</td>
                <td className="text-2">{r.last}</td>
                <td>
                  {r.status === 'active' && <span className="pill ok"><span className="dot" style={{ background: 'var(--success)' }}/>active</span>}
                  {r.status === 'paused' && <span className="pill muted"><span className="dot" style={{ background: 'var(--text-3)' }}/>paused</span>}
                  {r.status === 'intake' && <span className="pill info"><span className="dot" style={{ background: 'var(--accent)' }}/>intake</span>}
                </td>
                <td><Icon name="dots" size={14} style={{ color: 'var(--text-3)' }}/></td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>

      {/* Footer */}
      <div className="flex items-center justify-between px-5 py-2 hair-t mono text-3" style={{ fontSize: 11, background: 'var(--bg-1)' }}
           data-ipoint="ext.homecare.statusBar">
        <span>ext_homecare.clients · schema v7</span>
        <span>cap.call("homecare.roster.list") — 34ms</span>
      </div>
    </div>
  );
};
Object.assign(window, { HomecarePanel });
