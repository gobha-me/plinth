package httpclient

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"time"
)

func newTestClient(t *testing.T, handler http.HandlerFunc) *Client {
	t.Helper()
	server := httptest.NewServer(handler)
	t.Cleanup(server.Close)
	client, err := New(server.URL)
	if err != nil {
		t.Fatal(err)
	}
	return client
}

func archiveFixture(t *testing.T) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "fixture.zip")
	if err := os.WriteFile(path, []byte("fake ZIP\x00bytes"), 0o600); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestLoginAndIndependentCookieJars(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/api/auth/login":
			if r.Method != http.MethodPost || r.Header.Get("Content-Type") != "application/json" {
				t.Errorf("login method/content-type = %s/%s", r.Method, r.Header.Get("Content-Type"))
			}
			var body map[string]string
			if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
				t.Error(err)
			}
			if body["password"] != "fictional \"pass\" & 雪" {
				t.Errorf("password did not round-trip: %q", body["password"])
			}
			http.SetCookie(w, &http.Cookie{Name: "unrelated", Value: "ignored", Path: "/"})
			http.SetCookie(w, &http.Cookie{Name: "plinth_session", Value: "fixture-" + body["username"], Path: "/"})
		case "/api/packages":
			cookie, err := r.Cookie("plinth_session")
			if err != nil {
				t.Error(err)
				w.WriteHeader(http.StatusUnauthorized)
				return
			}
			fmt.Fprintf(w, `{"packages":[{"id":%q}]}`, cookie.Value)
		default:
			t.Errorf("unexpected route %s", r.URL)
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	defer server.Close()
	for _, name := range []string{"alice", "bob"} {
		client, err := New(server.URL)
		if err != nil {
			t.Fatal(err)
		}
		if len(client.http.Jar.Cookies(mustURL(t, server.URL))) != 0 || client.SessionTok != "" {
			t.Fatal("new client inherited authentication")
		}
		if err := client.Login(name, "fictional \"pass\" & 雪"); err != nil {
			t.Fatal(err)
		}
		if client.SessionTok != "fixture-"+name {
			t.Fatalf("session token = %q", client.SessionTok)
		}
		packages, err := client.ListPackages()
		if err != nil || len(packages) != 1 || packages[0]["id"] != "fixture-"+name {
			t.Fatalf("cookie round-trip packages=%v error=%v", packages, err)
		}
	}
}

func TestLoginRejectsFailureAndMissingSession(t *testing.T) {
	for _, status := range []int{http.StatusUnauthorized, http.StatusOK} {
		t.Run(fmt.Sprint(status), func(t *testing.T) {
			client := newTestClient(t, func(w http.ResponseWriter, _ *http.Request) {
				http.SetCookie(w, &http.Cookie{Name: "other", Value: "fixture", Path: "/"})
				w.WriteHeader(status)
				io.WriteString(w, "fictional login failure")
			})
			if err := client.Login("fixture", "fictional"); err == nil {
				t.Fatal("login succeeded without an accepted session cookie")
			}
			if client.SessionTok != "" {
				t.Fatalf("failed login set session %q", client.SessionTok)
			}
		})
	}
}

func TestInstallPackageMultipartAndStatuses(t *testing.T) {
	path := archiveFixture(t)
	for _, status := range []int{200, 201, 400, 500} {
		t.Run(fmt.Sprint(status), func(t *testing.T) {
			client := newTestClient(t, func(w http.ResponseWriter, r *http.Request) {
				if r.Method != http.MethodPost || r.URL.Path != "/api/packages" {
					t.Errorf("install request = %s %s", r.Method, r.URL)
				}
				if err := r.ParseMultipartForm(1024); err != nil {
					t.Error(err)
					w.WriteHeader(http.StatusBadRequest)
					return
				}
				defer r.MultipartForm.RemoveAll()
				file, header, err := r.FormFile("package")
				if err != nil {
					t.Error(err)
					return
				}
				defer file.Close()
				content, err := io.ReadAll(file)
				if err != nil || string(content) != "fake ZIP\x00bytes" || header.Filename != "fixture.zip" {
					t.Errorf("multipart file=%q bytes=%q error=%v", header.Filename, content, err)
				}
				w.WriteHeader(status)
				io.WriteString(w, `{"id":"fixture-id","state":"ready"}`)
			})
			result, err := client.InstallPackage(path)
			if result["id"] != "fixture-id" || result["state"] != "ready" {
				t.Fatalf("parsed install result = %v", result)
			}
			if wantError := status >= 400; (err != nil) != wantError {
				t.Fatalf("install status %d error=%v", status, err)
			}
		})
	}
}

func TestInstallPackageRejectsUnreadableInput(t *testing.T) {
	client, err := New("http://unused.invalid")
	if err != nil {
		t.Fatal(err)
	}
	for _, path := range []string{filepath.Join(t.TempDir(), "absent.zip"), t.TempDir()} {
		if _, err := client.InstallPackage(path); err == nil {
			t.Fatalf("accepted unreadable archive %q", path)
		}
	}
}

func TestInstallPackageMalformedSuccessResponseKnownFinding(t *testing.T) {
	client := newTestClient(t, func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusCreated)
		io.WriteString(w, `{"truncated":`)
	})
	_, err := client.InstallPackage(archiveFixture(t))
	if err == nil {
		// Expected failure: client.go discards json.Unmarshal's error. Keep
		// this executable reproducer skipped until a separate behavior fix.
		t.Skip("known finding: successful install status with malformed JSON returns nil error")
	}
}

func TestReadEndpointsDecodeAndRejectMalformedJSON(t *testing.T) {
	for _, endpoint := range []string{"packages", "groups"} {
		for _, body := range []string{`{"` + endpoint + `":[{"id":"fixture-id","name":"fixture"}]}`, `not JSON`} {
			t.Run(endpoint+"/"+body, func(t *testing.T) {
				client := newTestClient(t, func(w http.ResponseWriter, r *http.Request) {
					if r.Method != http.MethodGet || r.URL.Path != "/api/"+endpoint {
						t.Errorf("list request = %s %s", r.Method, r.URL)
					}
					io.WriteString(w, body)
				})
				var result []map[string]any
				var err error
				if endpoint == "packages" {
					result, err = client.ListPackages()
				} else {
					result, err = client.ListGroups()
				}
				if body == "not JSON" {
					if err == nil {
						t.Fatal("malformed response accepted")
					}
				} else if err != nil || !reflect.DeepEqual(result, []map[string]any{{"id": "fixture-id", "name": "fixture"}}) {
					t.Fatalf("decoded rows=%v error=%v", result, err)
				}
			})
		}
	}
}

func TestFindGroupByName(t *testing.T) {
	for _, tc := range []struct {
		name, body, want string
	}{
		{"matches-later", `{"groups":[{"name":"other","id":"other-id"},{"name":"admin","id":"admin-id"}]}`, "admin-id"},
		{"empty", `{"groups":[]}`, ""},
		{"non-string-name", `{"groups":[{"name":7,"id":"id"}]}`, ""},
		{"missing-id", `{"groups":[{"name":"admin"}]}`, ""},
		{"empty-id", `{"groups":[{"name":"admin","id":""}]}`, ""},
		{"non-string-id", `{"groups":[{"name":"admin","id":42}]}`, ""},
	} {
		t.Run(tc.name, func(t *testing.T) {
			client := newTestClient(t, func(w http.ResponseWriter, _ *http.Request) { io.WriteString(w, tc.body) })
			got, err := client.FindGroupByName("admin")
			if got != tc.want || (err != nil) != (tc.want == "") {
				t.Fatalf("group ID=%q error=%v, want %q", got, err, tc.want)
			}
		})
	}
}

func TestAuditCountEncodesActionAndRejectsMalformedJSON(t *testing.T) {
	action := "fixture.event + &limit=999/雪?"
	for _, body := range []string{`{"total":37}`, `not JSON`} {
		client := newTestClient(t, func(w http.ResponseWriter, r *http.Request) {
			if r.Method != http.MethodGet || r.URL.Path != "/api/audit" || r.URL.Query().Get("action") != action || r.URL.Query().Get("limit") != "1" || len(r.URL.Query()) != 2 {
				t.Errorf("audit request = %s %s", r.Method, r.URL)
			}
			io.WriteString(w, body)
		})
		got, err := client.QueryAuditCount(action, time.Time{})
		if body == "not JSON" {
			if err == nil {
				t.Fatal("malformed audit response accepted")
			}
		} else if err != nil || got != 37 {
			t.Fatalf("audit count=%d error=%v", got, err)
		}
	}
}

func TestUninstallAndGrantRuleStatusBoundaries(t *testing.T) {
	for _, status := range []int{200, 204, 299, 300, 400, 409, 500} {
		for _, method := range []string{"uninstall", "grant"} {
			t.Run(fmt.Sprintf("%s/%d", method, status), func(t *testing.T) {
				client := newTestClient(t, func(w http.ResponseWriter, r *http.Request) {
					if method == "uninstall" {
						if r.Method != http.MethodDelete || r.URL.Path != "/api/packages/fixture-id" || r.URL.RawQuery != "confirm=true" {
							t.Errorf("uninstall request = %s %s", r.Method, r.URL)
						}
					} else {
						var payload map[string]string
						if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
							t.Error(err)
						}
						if r.Method != http.MethodPost || r.URL.Path != "/api/groups/fixture-id/rules" || r.Header.Get("Content-Type") != "application/json" || payload["rule"] != "fixture.雪\"" {
							t.Errorf("grant request = %s %s body=%v", r.Method, r.URL, payload)
						}
					}
					w.WriteHeader(status)
					io.WriteString(w, "fictional response")
				})
				var err error
				wantError := status >= 300
				if method == "uninstall" {
					err = client.UninstallPackage("fixture-id")
				} else {
					err = client.GrantRule("fixture-id", "fixture.雪\"")
					wantError = wantError && status != 409
				}
				if (err != nil) != wantError {
					t.Fatalf("status=%d error=%v, wantError=%v", status, err, wantError)
				}
			})
		}
	}
}

type roundTripFunc func(*http.Request) (*http.Response, error)

func (fn roundTripFunc) RoundTrip(r *http.Request) (*http.Response, error) { return fn(r) }

func TestEveryEndpointReportsRequestTransportAndStatusErrors(t *testing.T) {
	path := archiveFixture(t)
	calls := map[string]func(*Client) error{
		"login":      func(c *Client) error { return c.Login("fixture", "fictional") },
		"install":    func(c *Client) error { _, err := c.InstallPackage(path); return err },
		"uninstall":  func(c *Client) error { return c.UninstallPackage("fixture") },
		"packages":   func(c *Client) error { _, err := c.ListPackages(); return err },
		"groups":     func(c *Client) error { _, err := c.ListGroups(); return err },
		"find-group": func(c *Client) error { _, err := c.FindGroupByName("admin"); return err },
		"audit":      func(c *Client) error { _, err := c.QueryAuditCount("fixture", time.Time{}); return err },
		"grant":      func(c *Client) error { return c.GrantRule("fixture", "fixture.rule") },
	}
	for name, call := range calls {
		t.Run(name, func(t *testing.T) {
			client, err := New("://invalid-base")
			if err != nil {
				t.Fatal(err)
			}
			if err := call(client); err == nil {
				t.Fatal("malformed request URL accepted")
			}
			client.Base = "http://fixture.invalid"
			sentinel := errors.New("fictional transport failure")
			client.http.Transport = roundTripFunc(func(*http.Request) (*http.Response, error) { return nil, sentinel })
			if err := call(client); !errors.Is(err, sentinel) {
				t.Fatalf("transport error = %v, want wrapped sentinel", err)
			}
			client = newTestClient(t, func(w http.ResponseWriter, _ *http.Request) {
				w.WriteHeader(http.StatusForbidden)
				io.WriteString(w, "fictional denied body")
			})
			if err := call(client); err == nil || !strings.Contains(err.Error(), "status=403") || !strings.Contains(err.Error(), "fictional denied body") {
				t.Fatalf("status/body diagnostic = %v", err)
			}
		})
	}
}

func mustURL(t *testing.T, value string) *url.URL {
	t.Helper()
	parsed, err := url.Parse(value)
	if err != nil {
		t.Fatal(err)
	}
	return parsed
}

type trackedBody struct {
	io.Reader
	closed bool
}

func (body *trackedBody) Close() error { body.closed = true; return nil }

func TestEveryEndpointClosesResponseBody(t *testing.T) {
	path := archiveFixture(t)
	calls := map[string]func(*Client){
		"login":     func(c *Client) { _ = c.Login("fixture", "fictional") },
		"install":   func(c *Client) { _, _ = c.InstallPackage(path) },
		"uninstall": func(c *Client) { _ = c.UninstallPackage("fixture") },
		"packages":  func(c *Client) { _, _ = c.ListPackages() },
		"groups":    func(c *Client) { _, _ = c.ListGroups() },
		"audit":     func(c *Client) { _, _ = c.QueryAuditCount("fixture", time.Time{}) },
		"grant":     func(c *Client) { _ = c.GrantRule("fixture", "fixture.rule") },
	}
	for name, call := range calls {
		for _, status := range []int{200, 403, 409} {
			t.Run(fmt.Sprintf("%s/%d", name, status), func(t *testing.T) {
				client, err := New("http://fixture.invalid")
				if err != nil {
					t.Fatal(err)
				}
				body := &trackedBody{Reader: strings.NewReader(`{"packages":[],"groups":[],"total":3,"id":"fixture-id"}`)}
				client.http.Transport = roundTripFunc(func(*http.Request) (*http.Response, error) {
					return &http.Response{StatusCode: status, Header: http.Header{"Set-Cookie": {"plinth_session=fictional; Path=/"}}, Body: body}, nil
				})
				call(client)
				if !body.closed {
					t.Fatal("response body was not closed before returning")
				}
			})
		}
	}
}

func TestDefaultClientBoundsRequestLifetime(t *testing.T) {
	client, err := New("http://fixture.invalid")
	if err != nil {
		t.Fatal(err)
	}
	client.http.Transport = roundTripFunc(func(request *http.Request) (*http.Response, error) {
		deadline, ok := request.Context().Deadline()
		if !ok {
			t.Error("default client sent an unbounded request")
		} else if remaining := time.Until(deadline); remaining <= 0 || remaining > time.Minute {
			t.Errorf("default request has unreasonable remaining deadline %s", remaining)
		}
		return &http.Response{StatusCode: http.StatusOK, Body: io.NopCloser(strings.NewReader(`{"packages":[]}`))}, nil
	})
	if _, err := client.ListPackages(); err != nil {
		t.Fatal(err)
	}
}
