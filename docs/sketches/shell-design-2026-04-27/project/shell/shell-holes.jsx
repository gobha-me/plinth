// Shell visual holes — the easy-to-miss, easy-to-break foundations.
//
// Lives as an overlay drawer so a package-coder session can invoke it
// mid-work without leaving the screen. Every item is something the shell
// currently gets right and a package regression would quickly destroy.

const SHELL_HOLES = [
  {
    id: 'density',
    title: 'Density & vertical rhythm',
    severity: 'high',
    trap: 'Packages render rows at web-app density (48–56px) instead of tool density (28–32px). One list at the wrong density and the whole shell reads like a SaaS dashboard, not an operator tool.',
    rule: 'Rows: 28–30px · Popover rows: 32–36px · Tiles: 118px min-h · Topbar: 48px fixed · Status bars: 24px',
    where: 'Homecare table, Admin packages table, Schedules table — all match. Diverge and the shell snaps out of character.',
  },
  {
    id: 'hairlines',
    title: 'Hairline borders, not cards',
    severity: 'high',
    trap: 'Extensions ship "card" components with rounded corners, drop shadows, and 16px gaps between them. The shell is a coherent surface; the moment you introduce floating card gutters the frame vocabulary collapses.',
    rule: '1px borders in --border, surfaces flush, shadows only on popovers & floating windows. No gap-cards.',
    where: 'Panels all border-meet. Only popovers + floating windows carry shadow.',
  },
  {
    id: 'monospace',
    title: 'Monospace is structural, not decorative',
    severity: 'high',
    trap: 'Using JetBrains Mono for copy because "it looks techy" — everywhere, reduces signal. Inter for prose, Mono strictly for IDs that users might paste or grep.',
    rule: 'Mono: capabilities, schema names, paths, package ids, timestamps, cron strings, client IDs. Inter: everything a human reads as sentence.',
    where: 'Tile footers, status bars, capability pills — mono. Headlines, descriptions, button labels — Inter.',
  },
  {
    id: 'color',
    title: 'Accent restraint',
    severity: 'high',
    trap: 'Extensions reach for their brand color to paint section headers, buttons, and row highlights. Three extensions with three accents and the frame reads like a theme park.',
    rule: 'Only the active-subtab underline, hyperlinks, focus rings, primary action buttons, and tiny identity swatches use --accent. Extension color lives in the 14–16px glyph and nowhere else.',
    where: 'Per-ext color lives in tile/row swatches only. Buttons use neutral chrome.',
  },
  {
    id: 'topbar-contract',
    title: 'Topbar zones are contracts, not suggestions',
    severity: 'high',
    trap: 'Extension stuffs a search bar into the topbar, or re-renders the home button, or puts its identity twice (once as app-identity, once as a breadcrumb). The topbar has three zones with three owners and that invariant is load-bearing.',
    rule: 'Shell owns: home, app-identity, bell, avatar. Extension owns: subtabs, right-side actions (3 max).',
    where: 'Flip "Highlight integration points" to see the contract lines.',
  },
  {
    id: 'tray-shape',
    title: 'Tray popovers ≠ modals',
    severity: 'med',
    trap: 'Notification or avatar popovers grow to 480–560px wide "because there\'s content" and start resembling Slack\'s right rail. They become modal panels with scroll-within-scroll and the shell feels bottom-heavy.',
    rule: 'Trays: 300–400px wide. Scroll inside at 420px max-height. Arrow on popover top-edge. Trays fade, not animate.',
    where: 'NotificationsTray + AvatarTray. Slider exposes the range.',
  },
  {
    id: 'floating-chrome',
    title: 'Floating windows must wear shell chrome',
    severity: 'med',
    trap: 'Extension ships its own title bar because it "wants control". Inconsistent close buttons, different drag targets, no capability shown. User loses trust.',
    rule: 'Shell provides title bar (ext swatch + title + cap + dock/close). Extension provides only the body. Same shadow, same border radius, same 30px bar.',
    where: 'Open a floating window from the Tweaks panel or Homecare Detach.',
  },
  {
    id: 'fallback',
    title: 'chrome_essential fallback must look deliberate',
    severity: 'med',
    trap: 'When a panel crashes, the shell falls back to a minimal render. If the fallback looks like an error page (gray, apologetic), trust collapses. If it looks deliberate (same vocabulary, labelled "fallback"), users keep going.',
    rule: 'Fallback uses the same popover/window chrome. Add a single pill labeled "fallback" in the corner. Keep at least the sign-out action.',
    where: 'Tweaks → "Avatar tray: chrome_essential fallback" to see the pattern.',
  },
  {
    id: 'scale-policy',
    title: 'UI scale is host-level, not app-level',
    severity: 'med',
    trap: 'Each extension wires its own zoom/font-size slider. User sets one to 125% and the others to 100% and the frame becomes visually incoherent.',
    rule: 'Scale is a shell-owned CSS zoom on the root. Extensions never override root font-size, viewport units, or px at the frame level.',
    where: 'Tweaks → UI scale · 75%–175%.',
  },
  {
    id: 'light-mode',
    title: 'Light mode is a first-class theme',
    severity: 'med',
    trap: 'Package ships dark-only, or treats light as "just invert the palette". Light uses a warm off-white, softer borders, less contrast on accents.',
    rule: 'Every color is a CSS variable. No hardcoded #fff, no hardcoded #000. Hand-tune light; never invert.',
    where: 'Flip Tweaks → Theme · Light. Both modes use the same component tree.',
  },
  {
    id: 'empty-states',
    title: 'Empty states carry the voice',
    severity: 'low',
    trap: 'Illustrations, "Nothing here yet!" with exclamation marks, a big "Add your first…" button. The shell voice is operator-quiet.',
    rule: 'Empty states are one mono sentence pointing at the next action. No art. No enthusiasm.',
    where: 'Capability-audit sidebar when an extension provides no rules.',
  },
  {
    id: 'schedule-copy',
    title: 'Cadence copy: human-first, cron-exposed',
    severity: 'low',
    trap: 'Admin UIs show raw cron strings as the primary label and force users to decode "*/15 * * * *" at a glance. Or hide the cron entirely and users lose trust they can set edge cases.',
    rule: 'Two lines per row: natural-language primary, cron secondary in mono. Composer takes natural language and previews cron live.',
    where: 'Admin · Schedules — every cadence cell.',
  },
];

const ShellHolesOverlay = ({ onClose }) => {
  const sevColor = { high: 'var(--danger)', med: 'var(--warn)', low: 'var(--text-3)' };
  const sevBg    = { high: 'var(--danger-soft)', med: 'var(--warn-soft)', low: 'var(--bg-2)' };

  return (
    <div style={{ position: 'fixed', inset: 0, zIndex: 50, display: 'flex', justifyContent: 'flex-end',
                  background: 'rgba(0,0,0,0.55)', backdropFilter: 'blur(2px)' }}
         onClick={onClose}>
      <div onClick={(e) => e.stopPropagation()}
           style={{ width: 520, maxWidth: '96vw', height: '100%', background: 'var(--bg-1)',
                    borderLeft: '1px solid var(--border-strong)',
                    display: 'flex', flexDirection: 'column',
                    boxShadow: '-20px 0 40px rgba(0,0,0,0.4)' }}>
        {/* Header */}
        <div className="hair flex items-start gap-3 px-4 py-3">
          <Icon name="alert" size={16} style={{ color: 'var(--warn)', marginTop: 2 }} />
          <div className="flex-1 min-w-0">
            <div className="text-0" style={{ fontSize: 14, fontWeight: 600 }}>Shell visual holes</div>
            <div className="text-2" style={{ fontSize: 12, marginTop: 2, lineHeight: 1.5 }}>
              Load-bearing details a package coder can quietly break. Every item below is something the shell currently gets right — diverge and the whole frame snaps out of character.
            </div>
          </div>
          <button className="tb-btn" onClick={onClose} style={{ padding: 3 }}><Icon name="x" size={13} /></button>
        </div>

        {/* Legend */}
        <div className="hair px-4 py-2 flex items-center gap-4" style={{ background: 'var(--bg-2)', fontSize: 11 }}>
          <div className="flex items-center gap-2">
            <span style={{ width: 8, height: 8, borderRadius: 2, background: 'var(--danger)' }} />
            <span className="text-2">high · breaks look-and-feel immediately</span>
          </div>
          <div className="flex items-center gap-2">
            <span style={{ width: 8, height: 8, borderRadius: 2, background: 'var(--warn)' }} />
            <span className="text-2">med · erodes over time</span>
          </div>
          <div className="flex items-center gap-2">
            <span style={{ width: 8, height: 8, borderRadius: 2, background: 'var(--text-3)' }} />
            <span className="text-2">low · voice drift</span>
          </div>
        </div>

        {/* Items */}
        <div style={{ flex: 1, overflowY: 'auto' }}>
          {SHELL_HOLES.map((h, i) => (
            <div key={h.id} style={{ borderBottom: '1px solid var(--border-soft)', padding: '14px 16px' }}>
              <div className="flex items-center gap-2 mb-2">
                <span className="mono text-3" style={{ fontSize: 11, width: 22 }}>{String(i + 1).padStart(2, '0')}</span>
                <span className="text-0" style={{ fontSize: 13.5, fontWeight: 600, flex: 1 }}>{h.title}</span>
                <span className="pill" style={{ color: sevColor[h.severity], background: sevBg[h.severity], fontWeight: 500, textTransform: 'uppercase', fontSize: 10, letterSpacing: '0.06em' }}>{h.severity}</span>
              </div>
              <div style={{ paddingLeft: 30 }}>
                <div className="text-2" style={{ fontSize: 12.5, lineHeight: 1.55, marginBottom: 8 }}>{h.trap}</div>
                <div style={{ background: 'var(--bg-0)', border: '1px solid var(--border)', borderLeft: '2px solid var(--accent)',
                              padding: '7px 10px', borderRadius: 3, marginBottom: 8 }}>
                  <div className="mono text-3" style={{ fontSize: 10, textTransform: 'uppercase', letterSpacing: '0.06em', marginBottom: 3 }}>Rule</div>
                  <div className="text-1" style={{ fontSize: 12.5, lineHeight: 1.5 }}>{h.rule}</div>
                </div>
                <div className="mono text-3" style={{ fontSize: 11, lineHeight: 1.5 }}>
                  <span className="text-3">where · </span>{h.where}
                </div>
              </div>
            </div>
          ))}
        </div>

        <div className="hair-t px-4 py-2 mono text-3" style={{ fontSize: 10.5, background: 'var(--bg-2)' }}>
          shell-holes.md · maintained alongside DESIGN-shell-v06x.md
        </div>
      </div>
    </div>
  );
};

Object.assign(window, { ShellHolesOverlay, SHELL_HOLES });
