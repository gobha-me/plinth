// Package wssub implements the WebSocket-subscriber fleet for the
// LH-2 ws-fanout tier (ICD-LH-2 §5.1). Each Subscriber owns a
// dedicated *websocket.Conn + runs its own receive loop — distinct
// from wsclient.Client's id-demuxed call/response pattern. The base
// wsclient reader goroutine discards event/subscribed frames (no id),
// so subscribers cannot share a connection with a caller.
//
// Per ICD-LH-2 §5.1, each subscriber:
//  1. Authenticates via POST /api/auth/login (caller-supplied session
//     token reused verbatim).
//  2. Dials /ws/events with the token per ICD-0.1.6 §WS auth.
//  3. Sends one {type:"subscribe",channels:[...]} frame; waits for
//     the ACK and fails fast if the channel is silent-omitted.
//  4. Enters a receive loop that records per-envelope observations
//     into a shared WsSubscriberRecorder.
//  5. On context cancel sends {type:"unsubscribe",...} + closes.
package wssub

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"net/url"
	"sync"
	"time"

	"github.com/gorilla/websocket"

	"github.com/gobha-me/plinth/load-harness/internal/observe"
)

// Subscriber wraps one /ws/events connection subscribed to a single
// channel. The receive loop owns conn; writes (subscribe,
// unsubscribe, pong) hold writeMu.
type Subscriber struct {
	id      int
	channel string
	conn    *websocket.Conn
	writeMu sync.Mutex
	rec     *observe.SubscriberRecorder
}

// Dial opens /ws/events with `sessionToken`, completes the ICD-0.1.6
// auth handshake (expects {"type":"connected"} back), sends the
// subscribe frame for `channel`, waits for the ACK, and returns a
// Subscriber ready for Run. Returns a non-nil error on dial / auth /
// subscribe-ack failure; the connection is torn down internally in
// that case.
func Dial(id int, httpBase, sessionToken, channel string,
	dialTimeout time.Duration,
	rec *observe.SubscriberRecorder) (*Subscriber, error) {

	u, err := url.Parse(httpBase)
	if err != nil {
		return nil, fmt.Errorf("wssub sub=%d parse base: %w", id, err)
	}
	scheme := "ws"
	if u.Scheme == "https" {
		scheme = "wss"
	}
	wsURL := scheme + "://" + u.Host + "/ws/events"

	d := websocket.Dialer{HandshakeTimeout: dialTimeout}
	conn, _, err := d.Dial(wsURL, http.Header{})
	if err != nil {
		return nil, fmt.Errorf("wssub sub=%d dial: %w", id, err)
	}

	_ = conn.SetReadDeadline(time.Now().Add(dialTimeout))
	if err := conn.WriteJSON(map[string]any{
		"type":  "auth",
		"token": sessionToken,
	}); err != nil {
		_ = conn.Close()
		return nil, fmt.Errorf("wssub sub=%d auth write: %w", id, err)
	}
	var connected map[string]any
	if err := conn.ReadJSON(&connected); err != nil {
		_ = conn.Close()
		return nil, fmt.Errorf("wssub sub=%d auth read: %w", id, err)
	}
	if connected["type"] != "connected" {
		_ = conn.Close()
		return nil, fmt.Errorf(
			"wssub sub=%d auth: expected connected, got %v",
			id, connected["type"])
	}

	if err := conn.WriteJSON(map[string]any{
		"type":     "subscribe",
		"channels": []string{channel},
	}); err != nil {
		_ = conn.Close()
		return nil, fmt.Errorf(
			"wssub sub=%d subscribe write: %w", id, err)
	}
	var ack map[string]any
	if err := conn.ReadJSON(&ack); err != nil {
		_ = conn.Close()
		return nil, fmt.Errorf(
			"wssub sub=%d subscribe ack: %w", id, err)
	}
	if ack["type"] != "subscribed" {
		_ = conn.Close()
		return nil, fmt.Errorf(
			"wssub sub=%d subscribe ack wrong type: %v",
			id, ack["type"])
	}
	if !ackIncludes(ack, channel) {
		_ = conn.Close()
		return nil, fmt.Errorf(
			"wssub sub=%d channel %q not in subscribed[]: %v",
			id, channel, ack["subscribed"])
	}
	_ = conn.SetReadDeadline(time.Time{})

	return &Subscriber{
		id:      id,
		channel: channel,
		conn:    conn,
		rec:     rec,
	}, nil
}

// ackIncludes returns true iff `channel` is present in the ACK
// frame's channels[] array (JSON array of strings). Kernel ACK
// shape per subscriptions.cpp:40 is {"type":"subscribed",
// "channels":[...]}; silent-omitted channels are simply absent.
func ackIncludes(ack map[string]any, channel string) bool {
	v, ok := ack["channels"].([]any)
	if !ok {
		return false
	}
	for _, e := range v {
		if s, ok := e.(string); ok && s == channel {
			return true
		}
	}
	return false
}

// Run blocks until the connection closes (typically from Close()
// called on ctx cancel). Each received frame feeds one observation
// through the recorder (event frames) or a pong reply (ping frames);
// non-event frames are skipped silently.
//
// Unlike a ticker-driven loop, this relies on ReadMessage errors —
// gorilla returns a permanent error the moment Close() runs on the
// connection, which unblocks the read. The external Close() caller
// is the shutdown signal; ctx is carried through solely to send the
// unsubscribe frame from the cancel-aware close path in
// runWorker-style callers.
func (s *Subscriber) Run(ctx context.Context) {
	_ = ctx // documentation; cancellation is observed via Close()
	for {
		msgType, data, err := s.conn.ReadMessage()
		if err != nil {
			// Normal close / peer close / reset — all end the loop.
			// gorilla's ReadMessage returns the first error and every
			// subsequent call returns the same error, so a single
			// return is sufficient.
			if !websocket.IsCloseError(err,
				websocket.CloseNormalClosure,
				websocket.CloseGoingAway,
				websocket.CloseNoStatusReceived) {
				log.Printf("wssub sub=%d read error: %v", s.id, err)
			}
			return
		}
		if msgType != websocket.TextMessage {
			continue
		}
		s.observe(data)
	}
}

// observe parses one frame and hands it to the recorder.
// Event frame shape per ICD-0.1.6: {type:"event", channel,
// payload:<envelope>}. Envelope.payload.seq +
// Envelope.payload.emit_started_at are the two fields the recorder
// needs for gap + lag computation.
func (s *Subscriber) observe(raw []byte) {
	received := time.Now().UnixMilli()

	var frame struct {
		Type      string  `json:"type"`
		Channel   string  `json:"channel"`
		Timestamp float64 `json:"timestamp"`
		Payload   struct {
			Payload struct {
				Seq           float64 `json:"seq"`
				EmitStartedAt float64 `json:"emit_started_at"`
			} `json:"payload"`
		} `json:"payload"`
	}
	if err := json.Unmarshal(raw, &frame); err != nil {
		s.rec.ParseError()
		return
	}
	if frame.Type == "ping" {
		// Echo the ping's timestamp — `on_pong_message` in
		// heartbeat.cpp matches on `timestamp == pending_ping_ts`; a
		// pong without the field is treated as stale and the next
		// heartbeat interval closes the conn with heartbeat_timeout.
		s.writeMu.Lock()
		_ = s.conn.WriteJSON(map[string]any{
			"type":      "pong",
			"timestamp": int64(frame.Timestamp),
		})
		s.writeMu.Unlock()
		return
	}
	if frame.Type != "event" {
		return
	}
	s.rec.Observe(observe.SubscriberObs{
		SubscriberID:   s.id,
		Channel:        frame.Channel,
		Seq:            int64(frame.Payload.Payload.Seq),
		EmitStartedAt:  int64(frame.Payload.Payload.EmitStartedAt),
		ReceivedAtUnix: received,
	})
}

// Close sends an unsubscribe frame (best effort) and tears down the
// WS connection. Safe to call multiple times; errors are logged,
// never returned.
func (s *Subscriber) Close() error {
	s.writeMu.Lock()
	_ = s.conn.WriteJSON(map[string]any{
		"type":     "unsubscribe",
		"channels": []string{s.channel},
	})
	_ = s.conn.WriteMessage(websocket.CloseMessage,
		websocket.FormatCloseMessage(websocket.CloseNormalClosure, ""))
	s.writeMu.Unlock()
	return s.conn.Close()
}

