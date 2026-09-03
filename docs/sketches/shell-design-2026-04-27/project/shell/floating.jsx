// Floating windows — draggable, resizable (corner), focusable panels that extensions
// open as detached surfaces. Shell owns the chrome; extension owns the body.
// The design doc calls these "detached panels" — think macOS Inspector windows.

const FloatingWindow = ({
  id, title, ext, capability,
  x, y, w, h,
  z, onFocus, onMove, onClose,
  children,
}) => {
  const extColor = window.EXT_COLORS?.[ext] || 'var(--accent)';

  const startDrag = (e) => {
    if (e.target.closest('.fw-close, .fw-action')) return;
    onFocus();
    const startX = e.clientX, startY = e.clientY;
    const ox = x, oy = y;
    const move = (ev) => {
      const nx = Math.max(8, ox + (ev.clientX - startX));
      const ny = Math.max(8, oy + (ev.clientY - startY));
      onMove({ x: nx, y: ny });
    };
    const up = () => {
      window.removeEventListener('mousemove', move);
      window.removeEventListener('mouseup', up);
    };
    window.addEventListener('mousemove', move);
    window.addEventListener('mouseup', up);
  };

  return (
    <div className="floating-window"
         data-ipoint={"shell.floatingWindow:" + id} data-ipoint-layer="shell"
         style={{
           position: 'absolute', left: x, top: y, width: w, height: h, zIndex: z,
           background: 'var(--bg-1)',
           border: '1px solid var(--border-strong)',
           borderRadius: 7,
           boxShadow: '0 24px 56px rgba(0,0,0,0.45), 0 8px 18px rgba(0,0,0,0.22)',
           display: 'flex', flexDirection: 'column',
           overflow: 'hidden',
         }}
         onMouseDown={onFocus}>

      {/* Title bar — shell-owned */}
      <div onMouseDown={startDrag}
           className="flex items-center gap-2 px-3"
           style={{ height: 30, background: 'var(--bg-2)', borderBottom: '1px solid var(--border)',
                    cursor: 'move', userSelect: 'none', flexShrink: 0 }}>
        <span style={{ width: 10, height: 10, borderRadius: 2, background: extColor, flexShrink: 0 }} />
        <span className="text-0" style={{ fontSize: 12, fontWeight: 600 }}>{title}</span>
        <span className="mono text-3" style={{ fontSize: 10 }}>{ext}</span>
        <div className="ml-auto flex items-center gap-1">
          <span className="mono text-3" style={{ fontSize: 10 }}>{capability}</span>
          <button className="fw-action tb-btn" style={{ padding: 2, marginLeft: 4 }} title="Dock back into app">
            <Icon name="arrow-left" size={11} />
          </button>
          <button className="fw-close tb-btn" onClick={onClose} style={{ padding: 2 }} title="Close">
            <Icon name="x" size={12} />
          </button>
        </div>
      </div>

      {/* Body — extension-owned */}
      <div data-ipoint={"ext." + ext + ".floatingBody:" + id}
           style={{ flex: 1, minHeight: 0, overflow: 'auto' }}>
        {children}
      </div>
    </div>
  );
};

// ------------------------------------------------------------------
// Manager — hooks to runtime state of open windows.
const useFloatingWindows = () => {
  const [wins, setWins] = React.useState([]);
  const [topZ, setTopZ] = React.useState(100);

  const open = (spec) => {
    setWins(cur => {
      const exists = cur.find(w => w.id === spec.id);
      if (exists) return cur.map(w => w.id === spec.id ? { ...w, z: topZ + 1 } : w);
      return [...cur, { ...spec, z: topZ + 1 }];
    });
    setTopZ(z => z + 1);
  };
  const close = (id) => setWins(cur => cur.filter(w => w.id !== id));
  const focus = (id) => {
    setTopZ(z => z + 1);
    setWins(cur => cur.map(w => w.id === id ? { ...w, z: topZ + 1 } : w));
  };
  const move = (id, pos) => setWins(cur => cur.map(w => w.id === id ? { ...w, ...pos } : w));
  return { wins, open, close, focus, move };
};

// ------------------------------------------------------------------
// Concrete window bodies

const FloatingClientCard = () => (
  <div style={{ padding: 16, fontSize: 12.5 }}>
    <div className="flex items-center gap-3 mb-3">
      <span style={{ width: 40, height: 40, borderRadius: 6, background: 'var(--bg-3)', color: 'var(--text-1)',
                     display: 'inline-flex', alignItems: 'center', justifyContent: 'center',
                     fontFamily: 'JetBrains Mono', fontWeight: 700 }}>AP</span>
      <div>
        <div className="text-0" style={{ fontSize: 14, fontWeight: 600 }}>Patel, Anaya</div>
        <div className="mono text-3" style={{ fontSize: 11 }}>C-0014 · aka Mrs. Patel</div>
      </div>
    </div>

    <div style={{ display: 'grid', gridTemplateColumns: '90px 1fr', rowGap: 6, columnGap: 12, marginBottom: 12 }}>
      <span className="text-3">Primary</span><span className="text-1">Alex Example</span>
      <span className="text-3">Group</span><span className="text-1">Example family</span>
      <span className="text-3">Meds</span><span className="text-1">4 active · Donepezil, Metformin, Lisinopril, Atorvastatin</span>
      <span className="text-3">Last visit</span><span className="text-1">08:14 today — logged by @alex</span>
    </div>

    <div className="hair-t" style={{ margin: '12px 0 10px' }} />
    <div className="text-3 mono" style={{ fontSize: 10.5, textTransform: 'uppercase', letterSpacing: '0.08em', marginBottom: 6 }}>This week</div>
    <div style={{ display: 'flex', flexDirection: 'column', gap: 4 }}>
      {[
        ['Tue 14:00', 'Home visit · 45m'],
        ['Tue 14:30', 'Overlap — Mr. Kowalski', true],
        ['Thu 10:00', 'Home visit · 30m'],
      ].map(([t, label, warn], i) => (
        <div key={i} className="flex items-center gap-2" style={{ fontSize: 12 }}>
          <span className="mono text-2" style={{ width: 70 }}>{t}</span>
          <span className={warn ? '' : 'text-1'} style={{ color: warn ? 'var(--warn)' : undefined }}>{label}</span>
          {warn && <span className="pill warn ml-auto">conflict</span>}
        </div>
      ))}
    </div>

    <div className="hair-t" style={{ margin: '12px 0' }} />
    <div className="flex items-center gap-2">
      <button className="tb-btn" style={{ background: 'var(--accent-soft)', borderColor: 'var(--accent-soft)', color: 'var(--accent)' }}>Open full record</button>
      <button className="tb-btn">Log visit</button>
      <span className="mono text-3 ml-auto" style={{ fontSize: 10 }}>ext_homecare.client</span>
    </div>
  </div>
);

const FloatingMedsInspector = () => (
  <div style={{ padding: 14, fontSize: 12 }}>
    <div className="flex items-center gap-2 mb-2">
      <Icon name="pill" size={14} style={{ color: 'var(--text-3)' }} />
      <span className="text-0" style={{ fontSize: 13, fontWeight: 600 }}>Medication schedule</span>
      <span className="text-3 mono ml-auto" style={{ fontSize: 10 }}>Mrs. Patel</span>
    </div>
    <table className="tbl" style={{ fontSize: 11.5 }}>
      <thead>
        <tr>
          <th>Drug</th><th>Dose</th><th>Schedule</th><th style={{ width: 52 }}>Refills</th>
        </tr>
      </thead>
      <tbody>
        <tr><td className="text-0">Donepezil</td><td className="mono">5mg</td><td className="mono text-2">08:30 qd</td><td className="mono text-1" style={{ textAlign: 'right' }}>2</td></tr>
        <tr><td className="text-0">Metformin</td><td className="mono">500mg</td><td className="mono text-2">08:30, 20:00</td><td className="mono text-1" style={{ textAlign: 'right' }}>3</td></tr>
        <tr><td className="text-0">Lisinopril</td><td className="mono">10mg</td><td className="mono text-2">08:30 qd</td><td className="mono text-1" style={{ textAlign: 'right' }}>1</td></tr>
        <tr><td className="text-0">Atorvastatin</td><td className="mono">20mg</td><td className="mono text-2">22:00 qd</td><td className="mono text-1" style={{ textAlign: 'right' }}>0</td></tr>
      </tbody>
    </table>
    <div className="text-3 mono" style={{ fontSize: 10, marginTop: 10 }}>cap.call("homecare.meds.read") · 18ms</div>
  </div>
);

Object.assign(window, {
  FloatingWindow, useFloatingWindows, FloatingClientCard, FloatingMedsInspector,
});
