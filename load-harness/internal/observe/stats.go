// Package observe records per-call latencies and error codes, and prints
// a summary (count, p50/p95/p99, error breakdown) at run end.
//
// The internal latency slice is kept in memory; at ~8 concurrent × ~500
// call/s for 5 min (the medium tier bound) that's ~1.2M samples, ~10 MB
// at 8 bytes each. Fine for a single-run harness; revisit if LH-1+
// durations get longer.
package observe

import (
	"fmt"
	"sort"
	"sync"
	"time"
)

type Recorder struct {
	mu        sync.Mutex
	latencies []time.Duration
	errors    map[string]int
}

func NewRecorder() *Recorder {
	return &Recorder{errors: make(map[string]int)}
}

// Success records a successful call's latency.
func (r *Recorder) Success(lat time.Duration) {
	r.mu.Lock()
	r.latencies = append(r.latencies, lat)
	r.mu.Unlock()
}

// Fail records an error code (kernel-side error code string or a
// harness-side sentinel like "ws_timeout").
func (r *Recorder) Fail(code string) {
	r.mu.Lock()
	r.errors[code]++
	r.mu.Unlock()
}

// Summary returns counts + latency quantiles + error breakdown.
type Summary struct {
	OK       int
	Fail     int
	P50, P95 time.Duration
	P99, Max time.Duration
	Errors   map[string]int
}

func (r *Recorder) Summary() Summary {
	r.mu.Lock()
	defer r.mu.Unlock()
	sorted := make([]time.Duration, len(r.latencies))
	copy(sorted, r.latencies)
	sort.Slice(sorted, func(i, j int) bool { return sorted[i] < sorted[j] })
	s := Summary{
		OK:     len(sorted),
		Errors: make(map[string]int, len(r.errors)),
	}
	for k, v := range r.errors {
		s.Errors[k] = v
		s.Fail += v
	}
	if len(sorted) > 0 {
		s.P50 = sorted[len(sorted)*50/100]
		s.P95 = sorted[len(sorted)*95/100]
		s.P99 = sorted[len(sorted)*99/100]
		s.Max = sorted[len(sorted)-1]
	}
	return s
}

// Print emits a human-readable summary to stdout.
func (r *Recorder) Print() {
	s := r.Summary()
	fmt.Printf("\n── call summary ──\n")
	fmt.Printf("  calls ok   : %d\n", s.OK)
	fmt.Printf("  calls fail : %d\n", s.Fail)
	if s.OK > 0 {
		fmt.Printf("  p50        : %s\n", s.P50)
		fmt.Printf("  p95        : %s\n", s.P95)
		fmt.Printf("  p99        : %s\n", s.P99)
		fmt.Printf("  max        : %s\n", s.Max)
	}
	if len(s.Errors) > 0 {
		fmt.Printf("  errors by code:\n")
		for k, v := range s.Errors {
			fmt.Printf("    %-24s %d\n", k, v)
		}
	}
}

// ─── LH-1 storm-tier subscriber recorder ────────────────────────────
//
// The producer side uses Recorder above for WS call RTT; subscribers
// observe PG NOTIFY arrivals and need a different aggregation (lag
// samples per notify + gap detection via envelope seq, per ICD-LH-1
// §5.3). Kept separate to avoid mixing RTT latency with notify lag.

// SubscriberObs is a single notify observation from a PG LISTEN
// subscriber.
type SubscriberObs struct {
	SubscriberID   int
	Channel        string
	Seq            int64
	EmitStartedAt  int64 // ms since unix epoch, from envelope payload
	ReceivedAtUnix int64 // ms since unix epoch, from subscriber wall clock
}

// SubscriberRecorder aggregates per-subscriber observations. PG LISTEN
// broadcast semantics mean every subscriber sees every notify, so the
// aggregate signal is `observed_per_sub ≥ 0.99 × emitted` (per ICD
// §7.1). maxSeq is captured per-subscriber for informational purposes
// (highest seq value observed in any burst); it is not a gap detector
// because handler seq restarts at 0 each burst and bursts overlap.
// Per-subscriber gap is computed in the main loop against the total
// emitted count (known only at teardown), not derived from maxSeq.
type SubscriberRecorder struct {
	mu        sync.Mutex
	lags      []time.Duration
	perSub    map[int]*subStats
	parseErrs int
}

type subStats struct {
	observed int
	maxSeq   int64
	lags     []time.Duration
}

func NewSubscriberRecorder() *SubscriberRecorder {
	return &SubscriberRecorder{perSub: make(map[int]*subStats)}
}

// Observe records one envelope arrival. Lag is computed from wall
// clock; if the envelope lacks an emit_started_at (e.g. parse fallback
// or a non-harness publisher observed on the shared channel), lag is
// recorded as zero and the observation still counts toward the
// observed total.
func (r *SubscriberRecorder) Observe(o SubscriberObs) {
	var lag time.Duration
	if o.EmitStartedAt > 0 && o.ReceivedAtUnix > 0 {
		lag = time.Duration(o.ReceivedAtUnix-o.EmitStartedAt) * time.Millisecond
		if lag < 0 {
			lag = 0 // clock skew or unsynchronised publisher; treat as zero
		}
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	r.lags = append(r.lags, lag)
	s, ok := r.perSub[o.SubscriberID]
	if !ok {
		s = &subStats{}
		r.perSub[o.SubscriberID] = s
	}
	s.observed++
	s.lags = append(s.lags, lag)
	if o.Seq > s.maxSeq {
		s.maxSeq = o.Seq
	}
}

// ParseError increments the count of envelopes the subscriber could
// not decode. Doubles as a hard-exit signal per ICD §6.3 exit code 1.
func (r *SubscriberRecorder) ParseError() {
	r.mu.Lock()
	r.parseErrs++
	r.mu.Unlock()
}

// SubscriberSummary aggregates across all subscribers. Gap is computed
// by the caller at teardown against the known emitted total (see
// ComputeGapAgainstEmitted); the recorder does not derive gaps from
// maxSeq because seq restarts at 0 per burst and bursts overlap.
type SubscriberSummary struct {
	Observed  int // sum across all subscribers
	ParseErrs int
	P50, P95  time.Duration
	P99, Max  time.Duration
	PerSub    map[int]SubscriberPerSub
}

type SubscriberPerSub struct {
	Observed int
	MaxSeq   int64
	P99Lag   time.Duration
}

func (r *SubscriberRecorder) Summary() SubscriberSummary {
	r.mu.Lock()
	defer r.mu.Unlock()

	sorted := make([]time.Duration, len(r.lags))
	copy(sorted, r.lags)
	sort.Slice(sorted, func(i, j int) bool { return sorted[i] < sorted[j] })
	s := SubscriberSummary{
		Observed:  len(r.lags),
		ParseErrs: r.parseErrs,
		PerSub:    make(map[int]SubscriberPerSub, len(r.perSub)),
	}
	if len(sorted) > 0 {
		s.P50 = sorted[len(sorted)*50/100]
		s.P95 = sorted[len(sorted)*95/100]
		s.P99 = sorted[len(sorted)*99/100]
		s.Max = sorted[len(sorted)-1]
	}
	for id, st := range r.perSub {
		perSub := SubscriberPerSub{
			Observed: st.observed,
			MaxSeq:   st.maxSeq,
		}
		if len(st.lags) > 0 {
			subSorted := make([]time.Duration, len(st.lags))
			copy(subSorted, st.lags)
			sort.Slice(subSorted, func(i, j int) bool {
				return subSorted[i] < subSorted[j]
			})
			perSub.P99Lag = subSorted[len(subSorted)*99/100]
		}
		s.PerSub[id] = perSub
	}
	return s
}

// Print emits a human-readable subscriber summary to stdout. emitted
// is the producer-side sum of successful burst returns (known only
// after all workers exit); pass 0 to skip the ratio line.
func (r *SubscriberRecorder) Print(emitted int) {
	s := r.Summary()
	fmt.Printf("\n── subscriber summary ──\n")
	fmt.Printf("  notifies observed : %d\n", s.Observed)
	fmt.Printf("  parse errors      : %d\n", s.ParseErrs)
	if emitted > 0 && len(s.PerSub) > 0 {
		// Each subscriber should see every emitted notify under PG
		// LISTEN broadcast semantics; ratio is averaged across
		// subscribers.
		perSubExpected := float64(emitted)
		avgObserved := float64(s.Observed) / float64(len(s.PerSub))
		fmt.Printf("  emitted (producer): %d\n", emitted)
		fmt.Printf("  avg observed/sub  : %.0f (ratio %.4f)\n",
			avgObserved, avgObserved/perSubExpected)
	}
	if s.Observed > 0 {
		fmt.Printf("  lag p50           : %s\n", s.P50)
		fmt.Printf("  lag p95           : %s\n", s.P95)
		fmt.Printf("  lag p99           : %s\n", s.P99)
		fmt.Printf("  lag max           : %s\n", s.Max)
	}
	if len(s.PerSub) > 0 {
		fmt.Printf("  per-subscriber:\n")
		ids := make([]int, 0, len(s.PerSub))
		for id := range s.PerSub {
			ids = append(ids, id)
		}
		sort.Ints(ids)
		for _, id := range ids {
			p := s.PerSub[id]
			gap := 0
			if emitted > 0 && p.Observed < emitted {
				gap = emitted - p.Observed
			}
			fmt.Printf("    sub %-2d observed=%-6d maxSeq=%-4d gap=%-4d p99lag=%s\n",
				id, p.Observed, p.MaxSeq, gap, p.P99Lag)
		}
	}
}
