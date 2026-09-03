// Package pglisten implements the external PG LISTEN subscriber fleet
// for the LH-1 storm tier (ICD-LH-1 §5).
//
// Each Subscriber owns its own pq.Listener on channel "plinth:realtime"
// (the single-channel fan-in per ICD-0.5.0). The Run loop parses each
// envelope's JSON, extracts payload.seq + payload.emit_started_at, and
// hands one observation per notify to the shared SubscriberRecorder.
//
// Reconnection: pq.Listener auto-reconnects on disconnect; the event
// callback logs ListenerEventReconnected to stderr but a reconnect is
// NOT treated as a notify loss (loss is signalled by gap detection
// against the producer-side emitted total in the recorder summary).
// Under the default 120s storm tier a reconnect is itself a secondary
// signal worth investigating (ICD §7.3).
package pglisten

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"time"

	"github.com/lib/pq"

	"github.com/gobha-me/plinth/load-harness/internal/observe"
)

const (
	channel          = "plinth:realtime"
	minReconnect     = 1 * time.Second
	maxReconnect     = 10 * time.Second
	pingIntervalSecs = 90 // pq.Listener.Ping cadence is internal; we rely on its default
)

// Subscriber wraps one pq.Listener + an ID used to tag observations.
type Subscriber struct {
	id       int
	listener *pq.Listener
	rec      *observe.SubscriberRecorder
}

// New opens a connection, starts a pq.Listener against the DSN, and
// subscribes to "plinth:realtime". The returned Subscriber is ready
// for Run(ctx); Close unsubscribes and releases the underlying
// connection.
func New(id int, dsn string, rec *observe.SubscriberRecorder) (*Subscriber, error) {
	eventCallback := func(ev pq.ListenerEventType, err error) {
		// Informational: reconnects are not lost-notify signals per se,
		// but they are secondary signals worth noticing (ICD §7.3).
		switch ev {
		case pq.ListenerEventConnected:
			log.Printf("pglisten sub=%d event=connected", id)
		case pq.ListenerEventDisconnected:
			log.Printf("pglisten sub=%d event=disconnected err=%v", id, err)
		case pq.ListenerEventReconnected:
			log.Printf("pglisten sub=%d event=reconnected", id)
		case pq.ListenerEventConnectionAttemptFailed:
			log.Printf("pglisten sub=%d event=attempt_failed err=%v", id, err)
		}
	}

	listener := pq.NewListener(dsn, minReconnect, maxReconnect, eventCallback)
	if err := listener.Listen(channel); err != nil {
		_ = listener.Close()
		return nil, fmt.Errorf("pglisten sub=%d LISTEN %q: %w", id, channel, err)
	}
	return &Subscriber{id: id, listener: listener, rec: rec}, nil
}

// Run blocks until ctx is cancelled, draining notifications into the
// recorder. Envelope parse errors increment the recorder's parse-error
// count but do not terminate the loop — a malformed envelope is itself
// a diagnostic signal (ICD §6.3 exit code 1) rather than a fatal
// harness error.
func (s *Subscriber) Run(ctx context.Context) {
	for {
		select {
		case <-ctx.Done():
			return
		case n, ok := <-s.listener.Notify:
			if !ok {
				return // listener closed
			}
			if n == nil {
				// pq.Listener delivers a nil on reconnect — the caller
				// is expected to resync; storm tier has no replay
				// protocol, so we just wait for the next real notify.
				continue
			}
			s.observe(n)
		}
	}
}

// observe parses one notification and hands it off to the recorder.
func (s *Subscriber) observe(n *pq.Notification) {
	received := time.Now().UnixMilli()

	// QuickJS's JSON serializer writes integer-valued JS numbers with a
	// trailing `.0` when they exceed int32 (Date.now() is ~1.7e12), so
	// json.Unmarshal into int64 rejects them. Accept both via float64
	// and truncate to int64 at the receiver — lossless for values under
	// 2^53 (Number.MAX_SAFE_INTEGER), which covers every realistic
	// Date.now() and harness seq.
	var envelope struct {
		Layer   string `json:"layer"`
		Channel string `json:"channel"`
		Payload struct {
			Seq           float64 `json:"seq"`
			EmitStartedAt float64 `json:"emit_started_at"`
		} `json:"payload"`
	}
	if err := json.Unmarshal([]byte(n.Extra), &envelope); err != nil {
		log.Printf("pglisten sub=%d parse error: %v (extra=%q)",
			s.id, err, trunc(n.Extra, 200))
		s.rec.ParseError()
		return
	}

	s.rec.Observe(observe.SubscriberObs{
		SubscriberID:   s.id,
		Channel:        envelope.Channel,
		Seq:            int64(envelope.Payload.Seq),
		EmitStartedAt:  int64(envelope.Payload.EmitStartedAt),
		ReceivedAtUnix: received,
	})
}

// Close shuts down the pq.Listener. Safe to call multiple times (pq
// itself is idempotent on Close).
func (s *Subscriber) Close() error {
	return s.listener.Close()
}

func trunc(v string, n int) string {
	if len(v) > n {
		return v[:n] + "...(truncated)"
	}
	return v
}
