// Package httpclient wraps /api/auth/login, /api/packages install/uninstall,
// and package-list lookups. All calls are session-cookie-based; a dedicated
// cookie jar is built per Client so concurrent Clients do not share auth.
package httpclient

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"mime/multipart"
	"net/http"
	"net/http/cookiejar"
	"net/url"
	"os"
	"path/filepath"
	"time"
)

// Client targets a single plinth kernel base URL (e.g. http://localhost:8080).
type Client struct {
	Base       string
	http       *http.Client
	SessionTok string // raw session token (plaintext); set after Login.
}

// New returns a fresh Client with its own cookie jar.
func New(base string) (*Client, error) {
	jar, err := cookiejar.New(nil)
	if err != nil {
		return nil, err
	}
	return &Client{
		Base: base,
		http: &http.Client{
			Jar:     jar,
			Timeout: 30 * time.Second,
		},
	}, nil
}

// Login POSTs /api/auth/login and captures the session cookie. The raw
// session token is extracted from the `plinth_session` Set-Cookie value
// so WS auth can reuse it.
func (c *Client) Login(username, password string) error {
	body, _ := json.Marshal(map[string]string{
		"username": username,
		"password": password,
	})
	req, err := http.NewRequest(http.MethodPost, c.Base+"/api/auth/login",
		bytes.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	resp, err := c.http.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		bodyBytes, _ := io.ReadAll(resp.Body)
		return fmt.Errorf("login: status=%d body=%s", resp.StatusCode,
			string(bodyBytes))
	}
	u, _ := url.Parse(c.Base)
	for _, ck := range c.http.Jar.Cookies(u) {
		if ck.Name == "plinth_session" {
			c.SessionTok = ck.Value
			return nil
		}
	}
	return fmt.Errorf("login: no plinth_session cookie in response")
}

// InstallPackage POSTs a .zip as multipart/form-data field `package`.
// Returns the parsed JSON response (expected to include `id`, `state`).
func (c *Client) InstallPackage(zipPath string) (map[string]any, error) {
	f, err := os.Open(zipPath)
	if err != nil {
		return nil, err
	}
	defer f.Close()

	var buf bytes.Buffer
	w := multipart.NewWriter(&buf)
	part, err := w.CreateFormFile("package", filepath.Base(zipPath))
	if err != nil {
		return nil, err
	}
	if _, err := io.Copy(part, f); err != nil {
		return nil, err
	}
	if err := w.Close(); err != nil {
		return nil, err
	}

	req, err := http.NewRequest(http.MethodPost, c.Base+"/api/packages",
		&buf)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", w.FormDataContentType())
	resp, err := c.http.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	var parsed map[string]any
	bodyBytes, _ := io.ReadAll(resp.Body)
	_ = json.Unmarshal(bodyBytes, &parsed)
	if resp.StatusCode != http.StatusCreated && resp.StatusCode != http.StatusOK {
		return parsed, fmt.Errorf("install: status=%d body=%s",
			resp.StatusCode, string(bodyBytes))
	}
	return parsed, nil
}

// UninstallPackage sends DELETE /api/packages/{id}?confirm=true. Non-2xx
// returns an error with the response body included.
func (c *Client) UninstallPackage(id string) error {
	req, err := http.NewRequest(http.MethodDelete,
		c.Base+"/api/packages/"+id+"?confirm=true", nil)
	if err != nil {
		return err
	}
	resp, err := c.http.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode >= 300 {
		bodyBytes, _ := io.ReadAll(resp.Body)
		return fmt.Errorf("uninstall: status=%d body=%s",
			resp.StatusCode, string(bodyBytes))
	}
	return nil
}

// ListPackages fetches /api/packages and returns the parsed body.
func (c *Client) ListPackages() ([]map[string]any, error) {
	req, err := http.NewRequest(http.MethodGet, c.Base+"/api/packages", nil)
	if err != nil {
		return nil, err
	}
	resp, err := c.http.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		bodyBytes, _ := io.ReadAll(resp.Body)
		return nil, fmt.Errorf("list: status=%d body=%s",
			resp.StatusCode, string(bodyBytes))
	}
	var out struct {
		Packages []map[string]any `json:"packages"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
		return nil, err
	}
	return out.Packages, nil
}

// ListGroups fetches /api/groups and returns the parsed body. Response
// shape is {"groups":[...]} per kernel/groups/handlers.cpp; each entry
// carries at least `id` and `name`.
func (c *Client) ListGroups() ([]map[string]any, error) {
	req, err := http.NewRequest(http.MethodGet, c.Base+"/api/groups", nil)
	if err != nil {
		return nil, err
	}
	resp, err := c.http.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		bodyBytes, _ := io.ReadAll(resp.Body)
		return nil, fmt.Errorf("groups list: status=%d body=%s",
			resp.StatusCode, string(bodyBytes))
	}
	var out struct {
		Groups []map[string]any `json:"groups"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
		return nil, err
	}
	return out.Groups, nil
}

// FindGroupByName returns the UUID of the group with the given name,
// or an error if no such group exists. The built-in admin group is
// seeded with name "admin".
func (c *Client) FindGroupByName(name string) (string, error) {
	groups, err := c.ListGroups()
	if err != nil {
		return "", err
	}
	for _, g := range groups {
		if n, _ := g["name"].(string); n == name {
			if id, _ := g["id"].(string); id != "" {
				return id, nil
			}
			return "", fmt.Errorf("group %q has no id field", name)
		}
	}
	return "", fmt.Errorf("group %q not found", name)
}

// QueryAuditCount returns the count of audit_log rows matching
// `action` (exact match). The caller is responsible for ensuring
// the DB baseline is zero for the action of interest before the run
// starts (LH-2 relies on schema reset via `--dev`). `sinceTs` is
// ignored today because the endpoint's `start=<iso8601>` parameter
// trips a PG protocol error under drogon's async binder — see
// kernel/audit/handlers.cpp SELECT_SQL + CHANGELOG LH-2 entry for
// the deviation. Safe to pass the zero value.
//
// Requires the caller to hold `kernel.admin` (the endpoint's
// registered rbac rule per kernel/audit/handlers.cpp:291). Intended
// for LH-2 teardown sampling of realtime.broker.* audit counts.
func (c *Client) QueryAuditCount(action string, _ time.Time) (int, error) {
	q := url.Values{}
	q.Set("action", action)
	q.Set("limit", "1")
	req, err := http.NewRequest(http.MethodGet,
		c.Base+"/api/audit?"+q.Encode(), nil)
	if err != nil {
		return 0, err
	}
	resp, err := c.http.Do(req)
	if err != nil {
		return 0, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		bodyBytes, _ := io.ReadAll(resp.Body)
		return 0, fmt.Errorf("audit query: status=%d body=%s",
			resp.StatusCode, string(bodyBytes))
	}
	var out struct {
		Total int `json:"total"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
		return 0, err
	}
	return out.Total, nil
}

// GrantRule POSTs /api/groups/{groupID}/rules with {"rule": rule}.
// Treats 409 (rule already granted) as success for idempotency under
// repeated harness setups; any other non-2xx status returns an error
// with the body attached.
func (c *Client) GrantRule(groupID, rule string) error {
	body, _ := json.Marshal(map[string]string{"rule": rule})
	req, err := http.NewRequest(http.MethodPost,
		c.Base+"/api/groups/"+groupID+"/rules",
		bytes.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	resp, err := c.http.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode >= 200 && resp.StatusCode < 300 {
		return nil
	}
	bodyBytes, _ := io.ReadAll(resp.Body)
	if resp.StatusCode == http.StatusConflict {
		return nil // already granted — idempotent
	}
	return fmt.Errorf("grant rule %q to group %s: status=%d body=%s",
		rule, groupID, resp.StatusCode, string(bodyBytes))
}
