// Package wsclient dials /ws/events, authenticates with a session token,
// and implements a correlation-ID-matched request/response call loop.
package wsclient

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/url"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

// Frame is a loose map mirroring the JSON message envelope used by the
// kernel WS protocol (see docs/icd/ICD-LH-0-load-harness-scaffold.md).
type Frame = map[string]any

// Client owns one /ws/events connection plus a single reader goroutine
// that demuxes frames by their "id" field.
type Client struct {
	conn    *websocket.Conn
	mu      sync.Mutex
	waiters map[string]chan Frame
	closed  chan struct{}
	writeMu sync.Mutex
}

// Dial opens the WS connection, sends {type:"auth",token:...}, waits for
// the server's {type:"connected"} frame, and starts the reader goroutine.
// httpBase looks like "http://host:port"; it is rewritten to ws://.
func Dial(httpBase, sessionToken string, timeout time.Duration) (*Client, error) {
	u, err := url.Parse(httpBase)
	if err != nil {
		return nil, err
	}
	scheme := "ws"
	if u.Scheme == "https" {
		scheme = "wss"
	}
	wsURL := scheme + "://" + u.Host + "/ws/events"

	d := websocket.Dialer{HandshakeTimeout: timeout}
	conn, _, err := d.Dial(wsURL, http.Header{})
	if err != nil {
		return nil, fmt.Errorf("ws dial: %w", err)
	}
	conn.SetReadDeadline(time.Now().Add(timeout))

	if err := conn.WriteJSON(Frame{"type": "auth", "token": sessionToken}); err != nil {
		conn.Close()
		return nil, fmt.Errorf("ws auth write: %w", err)
	}
	var connected Frame
	if err := conn.ReadJSON(&connected); err != nil {
		conn.Close()
		return nil, fmt.Errorf("ws auth read: %w", err)
	}
	if connected["type"] != "connected" {
		conn.Close()
		return nil, fmt.Errorf("ws auth: expected connected, got %v",
			connected["type"])
	}
	conn.SetReadDeadline(time.Time{})

	c := &Client{
		conn:    conn,
		waiters: make(map[string]chan Frame),
		closed:  make(chan struct{}),
	}
	go c.readLoop()
	return c, nil
}

func (c *Client) readLoop() {
	defer close(c.closed)
	for {
		var f Frame
		if err := c.conn.ReadJSON(&f); err != nil {
			return
		}
		// Server-initiated frames (ping) get a pong response on this
		// same connection. Everything else is demuxed by id.
		if f["type"] == "ping" {
			c.writeMu.Lock()
			_ = c.conn.WriteJSON(Frame{"type": "pong",
				"timestamp": f["timestamp"]})
			c.writeMu.Unlock()
			continue
		}
		id, _ := f["id"].(string)
		if id == "" {
			continue
		}
		c.mu.Lock()
		waiter, ok := c.waiters[id]
		if ok {
			delete(c.waiters, id)
		}
		c.mu.Unlock()
		if ok {
			waiter <- f
		}
	}
}

// Call issues a {type:"call",id,signature,args} frame and blocks on the
// matching call_result / call_error response up to timeout.
func (c *Client) Call(id, signature string, args any,
	timeout time.Duration) (Frame, error) {
	ch := make(chan Frame, 1)
	c.mu.Lock()
	c.waiters[id] = ch
	c.mu.Unlock()

	c.writeMu.Lock()
	err := c.conn.WriteJSON(Frame{
		"type":      "call",
		"id":        id,
		"signature": signature,
		"args":      args,
	})
	c.writeMu.Unlock()
	if err != nil {
		c.mu.Lock()
		delete(c.waiters, id)
		c.mu.Unlock()
		return nil, fmt.Errorf("ws write: %w", err)
	}

	select {
	case f := <-ch:
		return f, nil
	case <-time.After(timeout):
		c.mu.Lock()
		delete(c.waiters, id)
		c.mu.Unlock()
		return nil, fmt.Errorf("ws call %q timeout", id)
	case <-c.closed:
		return nil, fmt.Errorf("ws connection closed")
	}
}

// Close shuts the connection. Safe to call multiple times.
func (c *Client) Close() error {
	c.writeMu.Lock()
	_ = c.conn.WriteMessage(websocket.CloseMessage,
		websocket.FormatCloseMessage(websocket.CloseNormalClosure, ""))
	c.writeMu.Unlock()
	err := c.conn.Close()
	// Ensure readLoop has exited.
	<-c.closed
	return err
}

// Raw exposes the underlying frame envelope — useful for tests that need
// to assert wire shape beyond call_result/call_error.
func (c *Client) Raw() *websocket.Conn { return c.conn }

// Ensure the json package is referenced (reserved for future binary
// frame decoding; gorilla's WriteJSON/ReadJSON are used for now).
var _ = json.Marshal
