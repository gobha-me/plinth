// Extension-provided tray surfaces: live status icon + media-player popover.
// Matches the reference sketch: the tray is not just a bell + avatar, it is
// a row of live surfaces any extension can claim. Shell renders the container,
// extension renders the icon body and popover body.

// ---- Status health icon (plinth core extension) -----------------------------
const StatusHealthIcon = ({ open, onClick }) => (
  <div className={"tray-icon " + (open ? 'open' : '')} onClick={onClick} title="System health">
    <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
      <polyline points="3 12 6 12 8 8 11 16 13 10 16 14 18 12 21 12"/>
    </svg>
    <span style={{
      position: 'absolute', top: 5, right: 5,
      width: 6, height: 6, borderRadius: '50%',
      background: 'var(--success)',
      border: '2px solid var(--bg-1)',
    }} />
  </div>
);

const StatusHealthTray = ({ width = 320, onClose }) => (
  // Anchor to the tray-row right edge via negative right so popovers
  // from middle tray icons don't fly off the viewport.
  <div className="popover" style={{ right: -60, width }}>
    <div className="popover-arrow" style={{ right: 74 }} />
    <div className="flex items-center justify-between px-3 py-2 hair">
      <div className="text-0" style={{ fontWeight: 600, fontSize: 13 }}>System</div>
      <span className="pop-source">plinth-core</span>
    </div>
    <div className="px-3 py-3" style={{ display: 'grid', gap: 8 }}>
      {[
        { k: 'kernel',     v: '0.6.6',        pill: 'ok',    pillLabel: 'healthy' },
        { k: 'pubsub',     v: '248 conn',     pill: 'ok',    pillLabel: 'ok' },
        { k: 'storage',    v: '4.1 / 32 GB',  pill: 'ok',    pillLabel: 'ok' },
        { k: 'backups',    v: 'last 02:00',   pill: 'warn',  pillLabel: '1 failing' },
        { k: 'scheduler',  v: '6 jobs queued', pill: 'ok',   pillLabel: 'on time' },
      ].map(r => (
        <div key={r.k} className="flex items-center gap-3" style={{ fontSize: 12 }}>
          <span className="mono text-2" style={{ width: 78 }}>{r.k}</span>
          <span className="text-1" style={{ flex: 1 }}>{r.v}</span>
          <span className={"pill " + r.pill} style={{ fontSize: 10 }}>{r.pillLabel}</span>
        </div>
      ))}
    </div>
    <div className="hair-t px-3 py-2 mono text-3" style={{ fontSize: 10.5 }}>
      extension · tray icon provided by plinth-core · capability: plinth.status.read
    </div>
  </div>
);

// ---- Media player tray icon (ext_media extension) ---------------------------
// The icon itself animates even when the popover is closed — it's a live status
// surface. That is the point the reference sketch is making.
const MediaTrayIcon = ({ open, onClick, playing }) => (
  <div className={"tray-icon " + (open ? 'open' : '')} onClick={onClick} title="Now playing"
       style={{ color: playing ? 'var(--accent)' : 'var(--text-2)' }}>
    <svg width="18" height="18" viewBox="0 0 24 24" fill="currentColor">
      <rect x="5"  y="8"  width="2.5" height="8" rx="0.8" style={{ transformOrigin: 'center', animation: playing ? 'eq1 0.9s ease-in-out infinite' : 'none' }} />
      <rect x="10" y="6"  width="2.5" height="12" rx="0.8" style={{ animation: playing ? 'eq2 0.9s ease-in-out 0.12s infinite' : 'none' }} />
      <rect x="15" y="9"  width="2.5" height="6" rx="0.8" style={{ animation: playing ? 'eq3 0.9s ease-in-out 0.24s infinite' : 'none' }} />
    </svg>
  </div>
);

const MediaTrayPopover = ({ width = 380, playing, onTogglePlay, onClose }) => (
  <div className="popover" style={{ right: -24, width }}>
    <div className="popover-arrow" style={{ right: 38 }} />
    <div className="flex items-center justify-between px-3 py-2 hair">
      <div className="text-0" style={{ fontWeight: 600, fontSize: 13 }}>Now playing</div>
      <span className="pop-source">ext_media</span>
    </div>
    <div className="px-4 py-4">
      <div className="flex items-center gap-3" style={{ marginBottom: 14 }}>
        <div style={{ width: 56, height: 56, borderRadius: 6, background: 'linear-gradient(135deg, #2b3140, #3a4256, #4c5570)',
                      display: 'inline-flex', alignItems: 'center', justifyContent: 'center', color: 'var(--text-2)', flexShrink: 0 }}>
          <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5"><path d="M9 18V5l12-2v13"/><circle cx="6" cy="18" r="3"/><circle cx="18" cy="16" r="3"/></svg>
        </div>
        <div style={{ flex: 1, minWidth: 0 }}>
          <div className="text-0" style={{ fontSize: 14, fontWeight: 600, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>Midnight Geometry</div>
          <div className="text-2" style={{ fontSize: 12, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>Parallel Fields · Inner Atlas</div>
        </div>
      </div>

      <div style={{ height: 3, background: 'var(--bg-3)', borderRadius: 2, overflow: 'hidden', marginBottom: 6 }}>
        <div style={{ height: '100%', width: '38%', background: 'var(--accent)' }} />
      </div>
      <div className="flex justify-between mono text-3" style={{ fontSize: 11, marginBottom: 14 }}>
        <span>1:24</span><span>3:42</span>
      </div>

      <div className="flex items-center justify-center gap-2">
        <button className="tray-icon" title="Shuffle"><Icon name="shuffle" size={14}/></button>
        <button className="tray-icon" title="Prev">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor"><polygon points="19 20 9 12 19 4"/><rect x="4" y="5" width="2" height="14"/></svg>
        </button>
        <button onClick={onTogglePlay} title={playing ? 'Pause' : 'Play'}
                style={{ width: 44, height: 44, borderRadius: '50%', background: 'var(--accent)', color: '#0b1020',
                         display: 'inline-flex', alignItems: 'center', justifyContent: 'center', cursor: 'pointer', border: 'none' }}>
          {playing
            ? <svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor"><rect x="6" y="5" width="4" height="14" rx="1"/><rect x="14" y="5" width="4" height="14" rx="1"/></svg>
            : <svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor"><polygon points="6 4 20 12 6 20"/></svg>}
        </button>
        <button className="tray-icon" title="Next">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor"><polygon points="5 4 15 12 5 20"/><rect x="18" y="5" width="2" height="14"/></svg>
        </button>
        <button className="tray-icon" title="Repeat"><Icon name="repeat" size={14}/></button>
      </div>
    </div>
    <div className="hair-t px-3 py-2 mono text-3" style={{ fontSize: 10.5 }}>
      extension · ext_media.player · capability: media.player.control
    </div>
  </div>
);

Object.assign(window, { StatusHealthIcon, StatusHealthTray, MediaTrayIcon, MediaTrayPopover });
