// Admin · Schedules — cron-like tasks declared by packages + backups + user schedules.
//
// Design stance (§7 of the shell design doc):
//   1. Schedules come in three layers. Packages declare the shape (cron strings,
//      what handler to call). Operators tune cadence and enable/disable. Users
//      own their personal recurring items (reminders, scheduled sends).
//   2. All three layers share one visual vocabulary — the row, the cadence chip,
//      the next-fire ISO. Don't fork UIs per layer.
//   3. Creating a schedule is the hard part. The right-rail "new schedule"
//      composer is always present. It's a form, not a modal — composability over
//      ceremony. cap-gated inputs greyed with the cap name.

const AdminSchedules = () => {
  const [scope, setScope] = React.useState('system'); // system | backups | userPolicy
  const [selected, setSelected] = React.useState(null);
  const [composer, setComposer] = React.useState({
    name: '',
    cadence: 'every day at 08:30',
    handler: '',
    scope: 'system',
    enabled: true,
  });

  const systemJobs = [
    { id: 'pkg:homecare.roster.compact',   pkg: 'homecare', name: 'Roster index compaction',  cadence: '0 3 * * *',      human: 'daily at 03:00', next: '2026-04-24 03:00:00', last: 'Apr 23 03:00:14', lastDur: '2.4s', lastStatus: 'ok', cap: 'homecare.admin',     source: 'package' },
    { id: 'pkg:homecare.meds.reminder',    pkg: 'homecare', name: 'Medication reminders fan-out', cadence: '*/15 * * * *', human: 'every 15 min',   next: '2026-04-23 14:15:00', last: 'Apr 23 14:00:00', lastDur: '340ms', lastStatus: 'ok', cap: 'homecare.meds.read', source: 'package' },
    { id: 'pkg:notes.link_index.rebuild',  pkg: 'notes',    name: 'Backlink index rebuild',   cadence: '0 */6 * * *',    human: 'every 6 hrs',    next: '2026-04-23 18:00:00', last: 'Apr 23 12:00:02', lastDur: '1.1s', lastStatus: 'ok', cap: 'notes.admin',        source: 'package' },
    { id: 'pkg:admin.reconcile',           pkg: 'admin',    name: 'Reconcile packages',       cadence: '*/5 * * * *',    human: 'every 5 min',    next: '2026-04-23 14:05:00', last: 'Apr 23 14:00:00', lastDur: '88ms',  lastStatus: 'ok', cap: 'packages.manage',    source: 'package' },
    { id: 'pkg:shell.session.gc',          pkg: 'shell',    name: 'Session GC',               cadence: '0 4 * * *',      human: 'daily at 04:00', next: '2026-04-24 04:00:00', last: 'Apr 23 04:00:08', lastDur: '620ms', lastStatus: 'warn', cap: 'kernel.admin',   source: 'package', note: 'GC ran 2.1× median duration — fine, logged.' },
    { id: 'pkg:chat.transcript.trim',      pkg: 'chat',     name: 'Transcript trim',          cadence: '0 2 * * 0',      human: 'Sundays 02:00', next: '2026-04-26 02:00:00', last: 'Apr 20 02:00:03', lastDur: '4.8s', lastStatus: 'ok', cap: 'chat.admin',         source: 'package' },
  ];

  const backupJobs = [
    { id: 'bk:plinth.nightly',             pkg: 'admin', name: 'Plinth nightly backup',      cadence: '30 2 * * *', human: 'daily at 02:30', next: '2026-04-24 02:30:00', last: 'Apr 23 02:30:04', lastDur: '14m 03s', lastStatus: 'ok',   cap: 'backups.write',  source: 'backup', target: 's3://example-backups/plinth-nightly', size: '142 MB', retention: '30d' },
    { id: 'bk:plinth.hourly',              pkg: 'admin', name: 'Hourly delta (WAL)',         cadence: '0 * * * *',  human: 'hourly',         next: '2026-04-23 15:00:00', last: 'Apr 23 14:00:01', lastDur: '11s',     lastStatus: 'ok',   cap: 'backups.write',  source: 'backup', target: 's3://example-backups/wal', size: '3.4 MB',    retention: '7d' },
    { id: 'bk:homecare.roster.export',     pkg: 'homecare', name: 'Roster CSV export',       cadence: '0 1 * * *',  human: 'daily at 01:00', next: '2026-04-24 01:00:00', last: 'Apr 23 01:00:08', lastDur: '1.8s',    lastStatus: 'fail', cap: 'backups.write',  source: 'backup', target: 's3://example-backups/homecare', size: '—',   retention: '90d', note: 's3.PutObject: 403 — simulated access denial for error-state mockup' },
  ];

  const userPolicy = {
    totalUsers:         5,
    activeUsers:        3,
    totalSchedules:    12,
    perUserCeiling:    50,
    perUserMax:         7,   // highest any user is using
    capability:        'user.schedule.own',
    scriptingCap:      'user.scripting',
    scriptingEnabled:   false, // default OFF
    visibilityNote:    'Admins cannot read reminder bodies, handler code, or per-user schedule lists. Only aggregates, ceilings, and cap grants.',
    perUserAgg: [
      { user: '@alex',  count: 7,  caps: ['user.schedule.own'], lastFired: '2026-04-23 09:00' },
      { user: '@jordan', count: 3,  caps: ['user.schedule.own'], lastFired: '2026-04-21 07:00' },
      { user: '@marek', count: 2,  caps: ['user.schedule.own'], lastFired: '2026-04-22 18:00' },
      { user: '@sam',   count: 0,  caps: [],                    lastFired: '—' },
      { user: '@priya', count: 0,  caps: [],                    lastFired: '—' },
    ],
  };

  const jobs = scope === 'system' ? systemJobs : scope === 'backups' ? backupJobs : [];
  const selectedJob = jobs.find(j => j.id === selected) || jobs[0];

  return (
    <div className="flex" style={{ height: '100%' }}
         data-ipoint="ext.admin.workspace">
      {/* Left: scope selector */}
      <div style={{ width: 200, borderRight: '1px solid var(--border)', background: 'var(--bg-1)', display: 'flex', flexDirection: 'column' }}>
        <div className="px-3 py-2 hair">
          <div className="text-3 mono" style={{ fontSize: 11, letterSpacing: '0.08em', textTransform: 'uppercase' }}>plinth.schedules</div>
        </div>
        {[
          { id: 'system',     label: 'Package jobs',  icon: 'package',  count: systemJobs.length, desc: 'Cron declared by installed packages' },
          { id: 'backups',    label: 'Backups',       icon: 'db',       count: backupJobs.length, desc: 'System-critical, separate retention' },
          { id: 'userPolicy', label: 'User policy',   icon: 'shield',   count: userPolicy.totalSchedules, desc: 'Ceilings & cap grants. No content visible to admins.' },
        ].map(s => (
          <div key={s.id}
               onClick={() => { setScope(s.id); setSelected(null); }}
               className={scope === s.id ? 'bg-2' : 'hover-bg'}
               style={{ padding: '10px 12px', cursor: 'pointer',
                        borderLeft: scope === s.id ? '2px solid var(--accent)' : '2px solid transparent' }}>
            <div className="flex items-center gap-2 mb-1">
              <Icon name={s.icon} size={14} style={{ color: scope === s.id ? 'var(--text-0)' : 'var(--text-3)' }} />
              <span className={scope === s.id ? 'text-0' : 'text-1'} style={{ fontSize: 13, fontWeight: 500, flex: 1 }}>{s.label}</span>
              <span className="mono text-3" style={{ fontSize: 11 }}>{s.count}</span>
            </div>
            <div className="text-3" style={{ fontSize: 11, lineHeight: 1.4, paddingLeft: 20 }}>{s.desc}</div>
          </div>
        ))}

        <div className="hair-t mt-auto px-3 py-2 text-3 mono" style={{ fontSize: 10.5 }}>
          reads: plinth.schedules
        </div>
      </div>

      {/* Middle: job list + detail (system/backups) OR user-policy view */}
      {scope !== 'userPolicy' ? (
      <div style={{ flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column' }}>
        <div className="flex items-center gap-3 px-5 py-2 hair" style={{ background: 'var(--bg-1)' }}>
          <div className="text-3 mono" style={{ fontSize: 11, letterSpacing: '0.08em', textTransform: 'uppercase' }}>
            {scope === 'system' && 'package_jobs'}
            {scope === 'backups' && 'backup_jobs'}
            {scope === 'user' && 'user_schedules'}
          </div>
          <span className="pill muted">{jobs.length}</span>
          {scope === 'backups' && <span className="pill danger">1 failing</span>}
          <div className="ml-auto flex items-center gap-2">
            <button className="tb-btn"><Icon name="play" size={11}/> Run now</button>
            <button className="tb-btn"><Icon name="history" size={11}/> History</button>
          </div>
        </div>

        <div style={{ flex: 1, overflow: 'auto' }}>
          <table className="tbl">
            <thead>
              <tr>
                <th style={{ width: 28 }}></th>
                <th>Name</th>
                <th style={{ width: 170 }}>Cadence</th>
                <th style={{ width: 170 }}>Next fire</th>
                <th style={{ width: 120 }}>Last</th>
                <th style={{ width: 80 }}>Status</th>
                <th style={{ width: 130 }}>Capability</th>
                <th style={{ width: 30 }}></th>
              </tr>
            </thead>
            <tbody>
              {jobs.map(j => {
                const active = selectedJob && selectedJob.id === j.id;
                return (
                  <tr key={j.id}
                      onClick={() => setSelected(j.id)}
                      style={{ cursor: 'pointer', background: active ? 'var(--bg-2)' : undefined }}>
                    <td>
                      {j.source === 'package' && <span style={{ width: 14, height: 14, borderRadius: 3, background: window.EXT_COLORS?.[j.pkg] || 'var(--text-3)', display: 'inline-block' }} />}
                      {j.source === 'backup' && <Icon name="db" size={14} style={{ color: 'var(--accent)' }} />}
                      {j.source === 'user' && <Icon name="user" size={14} style={{ color: 'var(--text-3)' }} />}
                    </td>
                    <td>
                      <div className="text-0" style={{ fontWeight: 500 }}>{j.name}</div>
                      <div className="sub mono" style={{ fontSize: 11 }}>{j.id}</div>
                    </td>
                    <td>
                      <div className="mono text-1" style={{ fontSize: 12 }}>{j.cadence}</div>
                      <div className="sub" style={{ fontSize: 11 }}>{j.human}</div>
                    </td>
                    <td className="mono text-1" style={{ fontSize: 12 }}>{j.next}</td>
                    <td>
                      <div className="mono text-2" style={{ fontSize: 11.5 }}>{j.last}</div>
                      <div className="sub mono" style={{ fontSize: 10.5 }}>{j.lastDur}</div>
                    </td>
                    <td>
                      {j.lastStatus === 'ok'   && <span className="pill ok"><span className="dot" style={{ background: 'var(--success)' }}/>ok</span>}
                      {j.lastStatus === 'warn' && <span className="pill warn"><span className="dot" style={{ background: 'var(--warn)' }}/>warn</span>}
                      {j.lastStatus === 'fail' && <span className="pill danger"><span className="dot" style={{ background: 'var(--danger)' }}/>fail</span>}
                    </td>
                    <td className="mono text-2" style={{ fontSize: 11.5 }}>{j.cap}</td>
                    <td><Icon name="dots" size={14} style={{ color: 'var(--text-3)' }}/></td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>

        {/* Detail strip */}
        {selectedJob && (
          <div className="hair-t" style={{ background: 'var(--bg-1)', padding: '10px 20px', maxHeight: 180, overflowY: 'auto' }}>
            <div className="flex items-center gap-2 mb-2">
              <span className="text-0" style={{ fontSize: 13, fontWeight: 600 }}>{selectedJob.name}</span>
              <span className="mono text-3" style={{ fontSize: 11 }}>{selectedJob.id}</span>
              <div className="ml-auto flex items-center gap-2">
                <button className="tb-btn" style={{ fontSize: 11.5 }}><Icon name="play" size={11}/> Run now</button>
                <button className="tb-btn" style={{ fontSize: 11.5 }}><Icon name="pause" size={11}/> {selectedJob.source === 'backup' ? 'Pause' : 'Disable'}</button>
                <button className="tb-btn" style={{ fontSize: 11.5 }}><Icon name="gear" size={11}/> Edit</button>
              </div>
            </div>
            <div style={{ display: 'grid', gridTemplateColumns: '120px 1fr 120px 1fr', gap: '4px 16px', fontSize: 12 }}>
              <span className="text-3">Cadence</span><span className="mono text-1">{selectedJob.cadence} <span className="text-3">· {selectedJob.human}</span></span>
              <span className="text-3">Next fire</span><span className="mono text-1">{selectedJob.next}</span>
              <span className="text-3">Capability</span><span className="mono text-1">{selectedJob.cap}</span>
              <span className="text-3">Last</span><span className="mono text-1">{selectedJob.last} · {selectedJob.lastDur}</span>
              {selectedJob.target && <><span className="text-3">Target</span><span className="mono text-1">{selectedJob.target}</span></>}
              {selectedJob.retention && <><span className="text-3">Retention</span><span className="mono text-1">{selectedJob.retention}</span></>}
              {selectedJob.size && <><span className="text-3">Last size</span><span className="mono text-1">{selectedJob.size}</span></>}
              {selectedJob.user && <><span className="text-3">Owner</span><span className="mono text-1">{selectedJob.user}</span></>}
              {selectedJob.body && <><span className="text-3">Body</span><span className="text-1" style={{ gridColumn: 'span 3' }}>{selectedJob.body}</span></>}
              {selectedJob.note && <><span className="text-3">Note</span><span className="text-1" style={{ gridColumn: 'span 3', color: selectedJob.lastStatus === 'fail' ? 'var(--danger)' : 'var(--warn)' }}>{selectedJob.note}</span></>}
            </div>
          </div>
        )}

        <div className="flex items-center justify-between px-5 py-2 hair-t mono text-3" style={{ fontSize: 11, background: 'var(--bg-1)' }}>
          <span>plinth.scheduler · engine: internal · timezone: Europe/London</span>
          <span>jobs evaluated every 30s</span>
        </div>
      </div>
      ) : (
        <UserSchedulePolicy policy={userPolicy} />
      )}

      {/* Right: composer — the "making it easy" panel */}
      <div style={{ width: 340, borderLeft: '1px solid var(--border)', background: 'var(--bg-1)', display: 'flex', flexDirection: 'column' }}
           data-ipoint="shell.rightPanel:admin.scheduleComposer" data-ipoint-layer="shell">
        <div className="px-4 py-3 hair">
          <div className="flex items-center gap-2 mb-1">
            <Icon name="plus" size={14} style={{ color: 'var(--text-3)' }} />
            <div className="text-0" style={{ fontWeight: 600, fontSize: 13 }}>New schedule</div>
          </div>
          <div className="mono text-3" style={{ fontSize: 11 }}>admin:schedule-composer</div>
        </div>

        <div style={{ flex: 1, overflowY: 'auto', padding: '12px 16px' }}>
          {/* Scope */}
          <div style={{ marginBottom: 14 }}>
            <div className="text-3" style={{ fontSize: 11, textTransform: 'uppercase', letterSpacing: '0.06em', marginBottom: 6 }}>Scope</div>
            <div style={{ display: 'flex', background: 'var(--bg-0)', border: '1px solid var(--border)', borderRadius: 5, padding: 2, gap: 2 }}>
              {[
                { v: 'system',  l: 'Package' },
                { v: 'backup',  l: 'Backup' },
              ].map(s => (
                <button key={s.v}
                        onClick={() => setComposer(c => ({ ...c, scope: s.v }))}
                        className={composer.scope === s.v ? 'text-0' : 'text-3'}
                        style={{ flex: 1, padding: '5px 8px', fontSize: 12, borderRadius: 3,
                                 background: composer.scope === s.v ? 'var(--bg-2)' : 'transparent' }}>
                  {s.l}
                </button>
              ))}
            </div>
            <div className="text-3" style={{ fontSize: 10.5, marginTop: 6, fontStyle: 'italic' }}>
              Admins create system and backup jobs. Users' personal schedules live in the Automations extension — admins manage ceilings and cap grants only.
            </div>
          </div>

          {/* Name */}
          <div style={{ marginBottom: 14 }}>
            <div className="text-3" style={{ fontSize: 11, textTransform: 'uppercase', letterSpacing: '0.06em', marginBottom: 6 }}>Name</div>
            <input type="text" value={composer.name} onChange={(e) => setComposer(c => ({ ...c, name: e.target.value }))}
                   placeholder={'pkg.homecare.roster.compact'}
                   className="mono"
                   style={{ width: '100%', background: 'var(--bg-0)', border: '1px solid var(--border)',
                            borderRadius: 4, padding: '6px 8px', fontSize: 12, color: 'var(--text-0)', outline: 'none' }} />
          </div>

          {/* Cadence — human-first, with cron preview */}
          <div style={{ marginBottom: 14 }}>
            <div className="text-3 flex items-center gap-2" style={{ fontSize: 11, textTransform: 'uppercase', letterSpacing: '0.06em', marginBottom: 6 }}>
              <span>Cadence</span>
              <span className="mono" style={{ textTransform: 'none', fontSize: 10, color: 'var(--text-3)', letterSpacing: 0 }}>natural language → cron</span>
            </div>
            <input type="text" value={composer.cadence} onChange={(e) => setComposer(c => ({ ...c, cadence: e.target.value }))}
                   style={{ width: '100%', background: 'var(--bg-0)', border: '1px solid var(--border)',
                            borderRadius: 4, padding: '6px 8px', fontSize: 13, color: 'var(--text-0)', outline: 'none',
                            fontFamily: 'Inter' }} />
            <div className="mono text-2" style={{ fontSize: 11, marginTop: 6, padding: '5px 8px', background: 'var(--bg-0)', borderRadius: 4, border: '1px dashed var(--border)' }}>
              30 8 * * *
              <span className="text-3"> · next fire <span className="text-1">tomorrow 08:30 Europe/London</span></span>
            </div>

            {/* Quick presets */}
            <div className="flex flex-wrap" style={{ gap: 4, marginTop: 8 }}>
              {['every hour', 'every day at 08:30', 'weekdays 09:00', 'Mondays 07:00', 'first of month'].map(p => (
                <button key={p} onClick={() => setComposer(c => ({ ...c, cadence: p }))}
                        className="tb-btn"
                        style={{ fontSize: 11, padding: '3px 7px' }}>
                  {p}
                </button>
              ))}
            </div>
          </div>

          {/* Handler — always shown now (admin composer is package/backup only) */}
          <div style={{ marginBottom: 14 }}>
            <div className="text-3" style={{ fontSize: 11, textTransform: 'uppercase', letterSpacing: '0.06em', marginBottom: 6 }}>Handler</div>
            <input type="text" value={composer.handler} onChange={(e) => setComposer(c => ({ ...c, handler: e.target.value }))}
                   placeholder="ext_homecare.jobs.roster_compact"
                   className="mono"
                   style={{ width: '100%', background: 'var(--bg-0)', border: '1px solid var(--border)',
                            borderRadius: 4, padding: '6px 8px', fontSize: 12, color: 'var(--text-0)', outline: 'none' }} />
            <div className="text-3" style={{ fontSize: 11, marginTop: 4 }}>
              Must be a <span className="mono">job:*</span> capability declared by an installed package.
            </div>
          </div>

          {/* Enabled */}
          <div className="flex items-center gap-2 py-2" style={{ marginBottom: 6 }}>
            <input type="checkbox" checked={composer.enabled} onChange={(e) => setComposer(c => ({ ...c, enabled: e.target.checked }))} style={{ accentColor: 'var(--accent)' }}/>
            <span className="text-1" style={{ fontSize: 12.5 }}>Enabled on create</span>
          </div>
        </div>

        <div className="hair-t px-4 py-3 flex items-center gap-2">
          <button className="tb-btn" style={{ flex: 1, justifyContent: 'center', background: 'var(--accent-soft)', borderColor: 'var(--accent-soft)', color: 'var(--accent)', fontWeight: 500 }}>
            Create schedule
          </button>
          <button className="tb-btn">Cancel</button>
        </div>
      </div>
    </div>
  );
};

// Admin-facing, privacy-safe aggregate view of user schedules.
// Never shows reminder bodies, handler code, or per-user item lists — only
// aggregate counts, ceilings, capability grants, and the policy toggle for
// scripting. This is what "Admin sees user activity" must mean under the
// capability model.
const UserSchedulePolicy = ({ policy }) => (
  <div style={{ flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column' }}>
    <div className="flex items-center gap-3 px-5 py-2 hair" style={{ background: 'var(--bg-1)' }}>
      <div className="text-3 mono" style={{ fontSize: 11, letterSpacing: '0.08em', textTransform: 'uppercase' }}>user_schedule_policy</div>
      <span className="pill muted">{policy.totalSchedules} active</span>
      <span className="pill info"><Icon name="shield" size={10}/>privacy-scoped</span>
    </div>

    <div style={{ flex: 1, overflow: 'auto', padding: '16px 20px' }}>
      {/* Privacy banner — load-bearing */}
      <div style={{ background: 'var(--accent-soft)', border: '1px solid var(--border)', borderRadius: 6, padding: '10px 14px', marginBottom: 16, display: 'flex', gap: 10 }}>
        <Icon name="shield" size={14} style={{ color: 'var(--accent)', marginTop: 2, flexShrink: 0 }}/>
        <div>
          <div className="text-0" style={{ fontSize: 12.5, fontWeight: 500, marginBottom: 2 }}>What admins can and can't see</div>
          <div className="text-2" style={{ fontSize: 12, lineHeight: 1.55 }}>{policy.visibilityNote}</div>
        </div>
      </div>

      {/* KPI tiles */}
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(4, 1fr)', gap: 10, marginBottom: 20 }}>
        {[
          { k: 'Active users',       v: policy.activeUsers + ' / ' + policy.totalUsers, sub: 'with ≥1 schedule' },
          { k: 'Total schedules',    v: policy.totalSchedules,                         sub: 'count only' },
          { k: 'Per-user ceiling',   v: policy.perUserCeiling,                         sub: 'enforced' },
          { k: 'Current max',        v: policy.perUserMax,                             sub: '@alex' },
        ].map(tile => (
          <div key={tile.k} className="surface-1" style={{ borderRadius: 6, padding: '10px 12px' }}>
            <div className="text-3 mono" style={{ fontSize: 10.5, textTransform: 'uppercase', letterSpacing: '0.06em' }}>{tile.k}</div>
            <div className="text-0 mono" style={{ fontSize: 20, fontWeight: 600, marginTop: 4 }}>{tile.v}</div>
            <div className="text-3" style={{ fontSize: 11, marginTop: 2 }}>{tile.sub}</div>
          </div>
        ))}
      </div>

      {/* Per-user aggregate — counts only, no bodies */}
      <div className="text-0" style={{ fontSize: 13, fontWeight: 600, marginBottom: 8 }}>Per-user aggregate</div>
      <div className="surface-1" style={{ borderRadius: 6, marginBottom: 20 }}>
        <table className="tbl">
          <thead><tr><th>User</th><th>Schedules</th><th>Capabilities</th><th>Last run</th><th style={{ width: 90 }}>Actions</th></tr></thead>
          <tbody>
            {policy.perUserAgg.map(u => (
              <tr key={u.user}>
                <td className="mono text-0">{u.user}</td>
                <td className="mono text-1">{u.count === 0 ? <span className="text-3">—</span> : u.count}</td>
                <td>
                  {u.caps.length === 0
                    ? <span className="text-3 mono" style={{ fontSize: 11 }}>no grants</span>
                    : u.caps.map(c => <span key={c} className="mono" style={{ fontSize: 11, padding: '2px 6px', background: 'var(--bg-2)', borderRadius: 3, marginRight: 4 }}>{c}</span>)}
                </td>
                <td className="mono text-2" style={{ fontSize: 11.5 }}>{u.lastFired}</td>
                <td><button className="tb-btn" style={{ fontSize: 11 }}>Revoke cap</button></td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>

      {/* Capability grants & policy */}
      <div className="text-0" style={{ fontSize: 13, fontWeight: 600, marginBottom: 8 }}>Capabilities &amp; ceilings</div>
      <div className="surface-1" style={{ borderRadius: 6, padding: '12px 16px' }}>
        <div style={{ display: 'grid', gridTemplateColumns: '1fr auto', rowGap: 10, columnGap: 14, alignItems: 'center' }}>
          <div>
            <div className="text-1" style={{ fontSize: 12.5, fontWeight: 500 }}>Personal recurring schedules</div>
            <div className="mono text-3" style={{ fontSize: 11 }}>{policy.capability} · ceiling {policy.perUserCeiling} per user</div>
          </div>
          <span className="pill ok"><span className="dot" style={{ background: 'var(--success)' }}/>granted to all</span>

          <div>
            <div className="text-1" style={{ fontSize: 12.5, fontWeight: 500 }}>User scripting &amp; custom handlers</div>
            <div className="mono text-3" style={{ fontSize: 11 }}>{policy.scriptingCap} · gated · default off</div>
          </div>
          <span className={policy.scriptingEnabled ? 'pill ok' : 'pill muted'}>
            <span className="dot" style={{ background: policy.scriptingEnabled ? 'var(--success)' : 'var(--text-3)' }}/>
            {policy.scriptingEnabled ? 'enabled' : 'disabled'}
          </span>
        </div>
      </div>
    </div>

    <div className="flex items-center justify-between px-5 py-2 hair-t mono text-3" style={{ fontSize: 11, background: 'var(--bg-1)' }}>
      <span>plinth.scheduler · aggregation · admins cannot read user.schedule.* content</span>
      <span>refreshed just now</span>
    </div>
  </div>
);

Object.assign(window, { AdminSchedules, UserSchedulePolicy });
