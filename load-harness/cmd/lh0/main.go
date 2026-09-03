// Command lh0 — the load-harness binary for the parallel Load Harness
// stream. LH-0 (sync Tier 1 recursion) per
// docs/icd/ICD-LH-0-load-harness-scaffold.md; LH-0.1 async-bridge
// stress per docs/icd/ICD-LH-0.1-async-bridge-stress.md.
//
// Drives HTTP auth + (optional) POST /api/packages install + sustained
// WS call to either lh0:1:chain (easy/medium tiers) or lh0:1:js_stress
// (async tier). Observability is external: stats to stdout, kernel
// process to be monitored with ps/top/perf, signatures tailed from
// kernel log.
package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/gobha-me/plinth/load-harness/internal/httpclient"
	"github.com/gobha-me/plinth/load-harness/internal/observe"
	"github.com/gobha-me/plinth/load-harness/internal/pglisten"
	"github.com/gobha-me/plinth/load-harness/internal/tiers"
	"github.com/gobha-me/plinth/load-harness/internal/wsclient"
	"github.com/gobha-me/plinth/load-harness/internal/wssub"
)

// ws-fanout: grant on admin group covers both the storm producer's
// publish capability and the sidecar's install/read capabilities,
// plus the derived per-channel subscribe rule the widened
// classify_pubsub_subscribe gate consults for cross-ext subscribes
// (ICD-0.5.2 §SC6). Keep in sync with ICD-LH-2 §4.2.
var wsFanoutRules = []string{
	"lh1storm.burst",
	"lh1storm.realtime.subscribe.stormevent",
	"lh2sidecar.install",
	"lh2sidecar.read",
}

// wsFanoutChannel is the single channel the LH-2 subscriber fleet +
// sidecar subscribe to. Matches the channel lh1storm's burst handler
// publishes under (fixtures/lh1storm/server/handlers/burst.js).
const wsFanoutChannel = "plinth:ext:lh1storm:stormevent"

// Audit actions sampled at ws-fanout teardown per ICD-LH-2 §7.1.
// Broker-subsystem audit events from ICD-0.5.2 §Audit Events plus
// the realtime pipeline errors LH-1 already tailed.
var wsFanoutAuditActions = []string{
	"realtime.broker.subscribe_denied",
	"realtime.broker.dispatch_skipped",
	"realtime.broker.extension_drained",
	"realtime.notify.rejected",
	"realtime.listener.reconnected",
}

func main() {
	var (
		kernel       = flag.String("kernel", "http://localhost:8080", "base URL of plinth kernel")
		username     = flag.String("username", "", "admin username for login")
		password     = flag.String("password", "", "admin password for login")
		tierName      = flag.String("tier", "easy", "load profile: easy|medium|async|storm|ws-fanout")
		concurrency   = flag.Int("concurrency", 0, "override tier concurrency / producer count (0 = use profile default)")
		depth         = flag.Int("depth", 0, "override tier depth (0 = use profile default)")
		duration      = flag.Duration("duration", 0, "override tier duration (0 = use profile default)")
		driverZip     = flag.String("driver-zip", "", "path to driver.zip (empty = skip install)")
		keepDriver    = flag.Bool("keep-driver", false, "skip uninstall on exit")
		subscribers   = flag.Int("subscribers", 0, "storm/ws-fanout: override subscriber count (0 = use profile default)")
		wsSubscribers = flag.Int("ws-subscribers", 0, "ws-fanout: alias for --subscribers (0 = use profile default)")
		jsSubscribers = flag.Int("js-subscribers", -1, "ws-fanout: sidecar BridgeContext count; 0 disables sidecar install (-1 = use profile default)")
		sidecarZip    = flag.String("sidecar-zip", "", "ws-fanout: path to lh2sidecar.zip (required when --js-subscribers>0)")
		burstSize     = flag.Int("burst-size", 0, "storm/ws-fanout: override per-call burst count K (0 = use profile default)")
		payloadBytes  = flag.Int("payload-bytes", 0, "storm/ws-fanout: override payload body size B (0 = use profile default)")
		pgDSN         = flag.String("pg-dsn", "", "storm tier: postgres DSN for subscriber LISTEN (falls back to $PLINTH_PG_DSN)")
	)
	flag.Parse()

	if *username == "" || *password == "" {
		log.Fatalf("--username and --password required")
	}

	profile, err := tiers.Lookup(*tierName)
	if err != nil {
		log.Fatalf("%v", err)
	}
	if *concurrency > 0 {
		profile.Concurrency = *concurrency
	}
	if *depth > 0 {
		profile.Depth = *depth
	}
	if *duration > 0 {
		profile.Duration = *duration
	}
	if *subscribers > 0 {
		profile.Subscribers = *subscribers
	}
	if *wsSubscribers > 0 {
		profile.Subscribers = *wsSubscribers
	}
	if *jsSubscribers >= 0 {
		profile.JsSubscribers = *jsSubscribers
	}
	if *burstSize > 0 {
		profile.BurstSize = *burstSize
	}
	if *payloadBytes > 0 {
		profile.PayloadBytes = *payloadBytes
	}

	switch profile.Name {
	case "async":
		fmt.Printf("LH tier=%s concurrency=%d duration=%s kernel=%s "+
			"signature=lh0:1:js_stress\n",
			profile.Name, profile.Concurrency, profile.Duration, *kernel)
	case "storm":
		fmt.Printf("LH tier=%s producers=%d subscribers=%d burst=%d bytes=%d "+
			"duration=%s kernel=%s signature=lh1storm:1:burst\n",
			profile.Name, profile.Concurrency, profile.Subscribers,
			profile.BurstSize, profile.PayloadBytes, profile.Duration, *kernel)
	case "ws-fanout":
		fmt.Printf("LH tier=%s producers=%d ws_subscribers=%d "+
			"js_subscribers=%d burst=%d bytes=%d duration=%s kernel=%s "+
			"signature=lh1storm:1:burst channel=%s\n",
			profile.Name, profile.Concurrency, profile.Subscribers,
			profile.JsSubscribers, profile.BurstSize, profile.PayloadBytes,
			profile.Duration, *kernel, wsFanoutChannel)
	default:
		fmt.Printf("LH tier=%s concurrency=%d depth=%d duration=%s "+
			"kernel=%s signature=lh0:1:chain\n",
			profile.Name, profile.Concurrency, profile.Depth,
			profile.Duration, *kernel)
	}

	// Storm tier requires an extra setup path (grant rules, start
	// subscribers, use the storm-specific worker loop).
	stormDSN := ""
	if profile.Name == "storm" {
		stormDSN = *pgDSN
		if stormDSN == "" {
			stormDSN = os.Getenv("PLINTH_PG_DSN")
		}
		if stormDSN == "" {
			log.Fatalf("storm tier requires --pg-dsn or $PLINTH_PG_DSN")
		}
	}

	// ws-fanout tier requires the lh1storm driver AND (when
	// js-subscribers > 0) the lh2sidecar package, plus a non-zero
	// subscriber count on at least one arm — otherwise there's
	// nothing to observe.
	if profile.Name == "ws-fanout" {
		if *driverZip == "" {
			log.Fatalf("ws-fanout tier requires --driver-zip=fixtures/lh1storm.zip")
		}
		if profile.JsSubscribers > 0 && *sidecarZip == "" {
			log.Fatalf("ws-fanout tier with js-subscribers>0 requires --sidecar-zip=fixtures/lh2sidecar.zip")
		}
		if profile.Subscribers == 0 && profile.JsSubscribers == 0 {
			log.Fatalf("ws-fanout tier requires at least one of --ws-subscribers / --js-subscribers > 0")
		}
	}

	// Admin HTTP client (for install + list queries).
	admin, err := httpclient.New(*kernel)
	if err != nil {
		log.Fatalf("httpclient: %v", err)
	}
	if err := admin.Login(*username, *password); err != nil {
		log.Fatalf("login: %v", err)
	}
	fmt.Printf("login ok, session cookie captured\n")

	// Optional driver install.
	var installedID string
	if *driverZip != "" {
		res, err := admin.InstallPackage(*driverZip)
		if err != nil {
			log.Fatalf("install: %v (response=%v)", err, res)
		}
		if id, ok := res["id"].(string); ok {
			installedID = id
			fmt.Printf("installed driver id=%s state=%v\n", id, res["state"])
		}
	}
	defer func() {
		if installedID != "" && !*keepDriver {
			if err := admin.UninstallPackage(installedID); err != nil {
				log.Printf("uninstall failed (non-fatal): %v", err)
			} else {
				fmt.Printf("uninstalled driver id=%s\n", installedID)
			}
		}
	}()

	// ws-fanout tier: second package install (lh2sidecar). Separate
	// installedID variable so the deferred uninstall cleans both up
	// independently — a failed sidecar install still tears down
	// lh1storm, and vice versa.
	var sidecarID string
	if profile.Name == "ws-fanout" && profile.JsSubscribers > 0 {
		res, err := admin.InstallPackage(*sidecarZip)
		if err != nil {
			log.Fatalf("sidecar install: %v (response=%v)", err, res)
		}
		if id, ok := res["id"].(string); ok {
			sidecarID = id
			fmt.Printf("installed sidecar id=%s state=%v\n",
				id, res["state"])
		}
	}
	defer func() {
		if sidecarID != "" && !*keepDriver {
			if err := admin.UninstallPackage(sidecarID); err != nil {
				log.Printf("sidecar uninstall failed (non-fatal): %v", err)
			} else {
				fmt.Printf("uninstalled sidecar id=%s\n", sidecarID)
			}
		}
	}()

	// Storm tier: grant lh1storm.burst to the admin group. Install
	// registers the rule (ICD-0.4.6); it must be granted before
	// workers can invoke the capability. Idempotent via GrantRule's
	// 409-as-success contract.
	//
	// Note: ICD-LH-1 §4.2 also calls for granting a pubsub.publish
	// rule, but no such RBAC rule exists in the kernel — the
	// pubsub.publish binding is gated by extension-identity only
	// (src/kernel/js/stdlib/pubsub_bindings.cpp L154-167), with no
	// rbac_rule check. The ICD deviation is recorded in the LH-1
	// CHANGELOG entry.
	if profile.Name == "storm" {
		if installedID == "" {
			log.Fatalf("storm tier requires --driver-zip=fixtures/lh1storm.zip")
		}
		adminGroupID, err := admin.FindGroupByName("admin")
		if err != nil {
			log.Fatalf("storm: %v", err)
		}
		if err := admin.GrantRule(adminGroupID, "lh1storm.burst"); err != nil {
			log.Fatalf("storm: grant lh1storm.burst: %v", err)
		}
		fmt.Printf("granted lh1storm.burst → admin group\n")
	}

	// ws-fanout tier: grant all four rules listed in wsFanoutRules to
	// the admin group. The sidecar's pubsub.subscribe call against the
	// lh1storm channel requires the cross-ext rule per ICD-0.5.2 §SC6
	// (the widened classify gate honors it).
	if profile.Name == "ws-fanout" {
		adminGroupID, err := admin.FindGroupByName("admin")
		if err != nil {
			log.Fatalf("ws-fanout: %v", err)
		}
		for _, rule := range wsFanoutRules {
			if profile.JsSubscribers == 0 &&
				(rule == "lh2sidecar.install" ||
					rule == "lh2sidecar.read") {
				continue
			}
			if err := admin.GrantRule(adminGroupID, rule); err != nil {
				log.Fatalf("ws-fanout: grant %s: %v", rule, err)
			}
		}
		fmt.Printf("granted ws-fanout rules → admin group (%d)\n",
			len(wsFanoutRules))
	}

	rec := observe.NewRecorder()
	var subRec *observe.SubscriberRecorder
	if profile.Name == "storm" || profile.Name == "ws-fanout" {
		subRec = observe.NewSubscriberRecorder()
	}

	// Capture run-start wall clock before we install anything — the
	// ws-fanout teardown queries the audit log for rows since this
	// point to count broker-side RBAC denials / dispatch skips / etc.
	runStartTs := time.Now()

	ctx, cancel := context.WithTimeout(context.Background(), profile.Duration)
	defer cancel()

	// Handle ^C gracefully.
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-sig
		fmt.Printf("\ninterrupt received, winding down...\n")
		cancel()
	}()

	// Storm tier: subscribers must be LISTENing before the first
	// producer burst fires, or early notifies will be lost. Subscribers
	// own their own context that outlives the producer ctx by a short
	// grace so in-flight notifies land before teardown.
	var subCtx context.Context
	var subCancel context.CancelFunc
	var subWG sync.WaitGroup
	if profile.Name == "storm" {
		subCtx, subCancel = context.WithCancel(context.Background())
		for i := 0; i < profile.Subscribers; i++ {
			sub, err := pglisten.New(i, stormDSN, subRec)
			if err != nil {
				log.Fatalf("pglisten sub %d: %v", i, err)
			}
			subWG.Add(1)
			go func(s *pglisten.Subscriber) {
				defer subWG.Done()
				defer s.Close()
				s.Run(subCtx)
			}(sub)
		}
		fmt.Printf("started %d subscribers on plinth:realtime\n",
			profile.Subscribers)
	}

	// ws-fanout tier: WS-client subscribers + optional sidecar
	// install_subscription must both be in place before the first
	// producer burst fires, for the same reason storm subscribers do.
	// Each subscriber owns its own session — admin's one session
	// would fight the ConnectionRegistry same-identity displacement
	// (ICD-LH-0 convention the worker fleet already follows).
	var wsSubs []*wssub.Subscriber
	if profile.Name == "ws-fanout" {
		subCtx, subCancel = context.WithCancel(context.Background())
		for i := 0; i < profile.Subscribers; i++ {
			wc, err := httpclient.New(*kernel)
			if err != nil {
				log.Fatalf("ws-fanout sub %d httpclient: %v", i, err)
			}
			if err := wc.Login(*username, *password); err != nil {
				log.Fatalf("ws-fanout sub %d login: %v", i, err)
			}
			sub, err := wssub.Dial(i, *kernel, wc.SessionTok,
				wsFanoutChannel, 5*time.Second, subRec)
			if err != nil {
				log.Fatalf("wssub sub %d: %v", i, err)
			}
			wsSubs = append(wsSubs, sub)
			subWG.Add(1)
			go func(s *wssub.Subscriber) {
				defer subWG.Done()
				s.Run(subCtx)
			}(sub)
		}
		if profile.Subscribers > 0 {
			fmt.Printf("started %d WS subscribers on %s\n",
				profile.Subscribers, wsFanoutChannel)
		}

		// Sidecar install_subscription call — uses a short-lived WS
		// session. The bc lives in the RuntimePool slot assigned to
		// lh2sidecar for the duration of this harness run; the install
		// handler stores the unsubscribe function + counter in
		// globalThis so a later read_counters call can surface them.
		if profile.JsSubscribers > 0 {
			if err := installSidecar(*kernel, admin.SessionTok,
				wsFanoutChannel); err != nil {
				log.Fatalf("sidecar install_subscription: %v", err)
			}
			fmt.Printf("sidecar subscribed to %s\n", wsFanoutChannel)
		}
	}

	// Each worker needs its own session — the kernel's
	// ConnectionRegistry displaces duplicate (auth_type, id) pairs,
	// so two workers sharing a session token would fight over the
	// connection slot. Log each worker in independently.
	workerTokens := make([]string, profile.Concurrency)
	for i := 0; i < profile.Concurrency; i++ {
		wc, err := httpclient.New(*kernel)
		if err != nil {
			log.Fatalf("worker %d httpclient: %v", i, err)
		}
		if err := wc.Login(*username, *password); err != nil {
			log.Fatalf("worker %d login: %v", i, err)
		}
		workerTokens[i] = wc.SessionTok
	}

	var emittedCount int64 // storm tier only; summed from burst returns
	var wg sync.WaitGroup
	var activeWorkers int32
	for i := 0; i < profile.Concurrency; i++ {
		wg.Add(1)
		atomic.AddInt32(&activeWorkers, 1)
		go func(workerID int) {
			defer wg.Done()
			defer atomic.AddInt32(&activeWorkers, -1)
			runWorker(ctx, workerID, *kernel, workerTokens[workerID],
				profile, rec, &emittedCount)
		}(i)
	}

	// Periodic progress ticker (≤ duration / 10).
	tickInterval := profile.Duration / 10
	if tickInterval < time.Second {
		tickInterval = time.Second
	}
	go func() {
		t := time.NewTicker(tickInterval)
		defer t.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-t.C:
				s := rec.Summary()
				fmt.Printf("[progress] ok=%d fail=%d workers=%d\n",
					s.OK, s.Fail, atomic.LoadInt32(&activeWorkers))
			}
		}
	}()

	wg.Wait()

	// Storm / ws-fanout tier: allow ~1s grace for in-flight notifies
	// to land at subscribers before cancelling the subscriber context
	// and closing connections. Matches the §6.3 exit-code-0 tolerance
	// (observed/emitted ≥ 0.99 accepts a few boundary notifies).
	exitCode := 0
	var sidecarResult sidecarCounters
	var auditCounts map[string]int
	if profile.Name == "storm" {
		time.Sleep(1 * time.Second)
		subCancel()
		subWG.Wait()
	}
	if profile.Name == "ws-fanout" {
		time.Sleep(1 * time.Second)
		// ws-fanout: sidecar counters read before we tear down the
		// subscriber fleet — the sidecar's bc+handler have already
		// received everything in-flight by now (same 1s grace). The
		// read goes through admin's WS to the same extension.
		if profile.JsSubscribers > 0 {
			var err error
			sidecarResult, err = readSidecarCounters(*kernel, admin.SessionTok)
			if err != nil {
				log.Printf("sidecar read_counters failed (non-fatal): %v", err)
			}
		}
		subCancel()
		for _, s := range wsSubs {
			_ = s.Close()
		}
		subWG.Wait()

		// Audit count sampling per ICD-LH-2 §8. Use runStartTs as the
		// lower bound; kernel.admin grants the read via /api/audit.
		auditCounts = map[string]int{}
		for _, action := range wsFanoutAuditActions {
			n, err := admin.QueryAuditCount(action, runStartTs)
			if err != nil {
				log.Printf("audit count %q failed (non-fatal): %v",
					action, err)
				continue
			}
			auditCounts[action] = n
		}
	}

	rec.Print()

	if profile.Name == "storm" {
		emitted := int(atomic.LoadInt64(&emittedCount))
		subRec.Print(emitted)
		exitCode = stormExitCode(rec, subRec, emitted, profile.Subscribers)
	}
	if profile.Name == "ws-fanout" {
		emitted := int(atomic.LoadInt64(&emittedCount))
		subRec.Print(emitted)
		printSidecar(sidecarResult, emitted, profile.JsSubscribers)
		printAuditCounts(auditCounts)
		exitCode = wsFanoutExitCode(rec, subRec, sidecarResult,
			auditCounts, emitted, profile.Subscribers,
			profile.JsSubscribers)
	}

	// Final package-state snapshot.
	if pkgs, err := admin.ListPackages(); err == nil {
		byState := map[string]int{}
		for _, p := range pkgs {
			if s, ok := p["state"].(string); ok {
				byState[s]++
			}
		}
		fmt.Printf("\n── packages by state ──\n")
		for k, v := range byState {
			fmt.Printf("  %-16s %d\n", k, v)
		}
	}

	if exitCode != 0 {
		os.Exit(exitCode)
	}
}

// stormExitCode implements ICD-LH-1 §6.3: 0 = clean (zero worker
// errors, zero parse errors, avg observed/emitted ≥ 0.99); 1 = worker
// or subscriber errors observed. (Setup failures exit 2 earlier via
// log.Fatalf before reaching this function.)
func stormExitCode(rec *observe.Recorder, subRec *observe.SubscriberRecorder,
	emitted, subCount int) int {
	callSummary := rec.Summary()
	subSummary := subRec.Summary()
	if callSummary.Fail > 0 || subSummary.ParseErrs > 0 {
		return 1
	}
	if emitted == 0 || subCount == 0 {
		return 1 // nothing was emitted or no subscribers — degenerate
	}
	avgObservedPerSub := float64(subSummary.Observed) / float64(subCount)
	ratio := avgObservedPerSub / float64(emitted)
	if ratio < 0.99 {
		return 1
	}
	return 0
}

// asyncStressScript is the fixed JS program the async tier ships to the
// kernel for each lh0:1:js_stress call. 4 concurrent db.query(pg_sleep)
// per call mirror the shape of the ctest that fires free_zero_refcount
// (tests/kernel/js/async_hardening_test.cpp:151), so saturating this
// from harness load exercises the same signal_completion →
// JS_ExecutePendingJob path at scale. See ICD-LH-0.1 §8.
const asyncStressScript = "(async()=>{const ps=[];" +
	"for(let i=0;i<4;i++)" +
	"ps.push(db.query(`SELECT pg_sleep(0.01), ${i} AS x`));" +
	"return (await Promise.all(ps)).length;})()"

func runWorker(ctx context.Context, id int, kernel, sessionToken string,
	profile tiers.Profile, rec *observe.Recorder, emittedCount *int64) {
	ws, err := wsclient.Dial(kernel, sessionToken, 5*time.Second)
	if err != nil {
		log.Printf("worker %d dial: %v", id, err)
		rec.Fail("ws_dial_failed")
		return
	}
	defer ws.Close()

	var (
		signature string
		args      any
	)
	switch profile.Name {
	case "async":
		signature = "lh0:1:js_stress"
		args = []any{asyncStressScript}
	case "storm", "ws-fanout":
		signature = "lh1storm:1:burst"
		// lh1storm:1:burst takes {count, bytes} — the harness
		// argv format is a single object per ICD-LH-1 §4.3 handler
		// surface. ws-fanout reuses this verbatim (ICD-LH-2 §4.1).
		args = map[string]any{
			"count": profile.BurstSize,
			"bytes": profile.PayloadBytes,
		}
	default:
		signature = "lh0:1:chain"
		args = []any{profile.Depth}
	}

	// Storm / ws-fanout per-call budget includes K parallel PG
	// round-trips and the async-bridge dispatch; 10s is tight at
	// burst=64+. Give it 30s headroom when the BurstSize pushes that
	// envelope. Other tiers keep the original 10s deadline.
	callTimeout := 10 * time.Second
	if (profile.Name == "storm" || profile.Name == "ws-fanout") &&
		profile.BurstSize >= 64 {
		callTimeout = 30 * time.Second
	}

	counter := 0
	for {
		select {
		case <-ctx.Done():
			return
		default:
		}
		counter++
		callID := "w" + strconv.Itoa(id) + "-" + strconv.Itoa(counter)
		start := time.Now()
		f, err := ws.Call(callID, signature, args, callTimeout)
		if err != nil {
			// ws.Call returns an error for timeout, write failure, or
			// reader-goroutine exit. The latter two indicate a dead
			// connection — no point tight-looping. Record once and
			// exit the worker.
			msg := err.Error()
			if strings.Contains(msg, "connection closed") ||
				strings.Contains(msg, "ws write") {
				rec.Fail("ws_closed")
				log.Printf("worker %d exiting: %s", id, msg)
				return
			}
			rec.Fail("ws_timeout")
			continue
		}
		switch f["type"] {
		case "call_result":
			rec.Success(time.Since(start))
			// Storm / ws-fanout: sum {emitted: count} from the burst
			// handler return value. The kernel wraps the handler
			// return in a "result" field; fall back to the top-level
			// frame if the wrapper is absent.
			if (profile.Name == "storm" || profile.Name == "ws-fanout") &&
				emittedCount != nil {
				if n := extractEmitted(f); n > 0 {
					atomic.AddInt64(emittedCount, int64(n))
				}
			}
		case "call_error":
			code, _ := f["code"].(string)
			if code == "" {
				code = "unknown"
			}
			rec.Fail(code)
		default:
			rec.Fail("unknown_frame_type")
		}
	}
}

// extractEmitted pulls `{emitted: <number>}` out of a call_result
// frame. The kernel wraps the capability return under "value" in the
// WS call_result envelope (src/kernel/ws/call_dispatch.cpp:27). The
// "result" fallback is retained defensively in case the envelope shape
// changes in a future kernel revision.
func extractEmitted(f wsclient.Frame) int {
	var obj map[string]any
	if v, ok := f["value"].(map[string]any); ok {
		obj = v
	} else if r, ok := f["result"].(map[string]any); ok {
		obj = r
	} else {
		obj = f
	}
	switch v := obj["emitted"].(type) {
	case float64:
		return int(v)
	case int:
		return v
	case int64:
		return int(v)
	}
	return 0
}

// sidecarCounters mirrors the read_counters capability's return
// shape: {observed, lags, channel}. Lag samples are milliseconds.
type sidecarCounters struct {
	Observed int
	Lags     []int64
	Channel  string
}

// installSidecar opens an admin WS session and calls
// lh2sidecar:1:install_subscription(channel). Any call_error or
// non-call_result frame shape is a hard failure — the sidecar arm
// cannot run without a live subscription.
func installSidecar(kernel, sessionToken, channel string) error {
	ws, err := wsclient.Dial(kernel, sessionToken, 5*time.Second)
	if err != nil {
		return fmt.Errorf("sidecar ws dial: %w", err)
	}
	defer ws.Close()
	f, err := ws.Call("sidecar-install",
		"lh2sidecar:1:install_subscription",
		map[string]any{"channel": channel},
		30*time.Second)
	if err != nil {
		return fmt.Errorf("sidecar call: %w", err)
	}
	if f["type"] != "call_result" {
		code, _ := f["code"].(string)
		msg, _ := f["message"].(string)
		return fmt.Errorf("sidecar install rejected: type=%v code=%q message=%q",
			f["type"], code, msg)
	}
	return nil
}

// readSidecarCounters calls lh2sidecar:1:read_counters() on the same
// sidecar BridgeContext and unpacks the result into sidecarCounters.
// A zero result + nil error means "sidecar handler was installed but
// observed zero envelopes" — an informational outcome the harness
// will surface in the summary.
func readSidecarCounters(kernel, sessionToken string) (sidecarCounters, error) {
	ws, err := wsclient.Dial(kernel, sessionToken, 5*time.Second)
	if err != nil {
		return sidecarCounters{}, fmt.Errorf("sidecar ws dial: %w", err)
	}
	defer ws.Close()
	f, err := ws.Call("sidecar-read", "lh2sidecar:1:read_counters",
		map[string]any{}, 10*time.Second)
	if err != nil {
		return sidecarCounters{}, fmt.Errorf("sidecar call: %w", err)
	}
	if f["type"] != "call_result" {
		code, _ := f["code"].(string)
		msg, _ := f["message"].(string)
		return sidecarCounters{}, fmt.Errorf(
			"sidecar read rejected: type=%v code=%q message=%q",
			f["type"], code, msg)
	}

	// Unwrap the {value: ...} capability-return envelope, same shape
	// extractEmitted handles.
	var obj map[string]any
	if v, ok := f["value"].(map[string]any); ok {
		obj = v
	} else if r, ok := f["result"].(map[string]any); ok {
		obj = r
	} else {
		obj = f
	}
	out := sidecarCounters{}
	if v, ok := obj["observed"].(float64); ok {
		out.Observed = int(v)
	}
	if v, ok := obj["channel"].(string); ok {
		out.Channel = v
	}
	if lags, ok := obj["lags"].([]any); ok {
		out.Lags = make([]int64, 0, len(lags))
		for _, l := range lags {
			if f, ok := l.(float64); ok {
				out.Lags = append(out.Lags, int64(f))
			}
		}
	}
	return out, nil
}

// printSidecar surfaces the sidecar's observed count + lag quantiles
// alongside the producer-side emitted total. No-op when sidecar arm
// is disabled (jsSubscribers=0).
func printSidecar(c sidecarCounters, emitted, jsSubs int) {
	if jsSubs == 0 {
		return
	}
	fmt.Printf("\n── sidecar summary ──\n")
	fmt.Printf("  channel           : %s\n", c.Channel)
	fmt.Printf("  observed          : %d\n", c.Observed)
	if emitted > 0 {
		fmt.Printf("  emitted (producer): %d\n", emitted)
		fmt.Printf("  ratio (obs/emit)  : %.4f\n",
			float64(c.Observed)/float64(emitted))
	}
	if len(c.Lags) > 0 {
		sorted := make([]int64, len(c.Lags))
		copy(sorted, c.Lags)
		sort.Slice(sorted, func(i, j int) bool { return sorted[i] < sorted[j] })
		p50 := sorted[len(sorted)*50/100]
		p95 := sorted[len(sorted)*95/100]
		p99 := sorted[len(sorted)*99/100]
		fmt.Printf("  lag p50           : %d ms\n", p50)
		fmt.Printf("  lag p95           : %d ms\n", p95)
		fmt.Printf("  lag p99           : %d ms\n", p99)
		fmt.Printf("  lag max           : %d ms\n", sorted[len(sorted)-1])
	}
}

// printAuditCounts renders the realtime.* audit-action deltas
// sampled at teardown (ICD-LH-2 §7.1 / §8). Action ordering follows
// wsFanoutAuditActions for visual consistency across trials.
func printAuditCounts(counts map[string]int) {
	fmt.Printf("\n── broker audit counts (since run start) ──\n")
	for _, action := range wsFanoutAuditActions {
		n, ok := counts[action]
		if !ok {
			fmt.Printf("  %-38s <query failed>\n", action)
			continue
		}
		fmt.Printf("  %-38s %d\n", action, n)
	}
}

// wsFanoutExitCode implements ICD-LH-2 §6.3: exit 0 iff no
// worker/subscriber/parse errors, every realtime.* audit count is
// zero, both observed/emitted ratios clear 0.99. Exit 1 on any
// violation; exit 2 is reserved for setup failures (covered earlier
// via log.Fatalf).
func wsFanoutExitCode(rec *observe.Recorder,
	subRec *observe.SubscriberRecorder,
	sidecar sidecarCounters,
	auditCounts map[string]int,
	emitted, wsSubs, jsSubs int) int {
	if rec.Summary().Fail > 0 {
		return 1
	}
	if subRec != nil && subRec.Summary().ParseErrs > 0 {
		return 1
	}
	for _, action := range wsFanoutAuditActions {
		if n, ok := auditCounts[action]; ok && n > 0 {
			return 1
		}
	}
	if emitted == 0 {
		return 1 // degenerate — producers emitted nothing
	}
	// WS-fleet ratio check (mirror storm: average across subscribers).
	if wsSubs > 0 {
		avgWs := float64(subRec.Summary().Observed) / float64(wsSubs)
		if avgWs/float64(emitted) < 0.99 {
			return 1
		}
	}
	// Sidecar ratio check (single subscriber, not averaged).
	if jsSubs > 0 {
		if float64(sidecar.Observed)/float64(emitted) < 0.99 {
			return 1
		}
	}
	return 0
}
