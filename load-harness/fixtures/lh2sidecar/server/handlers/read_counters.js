// lh2sidecar:1:read_counters() — surface the sidecar's accumulated
// observation state so the harness can fold it into its summary.
// Empty state (install_subscription never called) is a valid reply,
// not an error — the harness interprets observed=0 as "sidecar arm
// disabled" when --js-subscribers=0.
export default async function read_counters(_args) {
  const s = globalThis.__lh2sidecar_state;
  if (!s) {
    return { observed: 0, lags: [], channel: null };
  }
  return { observed: s.observed, lags: s.lags, channel: s.channel };
}
