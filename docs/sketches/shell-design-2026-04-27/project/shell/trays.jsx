// Shell-owned tray panels: notifications + avatar. Plus chrome_essential fallback.
const NotificationsTray = ({ width = 360, onClose }) => {
  // The first item is the anatomy of an extension USING the tray — action buttons,
  // avatar, deep-link. Everything else flows from that same contract.
  return (
    <div className="popover" style={{ right: 0, width }}>
      <div className="popover-arrow" style={{ right: 44 }} />
      <div className="flex items-center justify-between px-3 py-2 hair">
        <div className="text-0" style={{ fontWeight: 600, fontSize: 13 }}>Notifications</div>
        <span className="mono text-3" style={{ fontSize: 10, letterSpacing: '0.06em', textTransform: 'uppercase' }}>shell:notifications</span>
      </div>
      <div style={{ maxHeight: 460, overflowY: 'auto' }}>

        {/* Rich item — extension-posted, with actions */}
        <div className="px-3 py-3" style={{ borderBottom: '1px solid var(--border-soft)', background: 'var(--bg-2)' }}>
          <div className="flex items-center gap-2 mb-2">
            <span style={{ width: 18, height: 18, borderRadius: 3, background: '#3bb77e', color: 'white',
                           display: 'inline-flex', alignItems: 'center', justifyContent: 'center',
                           fontFamily: 'JetBrains Mono', fontSize: 10, fontWeight: 700 }}>H</span>
            <span className="mono text-3" style={{ fontSize: 10 }}>homecare</span>
            <span className="text-3" style={{ fontSize: 10 }}>·</span>
            <span className="text-3 mono" style={{ fontSize: 10 }}>just now</span>
            <span className="pill warn ml-auto">action required</span>
          </div>
          <div className="text-0" style={{ fontSize: 13, fontWeight: 600, marginBottom: 3 }}>Schedule conflict — Mrs. Patel</div>
          <div className="text-2" style={{ fontSize: 12, lineHeight: 1.5, marginBottom: 8 }}>
            Tuesday 14:00 overlaps with Mr. Kowalski's 14:30. Shift one, or acknowledge the overlap.
          </div>
          <div className="flex items-center gap-2" style={{ marginTop: 6 }}>
            <button className="tb-btn" style={{ fontSize: 11.5, background: 'var(--accent-soft)', borderColor: 'var(--accent-soft)', color: 'var(--accent)' }}>Reschedule</button>
            <button className="tb-btn" style={{ fontSize: 11.5 }}>Acknowledge</button>
            <span className="text-3 mono ml-auto" style={{ fontSize: 10 }}>ext_homecare.notify("visit.conflict")</span>
          </div>
        </div>

        {/* Simpler items */}
        {[
          { ext: 'notes',    color: '#5aa9ff', glyph: 'N', title: 'Shared with partner', body: '"Care binder / intake" — Jordan accepted the share.', time: '42m', sev: 'info' },
          { ext: 'admin',    color: '#b07cff', glyph: 'A', title: 'Package upgrade available', body: 'homecare 1.3.1 → 1.3.2. Review changelog before upgrade.', time: '2h', sev: 'info' },
          { ext: 'admin',    color: '#b07cff', glyph: 'A', title: 'Backup completed', body: 'plinth.nightly → s3://example-backups · 142 MB', time: '6h', sev: 'muted' },
          { ext: 'shell',    color: '#8a94a0', glyph: 'S', title: 'Session extended', body: 'PAT refreshed from plinth-cli on host tor.', time: 'yesterday', sev: 'muted' },
        ].map((n, i) => (
          <div key={i} className="hover-bg px-3 py-3" style={{ borderBottom: '1px solid var(--border-soft)', cursor: 'pointer' }}>
            <div className="flex items-center gap-2 mb-1">
              <span style={{ width: 14, height: 14, borderRadius: 2, background: n.color, color: 'white',
                             display: 'inline-flex', alignItems: 'center', justifyContent: 'center',
                             fontFamily: 'JetBrains Mono', fontSize: 9, fontWeight: 700 }}>{n.glyph}</span>
              <span className="mono text-3" style={{ fontSize: 10 }}>{n.ext}</span>
              <span className="text-3" style={{ fontSize: 10 }}>·</span>
              <span className="text-3 mono" style={{ fontSize: 10 }}>{n.time}</span>
              {n.sev === 'info' && <span className="pill info ml-auto">info</span>}
            </div>
            <div className="text-0" style={{ fontSize: 13, fontWeight: 500, marginBottom: 2 }}>{n.title}</div>
            <div className="text-2" style={{ fontSize: 12, lineHeight: 1.5 }}>{n.body}</div>
          </div>
        ))}
      </div>
      <div className="flex items-center justify-between px-3 py-2 hair-t">
        <button className="tb-btn text-2" style={{ fontSize: 12 }}>Mark all read</button>
        <button className="tb-btn text-2" style={{ fontSize: 12 }}>Settings</button>
      </div>
    </div>
  );
};

const AvatarTray = ({ width = 320, onClose }) => {
  const [theme, setTheme] = React.useState(document.documentElement.getAttribute('data-theme') || 'dark');
  const [scale, setScale] = React.useState(100);
  const setT = (t) => {
    setTheme(t);
    if (t === 'system') {
      const prefersDark = window.matchMedia('(prefers-color-scheme: dark)').matches;
      document.documentElement.setAttribute('data-theme', prefersDark ? 'dark' : 'light');
    } else {
      document.documentElement.setAttribute('data-theme', t);
    }
  };
  return (
    <div className="popover" style={{ right: 0, width }}>
      <div className="popover-arrow" style={{ right: 14 }} />

      {/* Identity */}
      <div className="px-4 py-3 hair flex items-center gap-3">
        <div style={{ width: 36, height: 36, borderRadius: 6, background: '#3b82f6', color: 'white',
                      display: 'inline-flex', alignItems: 'center', justifyContent: 'center',
                      fontFamily: 'JetBrains Mono, monospace', fontWeight: 700 }}>RL</div>
        <div className="min-w-0">
          <div className="text-0" style={{ fontWeight: 600, fontSize: 14 }}>Alex Example</div>
          <div className="text-3 mono" style={{ fontSize: 11 }}>@alex · admin · everyone</div>
        </div>
      </div>

      {/* Theme */}
      <div className="px-3 py-2">
        <div className="flex items-center gap-2 px-1 py-1">
          <Icon name="sun" size={15} style={{ color: 'var(--text-3)' }} />
          <span className="text-1" style={{ fontSize: 12.5, flex: 1 }}>Theme</span>
          <div className="flex" style={{ background: 'var(--bg-0)', borderRadius: 5, padding: 2, gap: 2, border: '1px solid var(--border)' }}>
            {['light', 'dark', 'system'].map(t => (
              <button key={t}
                      onClick={() => setT(t)}
                      className={theme === t ? 'text-0' : 'text-3'}
                      style={{ padding: '3px 9px', fontSize: 11, fontWeight: 500, borderRadius: 3,
                               background: theme === t ? 'var(--bg-2)' : 'transparent', textTransform: 'capitalize' }}>
                {t}
              </button>
            ))}
          </div>
        </div>

        <div className="flex items-center gap-2 px-1 py-2">
          <Icon name="dot-grid" size={15} style={{ color: 'var(--text-3)' }} />
          <span className="text-1" style={{ fontSize: 12.5, flex: 1 }}>UI scale</span>
          <input type="range" min="80" max="175" step="5" value={scale} className="tweak-range"
                 onChange={(e) => setScale(+e.target.value)} style={{ width: 110 }} />
          <span className="mono text-2" style={{ fontSize: 11, width: 36, textAlign: 'right' }}>{scale}%</span>
        </div>
      </div>

      <div className="hair-t" />

      <div className="px-3 py-2">
        <div className="hover-bg flex items-center gap-2 px-2 py-2" style={{ borderRadius: 4, cursor: 'pointer' }}>
          <Icon name="switch" size={15} style={{ color: 'var(--text-3)' }} />
          <span className="text-1" style={{ fontSize: 13 }}>Switch persona</span>
          <span className="text-3 mono ml-auto" style={{ fontSize: 11 }}>alex → caregiver</span>
        </div>
        <div className="hover-bg flex items-center gap-2 px-2 py-2" style={{ borderRadius: 4, cursor: 'pointer' }}>
          <Icon name="gear" size={15} style={{ color: 'var(--text-3)' }} />
          <span className="text-1" style={{ fontSize: 13 }}>User settings</span>
        </div>
      </div>

      <div className="hair-t" />

      <div className="px-3 py-2">
        <div className="hover-bg flex items-center gap-2 px-2 py-2" style={{ borderRadius: 4, cursor: 'pointer', color: 'var(--danger)' }}>
          <Icon name="sign-out" size={15} />
          <span style={{ fontSize: 13 }}>Sign out</span>
        </div>
      </div>
    </div>
  );
};

// chrome_essential fallback: the avatar panel component crashed — shell provides minimal renderer.
const AvatarFallbackTray = ({ width = 320, onClose }) => {
  return (
    <div className="popover relative" style={{ right: 0, width }}>
      <div className="popover-arrow" style={{ right: 14 }} />
      <span className="fallback-label">fallback</span>

      <div className="px-4 py-4 hair flex items-start gap-3">
        <div style={{ width: 30, height: 30, borderRadius: 4, background: 'var(--bg-3)', color: 'var(--text-2)',
                      display: 'inline-flex', alignItems: 'center', justifyContent: 'center' }}>
          <Icon name="user" size={16} />
        </div>
        <div className="min-w-0 flex-1">
          <div className="text-0" style={{ fontWeight: 600, fontSize: 13 }}>alex</div>
          <div className="text-3 mono" style={{ fontSize: 11, marginTop: 2 }}>shell/panels/account.js</div>
          <div className="text-3" style={{ fontSize: 12, lineHeight: 1.55, marginTop: 6 }}>
            The account panel failed to mount. The shell is rendering a minimal fallback so you can still sign out.
          </div>
        </div>
      </div>

      <div className="px-3 py-3">
        <button className="w-full flex items-center justify-center gap-2 py-2"
                style={{ background: 'var(--danger-soft)', color: 'var(--danger)', border: '1px solid var(--danger-soft)', borderRadius: 5, fontSize: 13, fontWeight: 500, cursor: 'pointer' }}>
          <Icon name="sign-out" size={14} />
          Sign out
        </button>
        <div className="text-3 mono" style={{ fontSize: 10.5, marginTop: 8, textAlign: 'center' }}>
          panels[&#123; chrome_essential: true &#125;]
        </div>
      </div>
    </div>
  );
};

Object.assign(window, { NotificationsTray, AvatarTray, AvatarFallbackTray });
