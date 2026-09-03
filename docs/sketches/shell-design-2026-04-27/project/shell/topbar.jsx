// Topbar — renders identity + sub-tabs + tray. The active extension owns identity.
const EXT_COLORS = {
  notes:    '#5aa9ff',
  homecare: '#3bb77e',
  admin:    '#b07cff',
  automations: '#e4b847',
  files:    '#d6a640',
  chat:     '#e26565',
  kb:       '#ff8c5a',
  shell:    '#8a94a0',
};

const AppIdentity = ({ ext, treatment, onOpenSwitcher, switcherOpen }) => {
  if (!ext) {
    // Home / idle
    return (
      <div className="appid-mark text-2" style={{ cursor: 'default' }}>
        <span style={{ color: 'var(--text-3)', fontFamily: 'JetBrains Mono, monospace', fontSize: 11, letterSpacing: '0.08em', textTransform: 'uppercase' }}>plinth</span>
      </div>
    );
  }
  const color = EXT_COLORS[ext.id] || 'var(--accent)';
  if (treatment === 'tab') {
    return (
      <div className="appid-tab" onClick={onOpenSwitcher} style={{ cursor: 'pointer' }}>
        <span style={{ width: 10, height: 10, borderRadius: 2, background: color, display: 'inline-block' }} />
        <span>{ext.name}</span>
        <Icon name="chev-down" size={12} />
      </div>
    );
  }
  if (treatment === 'chip') {
    return (
      <div className="appid-chip" onClick={onOpenSwitcher} style={{ cursor: 'pointer' }}>
        <span className="swatch" style={{ background: color }}>{ext.glyph}</span>
        <span>{ext.name}</span>
        <Icon name="chev-down" size={12} style={{ color: 'var(--text-3)' }} />
      </div>
    );
  }
  // mark (default)
  return (
    <div className={"appid-mark " + (switcherOpen ? 'bg-2' : '')} onClick={onOpenSwitcher} style={{ cursor: 'pointer', borderRadius: 5 }}>
      <span className="glyph" style={{ background: color }}>{ext.glyph}</span>
      <span>{ext.name}</span>
      <Icon name="chev-down" size={12} className="chev" />
    </div>
  );
};

const Topbar = ({
  ext, subtabs, activeSubtab, onSubtab,
  onHome, homeActive, identityTreatment,
  actions, // extension-owned right-side actions
  onOpenSwitcher, switcherOpen,
  trayWidth,
  trays, // { bellOpen, avatarOpen, statusOpen, mediaOpen, unread, fallback, mediaPlaying }
  onTraySet,
  onToggleMediaPlay,
}) => {
  return (
    <div className="surface-1 flex items-stretch relative" style={{ height: 48, borderRadius: 0, borderLeft: 0, borderRight: 0, borderTop: 0 }}
         data-ipoint="shell.topbar" data-ipoint-layer="shell">
      {/* Left: home */}
      <div className="flex items-center px-2 gap-1">
        <div className={"tb-btn " + (homeActive ? 'active' : '')} onClick={onHome} title="Home"
             data-ipoint="shell.home" data-ipoint-layer="shell">
          <Icon name="home" size={16} />
        </div>
      </div>

      {/* Identity + sub-tabs */}
      <div className="flex items-center gap-1 px-2 flex-1 min-w-0 relative">
        <div data-ipoint="shell.appIdentity" data-ipoint-layer="shell" data-ipoint-side="bottom">
          <AppIdentity ext={ext} treatment={identityTreatment} onOpenSwitcher={onOpenSwitcher} switcherOpen={switcherOpen} />
        </div>
        {switcherOpen && (
          <AppSwitcher onPick={onOpenSwitcher} />
        )}
        {subtabs && subtabs.length > 0 && (
          <>
            <div className="v-divider" />
            <div className="flex items-center gap-0 min-w-0" style={{ height: '100%', overflow: 'hidden' }}
                 data-ipoint={ext ? "ext." + ext.id + ".subtabs" : "ext.subtabs"}>
              {subtabs.map(s => (
                <div key={s.id}
                     className={"subtab " + (s.id === activeSubtab ? 'active' : '')}
                     onClick={() => onSubtab(s.id)}>
                  {s.label}
                </div>
              ))}
            </div>
          </>
        )}
      </div>

      {/* Right: extension actions, tray */}
      <div className="flex items-center gap-1 px-2 flex-shrink-0">
        {actions && <div data-ipoint={ext ? "ext." + ext.id + ".topbarActions" : "ext.topbarActions"} data-ipoint-side="bottom" style={{ display: 'flex', gap: 4, alignItems: 'center' }}>{actions}</div>}
        {actions && <div className="v-divider" />}

        {/* Extension-provided tray icons — live status + media player */}
        <div className="relative" data-ipoint="ext.plinthcore.tray" data-ipoint-side="bottom">
          <StatusHealthIcon open={trays.statusOpen} onClick={() => onTraySet('status')} />
          {trays.statusOpen && (
            <StatusHealthTray width={Math.max(300, trayWidth - 40)} onClose={() => onTraySet(null)} />
          )}
        </div>
        <div className="relative" data-ipoint="ext.media.tray" data-ipoint-side="bottom">
          <MediaTrayIcon open={trays.mediaOpen} onClick={() => onTraySet('media')} playing={trays.mediaPlaying} />
          {trays.mediaOpen && (
            <MediaTrayPopover width={trayWidth} playing={trays.mediaPlaying}
              onTogglePlay={onToggleMediaPlay} onClose={() => onTraySet(null)} />
          )}
        </div>

        {/* Separator — extension tray icons vs shell-owned (bell + avatar) */}
        <div style={{ width: 1, height: 18, background: 'var(--border)', margin: '0 4px', alignSelf: 'center' }} />

        {/* Bell */}
        <div className="relative" data-ipoint="shell.notifications" data-ipoint-layer="shell" data-ipoint-side="bottom">
          <div className={"tray-icon " + (trays.bellOpen ? 'open' : '')}
               onClick={() => onTraySet('bell')}>
            <Icon name="bell" size={16} />
            {trays.unread > 0 && <span className="badge">{trays.unread}</span>}
          </div>
          {trays.bellOpen && (
            <NotificationsTray width={trayWidth} onClose={() => onTraySet(null)} />
          )}
        </div>

        {/* Avatar */}
        <div className="relative" data-ipoint="shell.identity" data-ipoint-layer="shell" data-ipoint-side="bottom">
          <div className={"tb-btn " + (trays.avatarOpen ? 'active' : '')}
               onClick={() => onTraySet('avatar')}
               style={{ paddingLeft: 6, paddingRight: 6 }}>
            <div style={{ width: 22, height: 22, borderRadius: 4, background: '#3b82f6', color: 'white',
                          display: 'inline-flex', alignItems: 'center', justifyContent: 'center',
                          fontFamily: 'JetBrains Mono, monospace', fontSize: 11, fontWeight: 700 }}>RL</div>
            <Icon name="chev-down" size={11} style={{ color: 'var(--text-3)' }} />
          </div>
          {trays.avatarOpen && (
            trays.fallback
              ? <AvatarFallbackTray width={trayWidth} onClose={() => onTraySet(null)} />
              : <AvatarTray width={trayWidth} onClose={() => onTraySet(null)} />
          )}
        </div>
      </div>
    </div>
  );
};

const AppSwitcher = ({ onPick }) => {
  const items = [
    { id: 'notes', name: 'Notes', cap: 'notes.open' },
    { id: 'homecare', name: 'Homecare', cap: 'homecare.open' },
    { id: 'admin', name: 'Admin', cap: 'admin.open' },
    { id: 'chat', name: 'Chat', cap: 'chat.open' },
    { id: 'files', name: 'Files', cap: 'files.open' },
  ];
  return (
    <div className="popover" style={{ left: 0, width: 260, top: 'calc(100% + 4px)' }}>
      <div className="px-3 py-2 hair text-3" style={{ fontSize: 11, letterSpacing: '0.08em', textTransform: 'uppercase' }}>Switch extension</div>
      {items.map(it => (
        <div key={it.id} className="hover-bg flex items-center gap-3 px-3 py-2" style={{ cursor: 'pointer' }} onClick={() => onPick(it.id)}>
          <span style={{ width: 16, height: 16, borderRadius: 4, background: EXT_COLORS[it.id] }} />
          <span className="text-0" style={{ fontWeight: 500 }}>{it.name}</span>
          <span className="mono text-3 ml-auto" style={{ fontSize: 11 }}>{it.cap}</span>
        </div>
      ))}
      <div className="hair-t px-3 py-2 text-3" style={{ fontSize: 11 }}>Go to Home for the full launcher</div>
    </div>
  );
};

Object.assign(window, { Topbar, EXT_COLORS, AppIdentity, AppSwitcher });
