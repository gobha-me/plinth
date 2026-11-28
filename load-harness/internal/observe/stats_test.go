package observe_test

import (
	"io"
	"os"
	"reflect"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gobha-me/plinth/load-harness/internal/observe"
)

func TestRecorderEmptyAndErrorsOnly(t *testing.T) {
	r := observe.NewRecorder()
	if got := r.Summary(); !reflect.DeepEqual(got, observe.Summary{Errors: map[string]int{}}) {
		t.Fatalf("empty summary = %+v", got)
	}
	r.Fail("ws_timeout")
	r.Fail("ws_timeout")
	r.Fail("kernel_error")
	want := observe.Summary{Fail: 3, Errors: map[string]int{"ws_timeout": 2, "kernel_error": 1}}
	if got := r.Summary(); !reflect.DeepEqual(got, want) {
		t.Fatalf("errors-only summary = %+v, want %+v", got, want)
	}
}

func TestRecorderQuantileRanks(t *testing.T) {
	for _, tc := range []struct {
		name               string
		count              int
		p50, p95, p99, max time.Duration
	}{
		{"single", 1, time.Millisecond, time.Millisecond, time.Millisecond, time.Millisecond},
		{"two", 2, 2 * time.Millisecond, 2 * time.Millisecond, 2 * time.Millisecond, 2 * time.Millisecond},
		{"hundred", 100, 51 * time.Millisecond, 96 * time.Millisecond, 100 * time.Millisecond, 100 * time.Millisecond},
		{"hundred-and-one", 101, 51 * time.Millisecond, 96 * time.Millisecond, 100 * time.Millisecond, 101 * time.Millisecond},
	} {
		t.Run(tc.name, func(t *testing.T) {
			r := observe.NewRecorder()
			// Reverse arrival order requires sorting before selecting the ranks.
			for i := tc.count; i > 0; i-- {
				r.Success(time.Duration(i) * time.Millisecond)
			}
			got := r.Summary()
			if got.OK != tc.count || got.Fail != 0 || got.P50 != tc.p50 || got.P95 != tc.p95 || got.P99 != tc.p99 || got.Max != tc.max {
				t.Fatalf("summary = %+v; want count=%d p50=%s p95=%s p99=%s max=%s", got, tc.count, tc.p50, tc.p95, tc.p99, tc.max)
			}
			if next := r.Summary(); !reflect.DeepEqual(next, got) {
				t.Fatalf("reading summary changed recorder: first=%+v second=%+v", got, next)
			}
		})
	}
}

func TestRecorderSummaryIsIndependentSnapshot(t *testing.T) {
	r := observe.NewRecorder()
	r.Success(3 * time.Millisecond)
	r.Fail("timeout")
	first := r.Summary()
	first.Errors["timeout"] = 100
	first.Errors["injected"] = 1
	r.Success(time.Millisecond)
	r.Fail("timeout")
	want := observe.Summary{OK: 2, Fail: 2, P50: 3 * time.Millisecond, P95: 3 * time.Millisecond, P99: 3 * time.Millisecond, Max: 3 * time.Millisecond, Errors: map[string]int{"timeout": 2}}
	if got := r.Summary(); !reflect.DeepEqual(got, want) {
		t.Fatalf("snapshot mutation or later recording corrupted summary: got %+v, want %+v", got, want)
	}
	if first.OK != 1 || first.Fail != 1 || first.Errors["timeout"] != 100 {
		t.Fatalf("later recording changed previous snapshot: %+v", first)
	}
}

func TestSubscriberEmptyAndParseErrors(t *testing.T) {
	r := observe.NewSubscriberRecorder()
	want := observe.SubscriberSummary{PerSub: map[int]observe.SubscriberPerSub{}}
	if got := r.Summary(); !reflect.DeepEqual(got, want) {
		t.Fatalf("empty summary = %+v, want %+v", got, want)
	}
	r.ParseError()
	r.ParseError()
	want.ParseErrs = 2
	if got := r.Summary(); !reflect.DeepEqual(got, want) {
		t.Fatalf("parse failures must not count as observations: got %+v, want %+v", got, want)
	}
}

func TestSubscriberTimestampFallbacks(t *testing.T) {
	for _, tc := range []struct {
		name              string
		emitted, received int64
		want              time.Duration
	}{
		{"valid", 100, 137, 37 * time.Millisecond},
		{"same-time", 100, 100, 0},
		{"clock-skew", 100, 99, 0},
		{"missing-emitted", 0, 137, 0},
		{"negative-emitted", -1, 137, 0},
		{"missing-received", 100, 0, 0},
		{"negative-received", 100, -1, 0},
	} {
		t.Run(tc.name, func(t *testing.T) {
			r := observe.NewSubscriberRecorder()
			r.Observe(observe.SubscriberObs{SubscriberID: 7, Seq: 9, EmitStartedAt: tc.emitted, ReceivedAtUnix: tc.received})
			want := observe.SubscriberSummary{Observed: 1, P50: tc.want, P95: tc.want, P99: tc.want, Max: tc.want, PerSub: map[int]observe.SubscriberPerSub{7: {Observed: 1, MaxSeq: 9, P99Lag: tc.want}}}
			if got := r.Summary(); !reflect.DeepEqual(got, want) {
				t.Fatalf("summary = %+v, want %+v", got, want)
			}
		})
	}
}

func TestSubscriberQuantilesAndBurstSequenceReset(t *testing.T) {
	r := observe.NewSubscriberRecorder()
	// The same sequence can recur across bursts; counts are not deduplicated.
	for _, seq := range []int64{3, 1, 1, 0} {
		r.Observe(observe.SubscriberObs{SubscriberID: 4, Seq: seq, EmitStartedAt: 100, ReceivedAtUnix: 105})
	}
	for i := 100; i > 0; i-- {
		r.Observe(observe.SubscriberObs{SubscriberID: 8, Seq: int64(i), EmitStartedAt: 100, ReceivedAtUnix: int64(100 + i)})
	}
	r.ParseError()
	want := observe.SubscriberSummary{
		Observed: 104, ParseErrs: 1,
		P50: 49 * time.Millisecond, P95: 95 * time.Millisecond, P99: 99 * time.Millisecond, Max: 100 * time.Millisecond,
		PerSub: map[int]observe.SubscriberPerSub{
			4: {Observed: 4, MaxSeq: 3, P99Lag: 5 * time.Millisecond},
			8: {Observed: 100, MaxSeq: 100, P99Lag: 100 * time.Millisecond},
		},
	}
	got := r.Summary()
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("summary = %+v, want %+v", got, want)
	}
	got.PerSub[4] = observe.SubscriberPerSub{Observed: 999}
	delete(got.PerSub, 8)
	if next := r.Summary(); !reflect.DeepEqual(next, want) {
		t.Fatalf("returned map aliases recorder state: got %+v, want %+v", next, want)
	}
}

func TestRecordersConcurrentUpdatesAndSnapshots(t *testing.T) {
	r := observe.NewRecorder()
	subs := observe.NewSubscriberRecorder()
	var wg sync.WaitGroup
	for worker := 0; worker < 2; worker++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			for i := 0; i < 100; i++ {
				r.Success(time.Millisecond)
				r.Fail("timeout")
				subs.Observe(observe.SubscriberObs{SubscriberID: id, Seq: int64(i), EmitStartedAt: 100, ReceivedAtUnix: 101})
				subs.ParseError()
				// Read concurrently with the other writer; -race checks snapshots too.
				_ = r.Summary()
				_ = subs.Summary()
			}
		}(worker)
	}
	wg.Wait()
	want := observe.Summary{OK: 200, Fail: 200, P50: time.Millisecond, P95: time.Millisecond, P99: time.Millisecond, Max: time.Millisecond, Errors: map[string]int{"timeout": 200}}
	if got := r.Summary(); !reflect.DeepEqual(got, want) {
		t.Fatalf("concurrent call totals = %+v, want %+v", got, want)
	}
	wantSubs := observe.SubscriberSummary{Observed: 200, ParseErrs: 200, P50: time.Millisecond, P95: time.Millisecond, P99: time.Millisecond, Max: time.Millisecond, PerSub: map[int]observe.SubscriberPerSub{
		0: {Observed: 100, MaxSeq: 99, P99Lag: time.Millisecond},
		1: {Observed: 100, MaxSeq: 99, P99Lag: time.Millisecond},
	}}
	if got := subs.Summary(); !reflect.DeepEqual(got, wantSubs) {
		t.Fatalf("concurrent subscriber totals = %+v, want %+v", got, wantSubs)
	}
}

// These tests are deliberately serial: Print writes the process-wide stdout.
func captureOutput(t *testing.T, print func()) string {
	t.Helper()
	file, err := os.CreateTemp(t.TempDir(), "summary-")
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	previous := os.Stdout
	os.Stdout = file
	defer func() { os.Stdout = previous }()
	print()
	os.Stdout = previous
	if _, err := file.Seek(0, io.SeekStart); err != nil {
		t.Fatal(err)
	}
	output, err := io.ReadAll(file)
	if err != nil {
		t.Fatal(err)
	}
	return string(output)
}

func TestRecorderPrintSeparatesEmptySuccessAndFailures(t *testing.T) {
	r := observe.NewRecorder()
	empty := captureOutput(t, r.Print)
	for _, field := range []string{"calls ok   : 0", "calls fail : 0"} {
		if !strings.Contains(empty, field) {
			t.Fatalf("empty output %q missing %q", empty, field)
		}
	}
	if strings.Contains(empty, "p50") || strings.Contains(empty, "errors by code") {
		t.Fatalf("empty recorder printed absent samples: %q", empty)
	}
	r.Success(3 * time.Millisecond)
	r.Fail("timeout")
	r.Fail("timeout")
	output := captureOutput(t, r.Print)
	for _, field := range []string{"calls ok   : 1", "calls fail : 2", "p50        : 3ms", "p95        : 3ms", "p99        : 3ms", "max        : 3ms", "errors by code:"} {
		if !strings.Contains(output, field) {
			t.Fatalf("output %q missing %q", output, field)
		}
	}
	if !strings.Contains(strings.Join(strings.Fields(output), " "), "timeout 2") {
		t.Fatalf("error count missing: %q", output)
	}
}

func TestSubscriberPrintOrdersIDsAndCountsGapAgainstEmitted(t *testing.T) {
	r := observe.NewSubscriberRecorder()
	if empty := captureOutput(t, func() { r.Print(4) }); strings.Contains(empty, "ratio") || strings.Contains(empty, "lag p50") || strings.Contains(empty, "per-subscriber:") {
		t.Fatalf("empty recorder printed absent subscriber data: %q", empty)
	}
	for _, id := range []int{9, 9, 9, 2} {
		r.Observe(observe.SubscriberObs{SubscriberID: id, Seq: 5, EmitStartedAt: 100, ReceivedAtUnix: 104})
	}
	r.ParseError()
	output := captureOutput(t, func() { r.Print(2) })
	for _, field := range []string{"notifies observed : 4", "parse errors      : 1", "emitted (producer): 2", "avg observed/sub  : 2 (ratio 1.0000)", "lag p50           : 4ms", "lag p95           : 4ms", "lag p99           : 4ms", "lag max           : 4ms"} {
		if !strings.Contains(output, field) {
			t.Fatalf("output %q missing %q", output, field)
		}
	}
	var rows []string
	for _, line := range strings.Split(output, "\n") {
		if strings.HasPrefix(strings.TrimSpace(line), "sub ") {
			rows = append(rows, strings.Join(strings.Fields(line), " "))
		}
	}
	want := []string{"sub 2 observed=1 maxSeq=5 gap=1 p99lag=4ms", "sub 9 observed=3 maxSeq=5 gap=0 p99lag=4ms"}
	if !reflect.DeepEqual(rows, want) {
		t.Fatalf("subscriber rows = %q, want %q", rows, want)
	}
	withoutEmitted := captureOutput(t, func() { r.Print(0) })
	if strings.Contains(withoutEmitted, "ratio") || strings.Contains(withoutEmitted, "emitted (producer)") || strings.Contains(withoutEmitted, "gap=1") {
		t.Fatalf("missing emitted count must not infer a gap: %q", withoutEmitted)
	}
}
