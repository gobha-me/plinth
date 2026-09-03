// lh2sidecar entry point. The two capability handlers
// (install_subscription, read_counters) live under server/handlers/
// and share state via globalThis — same JSContext per BridgeContext
// slot, so globalThis is effectively a per-extension singleton.
export default function main() { return null; }
