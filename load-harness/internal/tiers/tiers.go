// Package tiers owns the named load profiles for the load harness.
// A Profile pairs concurrency (number of parallel WS connections
// issuing calls), depth (the lh0:1:chain argument passed to each
// call for sync tiers), and duration.
//
// LH-0 shipped easy + medium sync tiers per ROADMAP §Load Harness.
// LH-0.1 adds the async tier that drives lh0:1:js_stress through the
// JS async bridge. LH-1 adds the storm tier (ICD-LH-1 §6.1) — M
// producer workers fire ext_lh1_storm:1:burst calls while S
// subscribers LISTEN on plinth:realtime. LH-2 adds the ws-fanout
// tier (ICD-LH-2 §6.1) — reuses LH-1's producer but replaces the
// PG LISTEN subscriber fleet with M WS-client subscribers dialling
// /ws/events + one optional sidecar extension calling
// pubsub.subscribe. Harder tiers (hard, crushing) are LH-4 scope.
package tiers

import (
	"fmt"
	"time"
)

type Profile struct {
	Name        string
	Concurrency int
	// Depth is used by the sync tiers (easy/medium) as the lh0:1:chain
	// recursion depth. Unused by async-tier workers — they send a fixed
	// script string to lh0:1:js_stress. Unused by storm tier — bursts
	// are parameterised by BurstSize/PayloadBytes.
	Depth    int
	Duration time.Duration
	// Storm-tier-specific parameters (zero for non-storm tiers).
	// Subscribers is the PG-LISTEN subscriber count under storm,
	// reused as the WS-client subscriber count (M_s) under ws-fanout.
	Subscribers  int
	BurstSize    int
	PayloadBytes int
	// ws-fanout-specific: number of sidecar-extension BridgeContexts
	// running pubsub.subscribe against the storm channel. Default 1
	// exercises the per-bc registry path with at least one handler
	// installed; 0 disables the sidecar install entirely (debugging
	// convenience). Unused by non-ws-fanout tiers.
	JsSubscribers int
}

var (
	Easy = Profile{
		Name:        "easy",
		Concurrency: 2,
		Depth:       4,
		Duration:    60 * time.Second,
	}
	Medium = Profile{
		Name:        "medium",
		Concurrency: 8,
		Depth:       8,
		Duration:    5 * time.Minute,
	}
	// Async drives lh0:1:js_stress through the kernel's JS async bridge
	// per docs/icd/ICD-LH-0.1-async-bridge-stress.md §8. Concurrency is
	// lower than medium because each call fans out 4 concurrent
	// db.query(pg_sleep(0.01)) under the hood — effective fan-out is
	// roughly 4 × workers.
	Async = Profile{
		Name:        "async",
		Concurrency: 4,
		Depth:       0, // not applicable; script is fixed in main.go
		Duration:    2 * time.Minute,
	}
	// Storm drives lh1storm:1:burst from M producer workers while
	// S external PG LISTEN subscribers observe notifies on
	// plinth:realtime.
	//
	// BurstSize=8 deviates from ICD-LH-1 §6.1's original 16. The ICD
	// assumed `default_runtime_limits().max_concurrent_async_ops = 32`
	// but the actual default has been 8 since 0.3.3
	// (src/kernel/js/runtime_pool.cpp:39). Per-BridgeContext
	// Promise.all fan-out that exceeds max_concurrent enters the
	// pending-ops requeue path, where run_on_context's outer-loop step
	// 2 skips its completion await while pending_ops is non-empty —
	// burning one event-loop thread in a spin cycle until wall_clock
	// cancellation. 0.5.0.4's extension dispatch reuses the same
	// run_on_context loop, so LH-1's burst=16 reproduced the spin
	// deterministically at the first call. burst=8 lets the storm
	// tier actually exercise the v0.5.0 realtime emit/dispatch paths
	// end-to-end (ICD §1 intent) instead of masking them behind the
	// async-bridge back-pressure issue. The fan-out-requeue fix is
	// tracked separately; see the CHANGELOG LH-1 §Findings entry and
	// tests/kernel/js/async_hardening_test.cpp:151 — that ctest keeps
	// its fan-out below cap for the same reason.
	Storm = Profile{
		Name:         "storm",
		Concurrency:  4,
		Depth:        0,
		Duration:     120 * time.Second,
		Subscribers:  4,
		BurstSize:    8,
		PayloadBytes: 512,
	}
	// WsFanout drives lh1storm:1:burst from M producer workers while
	// M_s WS-client subscribers dial /ws/events and subscribe to
	// plinth:ext:lh1storm:storm_event + 1 sidecar extension calls
	// pubsub.subscribe on the same channel (ICD-LH-2 §6.1).
	//
	// BurstSize=16 matches ICD-LH-2 §6.1 default (not Storm's 8). The
	// storm-tier downgrade to 8 was a workaround for the async-bridge
	// requeue spin at burst>max_concurrent; LH-2 sticks at the
	// max_concurrent_async_ops boundary (16), preserving
	// cross-tier-comparability with LH-1's original 16. If the
	// boundary case surfaces the async-bridge spin here too, drop to
	// 8 via --burst-size=8 and log it.
	WsFanout = Profile{
		Name:          "ws-fanout",
		Concurrency:   4,
		Depth:         0,
		Duration:      120 * time.Second,
		Subscribers:   4,
		BurstSize:     16,
		PayloadBytes:  512,
		JsSubscribers: 1,
	}
)

// Lookup returns the Profile matching `name`, or an error.
func Lookup(name string) (Profile, error) {
	switch name {
	case "easy":
		return Easy, nil
	case "medium":
		return Medium, nil
	case "async":
		return Async, nil
	case "storm":
		return Storm, nil
	case "ws-fanout":
		return WsFanout, nil
	default:
		return Profile{}, fmt.Errorf(
			"unknown tier %q (expected easy|medium|async|storm|ws-fanout)", name)
	}
}
