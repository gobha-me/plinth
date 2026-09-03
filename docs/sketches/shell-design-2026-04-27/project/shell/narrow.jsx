// Narrow-viewport demo — mirrors reference sketch state 6.
// Two framed examples showing: collapsed tray (bell + avatar only), sub-tabs
// horizontal-scroll inside content zone, popovers → bottom sheet.

const NarrowFrame = ({ children, label }) => (
  <div style={{ flex: 1, minWidth: 0 }}>
    <div className="text-3 mono" style={{ fontSize: 10.5, letterSpacing: '0.08em', textTransform: 'uppercase', marginBottom: 8 }}>{label}</div>
    <div style={{ width: '100%', maxWidth: 380, border: '1px solid var(--border-strong)', borderRadius: 10,
                  overflow: 'hidden', background: 'var(--bg-0)' }}>{children}</div>
  </div>
);

const NarrowActive = () => (
  <NarrowFrame label="Active app · narrow">
    {/* Narrow topbar */}
    <div className="surface-1 flex items-stretch" style={{ height: 42, borderBottom: '1px solid var(--border)' }}>
      <div className="flex items-center px-1">
        <div className="tb-btn" style={{ height: 30, width: 30, justifyContent: 'center', padding: 0 }}><Icon name="home" size={14}/></div>
      </div>
      <div className="flex items-center gap-1 px-1 flex-1 min-w-0">
        <div className="appid-mark" style={{ height: 28, gap: 8, padding: '0 4px' }}>
          <span className="glyph" style={{ background: '#5aa9ff', width: 16, height: 16, fontSize: 9 }}>N</span>
          <span style={{ fontSize: 13 }}>Notes</span>
          <Icon name="chev-down" size={10} style={{ color: 'var(--text-3)' }} />
        </div>
      </div>
      <div className="flex items-center px-1 gap-1">
        <div className="tray-icon" style={{ width: 28, height: 28 }}><Icon name="bell" size={14}/></div>
        <div style={{ width: 22, height: 22, borderRadius: 4, background: '#3b82f6', color: 'white',
                      display: 'inline-flex', alignItems: 'center', justifyContent: 'center',
                      fontFamily: 'JetBrains Mono', fontSize: 10, fontWeight: 700, marginRight: 6 }}>RL</div>
      </div>
    </div>
    {/* Narrow sub-tabs: horizontal scroll inside content zone */}
    <div style={{ display: 'flex', gap: 4, overflowX: 'auto', padding: '8px 8px', borderBottom: '1px solid var(--border-soft)', background: 'var(--bg-1)' }}>
      {['Editor', 'Preview', 'Graph', 'Shared', 'Search', 'Settings'].map((l, i) => (
        <div key={l} className={"subtab " + (i === 0 ? 'active' : '')}
             style={{ height: 26, fontSize: 12, flexShrink: 0, padding: '0 10px' }}>{l}</div>
      ))}
    </div>
    {/* Narrow content */}
    <div className="px-4 py-4" style={{ minHeight: 160 }}>
      <div className="text-0" style={{ fontSize: 15, fontWeight: 600, marginBottom: 6 }}>Care binder / intake</div>
      <div className="text-2" style={{ fontSize: 12, lineHeight: 1.55 }}>
        Same panel component; shell adapts the container. Topbar: 42px · tray: bell + avatar only · sub-tabs: scroll-within-content.
      </div>
    </div>
  </NarrowFrame>
);

const NarrowBottomSheet = () => (
  <NarrowFrame label="Tray popover → bottom sheet">
    {/* Dim topbar */}
    <div style={{ opacity: 0.35 }}>
      <div className="surface-1 flex items-stretch" style={{ height: 42, borderBottom: '1px solid var(--border)' }}>
        <div className="flex items-center px-1">
          <div className="tb-btn" style={{ height: 30, width: 30, padding: 0, justifyContent: 'center' }}><Icon name="home" size={14}/></div>
        </div>
        <div className="flex items-center gap-1 px-1 flex-1 min-w-0">
          <div className="appid-mark" style={{ height: 28, gap: 8, padding: '0 4px' }}>
            <span className="glyph" style={{ background: '#5aa9ff', width: 16, height: 16, fontSize: 9 }}>N</span>
            <span style={{ fontSize: 13 }}>Notes</span>
          </div>
        </div>
      </div>
      <div style={{ height: 48, background: 'var(--bg-0)' }} />
    </div>

    {/* Bottom sheet */}
    <div style={{ background: 'var(--bg-1)', borderTop: '1px solid var(--border-strong)', padding: '10px 14px 14px',
                  borderRadius: '12px 12px 0 0' }}>
      <div style={{ width: 36, height: 4, background: 'var(--bg-3)', borderRadius: 2, margin: '0 auto 12px' }} />

      {/* Identity */}
      <div className="flex items-center gap-3" style={{ marginBottom: 10 }}>
        <div style={{ width: 32, height: 32, borderRadius: 6, background: '#3b82f6', color: 'white',
                      display: 'inline-flex', alignItems: 'center', justifyContent: 'center',
                      fontFamily: 'JetBrains Mono', fontWeight: 700, fontSize: 13 }}>RL</div>
        <div>
          <div className="text-0" style={{ fontSize: 13, fontWeight: 600 }}>Alex Example</div>
          <div className="text-3 mono" style={{ fontSize: 10.5 }}>@alex · admin</div>
        </div>
      </div>

      {/* Rows */}
      <div className="flex items-center gap-2" style={{ padding: '6px 0', borderTop: '1px solid var(--border-soft)' }}>
        <Icon name="sun" size={14} style={{ color: 'var(--text-3)' }} />
        <span className="text-1" style={{ fontSize: 12.5, flex: 1 }}>Theme</span>
        <div className="flex" style={{ background: 'var(--bg-0)', borderRadius: 5, padding: 2, gap: 2, border: '1px solid var(--border)' }}>
          {['Light','Dark','Auto'].map((t,i) => (
            <span key={t} className={i === 1 ? 'text-0' : 'text-3'}
                  style={{ padding: '2px 7px', fontSize: 10, fontWeight: 500, borderRadius: 3,
                           background: i === 1 ? 'var(--bg-2)' : 'transparent' }}>{t}</span>
          ))}
        </div>
      </div>
      <div className="flex items-center gap-2" style={{ padding: '7px 0', borderTop: '1px solid var(--border-soft)', cursor: 'pointer' }}>
        <Icon name="gear" size={14} style={{ color: 'var(--text-3)' }} />
        <span className="text-1" style={{ fontSize: 12.5, flex: 1 }}>User settings</span>
      </div>
      <div className="flex items-center gap-2" style={{ padding: '7px 0', borderTop: '1px solid var(--border-soft)', cursor: 'pointer', color: 'var(--danger)' }}>
        <Icon name="sign-out" size={14} />
        <span style={{ fontSize: 12.5 }}>Sign out</span>
      </div>
    </div>
  </NarrowFrame>
);

const NarrowViewportOverlay = ({ onClose }) => (
  <div style={{ position: 'fixed', inset: 0, zIndex: 50, display: 'flex',
                background: 'rgba(0,0,0,0.55)', backdropFilter: 'blur(2px)' }}
       onClick={onClose}>
    <div onClick={(e) => e.stopPropagation()}
         style={{ margin: 'auto', background: 'var(--bg-0)', border: '1px solid var(--border-strong)',
                  borderRadius: 12, padding: 28, maxWidth: 900, width: '90vw',
                  boxShadow: '0 30px 60px rgba(0,0,0,0.5)' }}>
      <div className="flex items-start justify-between" style={{ marginBottom: 6 }}>
        <div>
          <div className="text-0" style={{ fontSize: 16, fontWeight: 600 }}>Narrow viewport — mobile treatment</div>
          <div className="text-2" style={{ fontSize: 12.5, marginTop: 4, maxWidth: 640, lineHeight: 1.55 }}>
            Shell adapts the container, panel stays the same. Tray collapses to bell + avatar. Sub-tabs move into horizontal-scroll inside the content zone. Popovers become bottom sheets.
          </div>
        </div>
        <button className="tb-btn" onClick={onClose} style={{ padding: 3 }}><Icon name="x" size={13}/></button>
      </div>
      <div className="flex gap-5" style={{ marginTop: 20, alignItems: 'flex-start', flexWrap: 'wrap' }}>
        <NarrowActive />
        <NarrowBottomSheet />
      </div>
      <div className="mono text-3" style={{ fontSize: 10.5, marginTop: 18, textAlign: 'right' }}>
        shell/responsive · breakpoint 640px · same Preact panel, different container
      </div>
    </div>
  </div>
);

Object.assign(window, { NarrowViewportOverlay });
