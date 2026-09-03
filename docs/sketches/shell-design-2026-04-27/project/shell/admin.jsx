// Admin — installed extensions list + capability audit sidebar.
const AdminPanel = ({ activeSubtab = 'packages' }) => {
  if (activeSubtab === 'schedules') return <AdminSchedules />;
  const [selected, setSelected] = React.useState('homecare');
  const pkgs = [
    { id: 'shell', name: 'Plinth shell', version: '0.6.6', provenance: 'bundled', schema: 'ext_shell', rules: 0, status: 'active' },
    { id: 'admin', name: 'Admin', version: '0.6.4', provenance: 'bundled', schema: 'ext_admin', rules: 5, status: 'active' },
    { id: 'notes', name: 'Notes', version: '1.4.0', provenance: 'registry', schema: 'ext_notes', rules: 3, status: 'active' },
    { id: 'homecare', name: 'Homecare', version: '1.3.1', provenance: 'registry', schema: 'ext_homecare', rules: 6, status: 'active', upgrade: '1.3.2' },
    { id: 'files', name: 'Files', version: '0.9.2', provenance: 'bundled', schema: 'ext_files', rules: 2, status: 'active' },
    { id: 'chat', name: 'Chat', version: '0.8.0', provenance: 'registry', schema: 'ext_chat', rules: 4, status: 'disabled' },
  ];
  const rulesByExt = {
    homecare: [
      { rule: 'homecare.roster.read',    groups: ['caregiver', 'admin'],  kind: 'read' },
      { rule: 'homecare.roster.write',   groups: ['admin'],               kind: 'write' },
      { rule: 'homecare.schedule.read',  groups: ['caregiver', 'admin'],  kind: 'read' },
      { rule: 'homecare.schedule.write', groups: ['admin'],               kind: 'write' },
      { rule: 'homecare.meds.read',      groups: ['caregiver', 'admin'],  kind: 'read' },
      { rule: 'homecare.visit.log',      groups: ['caregiver', 'admin'],  kind: 'write' },
    ],
    notes: [
      { rule: 'notes.read',  groups: ['authenticated'], kind: 'read' },
      { rule: 'notes.write', groups: ['authenticated'], kind: 'write' },
      { rule: 'notes.share.grant', groups: ['authenticated'], kind: 'write' },
    ],
    shell:   [],
    admin:   [
      { rule: 'kernel.admin',     groups: ['admin'], kind: 'admin' },
      { rule: 'packages.manage',  groups: ['admin'], kind: 'admin' },
      { rule: 'groups.manage',    groups: ['admin'], kind: 'admin' },
      { rule: 'rbac.manage',      groups: ['admin'], kind: 'admin' },
      { rule: 'audit.read',       groups: ['admin'], kind: 'read' },
    ],
    files: [
      { rule: 'files.read',  groups: ['authenticated'], kind: 'read' },
      { rule: 'files.write', groups: ['authenticated'], kind: 'write' },
    ],
    chat: [
      { rule: 'chat.room.read',  groups: ['authenticated'], kind: 'read' },
      { rule: 'chat.room.write', groups: ['authenticated'], kind: 'write' },
      { rule: 'chat.room.create',groups: ['admin'],         kind: 'write' },
      { rule: 'chat.room.admin', groups: ['admin'],         kind: 'admin' },
    ],
  };

  const currentRules = rulesByExt[selected] || [];

  return (
    <div className="flex" style={{ height: '100%' }}>
      {/* Main: installed extensions */}
      <div style={{ flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column' }}
           data-ipoint="ext.admin.workspace">
        <div className="flex items-center gap-3 px-5 py-2 hair" style={{ background: 'var(--bg-1)' }}>
          <div className="text-3 mono" style={{ fontSize: 11, letterSpacing: '0.08em', textTransform: 'uppercase' }}>plinth.packages</div>
          <span className="pill muted">{pkgs.length} installed</span>
          <span className="pill warn">1 upgrade</span>
          <div className="ml-auto flex items-center gap-2">
            <button className="tb-btn"><Icon name="plus" size={12}/> Install package</button>
          </div>
        </div>

        <div style={{ flex: 1, overflow: 'auto' }}>
          <table className="tbl">
            <thead>
              <tr>
                <th style={{ width: 28 }}></th>
                <th>Package</th>
                <th>Version</th>
                <th>Schema</th>
                <th>Provenance</th>
                <th style={{ textAlign: 'right' }}>Rules</th>
                <th>Status</th>
                <th style={{ width: 90 }}></th>
              </tr>
            </thead>
            <tbody>
              {pkgs.map(p => (
                <tr key={p.id} onClick={() => setSelected(p.id)}
                    style={{ cursor: 'pointer', background: selected === p.id ? 'var(--bg-2)' : undefined }}>
                  <td><span style={{ width: 16, height: 16, borderRadius: 3, background: EXT_COLORS[p.id] || 'var(--text-3)', display: 'inline-block' }} /></td>
                  <td>
                    <div className="text-0" style={{ fontWeight: 500 }}>{p.name}</div>
                    <div className="sub mono">{p.id}</div>
                  </td>
                  <td className="mono">
                    {p.version}
                    {p.upgrade && <div className="sub" style={{ color: 'var(--warn)' }}>→ {p.upgrade}</div>}
                  </td>
                  <td className="mono text-2" style={{ fontSize: 12 }}>{p.schema}</td>
                  <td>
                    <span className={"pill " + (p.provenance === 'bundled' ? 'info' : 'muted')}>{p.provenance}</span>
                  </td>
                  <td className="mono text-1" style={{ textAlign: 'right' }}>{p.rules}</td>
                  <td>
                    {p.status === 'active'
                      ? <span className="pill ok"><span className="dot" style={{ background: 'var(--success)' }}/>active</span>
                      : <span className="pill muted"><span className="dot" style={{ background: 'var(--text-3)' }}/>disabled</span>}
                  </td>
                  <td><span className="tb-btn text-2" style={{ fontSize: 12 }}>Manage</span></td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>

        <div className="flex items-center justify-between px-5 py-2 hair-t mono text-3" style={{ fontSize: 11, background: 'var(--bg-1)' }}>
          <span>/api/packages · cap: packages.manage</span>
          <span>last reconcile 2m ago</span>
        </div>
      </div>

      {/* Sidebar: capability audit */}
      <div style={{ width: 380, borderLeft: '1px solid var(--border)', background: 'var(--bg-1)', display: 'flex', flexDirection: 'column' }}
           data-ipoint="shell.rightPanel:admin.capabilityAudit" data-ipoint-layer="shell">
        <div className="px-4 py-3 hair">
          <div className="flex items-center gap-2 mb-1">
            <Icon name="shield" size={14} style={{ color: 'var(--text-3)' }} />
            <div className="text-0" style={{ fontWeight: 600, fontSize: 13 }}>Capability audit</div>
          </div>
          <div className="mono text-3" style={{ fontSize: 11 }}>admin:capability-audit</div>
        </div>

        <div className="px-4 py-3 hair">
          <div className="text-3" style={{ fontSize: 11, letterSpacing: '0.06em', textTransform: 'uppercase', marginBottom: 6 }}>Scope</div>
          <div className="flex items-center gap-2">
            <span style={{ width: 14, height: 14, borderRadius: 3, background: EXT_COLORS[selected] }} />
            <span className="text-0 mono" style={{ fontSize: 13, fontWeight: 500 }}>{selected}</span>
            <span className="text-3 mono ml-auto" style={{ fontSize: 11 }}>{currentRules.length} rules</span>
          </div>
        </div>

        <div style={{ flex: 1, overflowY: 'auto' }}>
          {currentRules.length === 0 && (
            <div className="px-4 py-6 text-3 mono" style={{ fontSize: 12 }}>
              No RBAC rules declared. This extension consumes kernel capabilities but provides none.
            </div>
          )}
          {currentRules.map((r, i) => (
            <div key={i} className="px-4 py-3" style={{ borderBottom: '1px solid var(--border-soft)' }}>
              <div className="flex items-center justify-between mb-1">
                <span className="mono text-0" style={{ fontSize: 12.5 }}>{r.rule}</span>
                <span className={"pill " + (r.kind === 'read' ? 'muted' : r.kind === 'write' ? 'info' : 'warn')}>{r.kind}</span>
              </div>
              <div className="text-2" style={{ fontSize: 12 }}>Granted to:</div>
              <div className="flex flex-wrap gap-1 mt-1">
                {r.groups.map(g => (
                  <span key={g} className="mono pill muted">{g}</span>
                ))}
              </div>
            </div>
          ))}
        </div>
        <div className="hair-t px-4 py-2 mono text-3" style={{ fontSize: 10.5 }}>
          reads: plinth.rbac_rules · plinth.rbac_grants
        </div>
      </div>
    </div>
  );
};
Object.assign(window, { AdminPanel });
