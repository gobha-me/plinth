export default async function burst(args) {
  const count = (args && typeof args.count === "number") ? args.count : 1;
  const bytes = (args && typeof args.bytes === "number") ? args.bytes : 512;

  if (bytes > 7800) {
    throw new Error("lh1storm.burst: bytes must be <= 7800 (envelope overhead reserves headroom under the 8000-byte pubsub.publish ceiling)");
  }

  const data = "x".repeat(bytes);
  const promises = [];
  for (let i = 0; i < count; i++) {
    promises.push(pubsub.publish(
      "plinth:ext:lh1storm:stormevent",
      { seq: i, data: data, emit_started_at: Date.now() }
    ));
  }
  await Promise.all(promises);
  return { emitted: count };
}
