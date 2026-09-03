// lh2sidecar:1:install_subscription(channel) — subscribe the sidecar
// to `channel` and route each received envelope into a globalThis-
// backed counter + lag array the harness reads back via
// lh2sidecar:1:read_counters at teardown.
//
// `channel` is expected to be a cross-extension Layer-3 channel (e.g.
// plinth:ext:lh1storm:storm_event). The admin user running the
// harness must hold the derived per-channel rule
// (<other>.realtime.subscribe.<event_class>) per ICD-0.5.2 §SC6 —
// otherwise classify_pubsub_subscribe rejects with pubsub.rbac_denied.
export default async function install_subscription(args) {
  const channel = (args && typeof args.channel === "string")
    ? args.channel
    : null;
  if (!channel) {
    throw new Error("lh2sidecar.install_subscription: channel required");
  }

  // Initialize / reset shared state. A second install overwrites the
  // prior subscription (pubsub.subscribe last-writer-wins per ICD-0.5.2
  // §OQ3) — reset the counters too so the harness gets a clean window.
  if (globalThis.__lh2sidecar_state && globalThis.__lh2sidecar_state.unsub) {
    try { await globalThis.__lh2sidecar_state.unsub(); } catch (_) {}
  }
  globalThis.__lh2sidecar_state = {
    observed: 0,
    lags: [],
    channel: channel,
    unsub: null,
  };

  const state = globalThis.__lh2sidecar_state;
  const unsub = await pubsub.subscribe(channel, (envelope) => {
    const now = Date.now();
    const emit = (envelope && envelope.payload
                  && typeof envelope.payload.emit_started_at === "number")
      ? envelope.payload.emit_started_at
      : now;
    state.observed += 1;
    state.lags.push(now - emit);
  });
  state.unsub = unsub;

  return { ok: true, channel };
}
