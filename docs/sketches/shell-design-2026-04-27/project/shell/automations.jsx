// Automations — where a user lives for personal scripting & scheduling.
// Three subtabs:
//   - my-schedules: personal recurring items (admin can't see bodies)
//   - scripts:      code-mode, gated on `user.scripting` cap, default off
//   - triggers:     contextual "schedule this" reminders raised from other apps
//
// The design distinction the user called out: scheduling a *deferred action*
// from another app ("remind me on Tuesday") is right-click contextual on the
// artifact itself. That's represented here on the `triggers` tab as the inbox
// that receives those items back. Recurring jobs + scripts live here natively.

const AutomationsPanel = ({ activeSubtab }) => {
  const st = activeSubtab || 'my-schedules';
  return (
    <div style={{ height: '100%', display: 'flex', flexDirection: 'column' }}>
      <div className="flex items-baseline gap-3 px-6 py-3 hair">
        <div className="text-0" style={{ fontSize: 17, fontWeight: 600 }}>Automations</div>
        <div className="mono text-3" style={{ fontSize: 11 }}>ext_automations@0.4.0 · user-space</div>
        <div className="ml-auto flex items-center gap-2">
          <span className="pill info"><Icon name="shield" size={10}/>your data · private to you</span>
        </div>
      </div>

      {st === 'my-schedules' && <MySchedules/>}
      {st === 'scripts'      && <UserScripts/>}
      {st === 'triggers'     && <DeferredInbox/>}
    </div>
  );
};

const MySchedules = () => {
  const rows = [
    { name: 'Check-in: Mrs. Patel',     cadence: 'Tue + Thu 09:00', next: 'Tue 28 Apr · 09:00', body: 'Before the 14:00 visit', from: 'homecare · client card' },
    { name: 'Order meds refills',       cadence: '4-weekly',        next: 'Mon 11 May · 08:00', body: 'Open Homecare → Meds', from: 'homecare · meds review' },
    { name: 'Weekly handover export',   cadence: 'Mon 07:00',       next: 'Mon 28 Apr · 07:00', body: 'Post to chat #handover', from: 'script · weekly_handover.js' },
  ];
  return (
    <div style={{ flex: 1, overflow: 'auto', padding: '16px 24px' }}>
      <div className="flex items-center justify-between mb-3">
        <div className="text-0" style={{ fontSize: 13.5, fontWeight: 600 }}>My schedules</div>
        <button className="tb-btn"><Icon name="plus" size={11}/>New schedule</button>
      </div>
      <div className="surface-1" style={{ borderRadius: 6 }}>
        <table className="tbl">
          <thead><tr><th>Name</th><th style={{ width: 150 }}>Cadence</th><th style={{ width: 180 }}>Next</th><th>Body / target</th><th style={{ width: 180 }}>Source</th></tr></thead>
          <tbody>
            {rows.map((r, i) => (
              <tr key={i}>
                <td className="text-0" style={{ fontWeight: 500 }}>{r.name}</td>
                <td className="mono text-1">{r.cadence}</td>
                <td className="mono text-1">{r.next}</td>
                <td className="text-2" style={{ fontSize: 12.5 }}>{r.body}</td>
                <td className="mono text-3" style={{ fontSize: 11 }}>{r.from}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      <div className="mono text-3 mt-3" style={{ fontSize: 11 }}>
        cap: user.schedule.own · ceiling 50 per user · only you and the kernel see these rows
      </div>
    </div>
  );
};

const UserScripts = () => {
  const [enabled, setEnabled] = React.useState(true);
  return (
    <div style={{ flex: 1, overflow: 'auto', padding: '16px 24px' }}>
      {/* Gate banner */}
      <div className="surface-1" style={{ borderRadius: 6, padding: '12px 14px', marginBottom: 16, display: 'flex', gap: 12, alignItems: 'center' }}>
        <Icon name="shield" size={14} className="text-3"/>
        <div style={{ flex: 1 }}>
          <div className="text-1" style={{ fontSize: 12.5, fontWeight: 500 }}>User scripting</div>
          <div className="mono text-3" style={{ fontSize: 11 }}>user.scripting · granted by admin · runs sandboxed with only your caps</div>
        </div>
        <span className={enabled ? 'pill ok' : 'pill muted'}>
          <span className="dot" style={{ background: enabled ? 'var(--success)' : 'var(--text-3)' }}/>{enabled ? 'enabled' : 'disabled'}
        </span>
      </div>

      <div className="flex items-center justify-between mb-3">
        <div className="text-0" style={{ fontSize: 13.5, fontWeight: 600 }}>Scripts</div>
        <div className="flex items-center gap-2">
          <button className="tb-btn"><Icon name="folder" size={11}/>Examples</button>
          <button className="tb-btn"><Icon name="plus" size={11}/>New script</button>
        </div>
      </div>

      <div className="surface-1" style={{ borderRadius: 6, marginBottom: 18 }}>
        <table className="tbl">
          <thead><tr><th>Name</th><th style={{ width: 200 }}>Trigger</th><th style={{ width: 160 }}>Caps used</th><th style={{ width: 110 }}>Last run</th></tr></thead>
          <tbody>
            <tr>
              <td>
                <div className="text-0" style={{ fontWeight: 500 }}>weekly_handover.js</div>
                <div className="sub mono" style={{ fontSize: 11 }}>export the week and post to #handover</div>
              </td>
              <td><span className="mono" style={{ fontSize: 11.5 }}>cron · Mon 07:00</span></td>
              <td className="mono text-2" style={{ fontSize: 11 }}>homecare.read, chat.post</td>
              <td className="mono text-2" style={{ fontSize: 11.5 }}>Apr 21 07:00</td>
            </tr>
            <tr>
              <td>
                <div className="text-0" style={{ fontWeight: 500 }}>after_visit_note.js</div>
                <div className="sub mono" style={{ fontSize: 11 }}>on visit.logged → draft note template</div>
              </td>
              <td><span className="mono" style={{ fontSize: 11.5 }}>event · homecare.visit.logged</span></td>
              <td className="mono text-2" style={{ fontSize: 11 }}>notes.write</td>
              <td className="mono text-2" style={{ fontSize: 11.5 }}>Apr 23 14:02</td>
            </tr>
          </tbody>
        </table>
      </div>

      {/* Code preview */}
      <div className="text-0 mb-2" style={{ fontSize: 13, fontWeight: 600 }}>weekly_handover.js</div>
      <div className="mono" style={{ background: 'var(--bg-0)', border: '1px solid var(--border)', borderRadius: 6, padding: '12px 14px', fontSize: 12, lineHeight: 1.75, color: 'var(--text-1)', overflow: 'auto' }}>
        <div><span style={{ color: 'var(--text-3)' }}>// cap: homecare.read, chat.post</span></div>
        <div><span style={{ color: 'var(--accent)' }}>on</span>(<span style={{ color: 'var(--success)' }}>{"'cron:mon-07:00'"}</span>, <span style={{ color: 'var(--accent)' }}>async</span> () =&gt; {'{'}</div>
        <div>{'  '}<span style={{ color: 'var(--accent)' }}>const</span> week = <span style={{ color: 'var(--accent)' }}>await</span> homecare.visits.thisWeek();</div>
        <div>{'  '}<span style={{ color: 'var(--accent)' }}>const</span> summary = format(week);</div>
        <div>{'  '}<span style={{ color: 'var(--accent)' }}>await</span> chat.post(<span style={{ color: 'var(--success)' }}>{"'#handover'"}</span>, summary);</div>
        <div>{'}'});</div>
      </div>
      <div className="mono text-3 mt-2" style={{ fontSize: 11 }}>
        sandbox · 512 KB memory · 3s wall time · no network · reads bounded by your caps
      </div>
    </div>
  );
};

const DeferredInbox = () => {
  const items = [
    { at: 'in 2 days · Thu 09:00', title: 'Follow up on Patel meds review',   from: 'notes · handover', ctx: 'triggered from a note', action: 'Open note' },
    { at: 'in 5 days · Mon 08:00', title: 'Prepare supplies order',            from: 'chat · #supplies', ctx: 'right-click → remind me',  action: 'Open chat' },
    { at: 'Apr 30 · end of month', title: 'Close out April rota',              from: 'homecare · rota',  ctx: 'scheduled from menu',      action: 'Open rota' },
  ];
  return (
    <div style={{ flex: 1, overflow: 'auto', padding: '16px 24px' }}>
      <div className="mb-3">
        <div className="text-0" style={{ fontSize: 13.5, fontWeight: 600 }}>Deferred actions</div>
        <div className="text-3" style={{ fontSize: 12 }}>
          One-off items you snoozed or scheduled from another app via <span className="kbd">right-click</span> → <span className="kbd">Remind me</span>.
        </div>
      </div>
      <div className="surface-1" style={{ borderRadius: 6 }}>
        {items.map((it, i) => (
          <div key={i} className="flex items-center gap-3 px-4 py-3" style={{ borderBottom: '1px solid var(--border-soft)' }}>
            <Icon name="clock" size={14} className="text-3"/>
            <div style={{ flex: 1 }}>
              <div className="text-0" style={{ fontSize: 13, fontWeight: 500 }}>{it.title}</div>
              <div className="mono text-3" style={{ fontSize: 11 }}>{it.from} · {it.ctx}</div>
            </div>
            <div className="mono text-2" style={{ fontSize: 12 }}>{it.at}</div>
            <button className="tb-btn" style={{ fontSize: 11.5 }}>{it.action}</button>
          </div>
        ))}
      </div>
    </div>
  );
};

Object.assign(window, { AutomationsPanel });
