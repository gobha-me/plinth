// Benchmarks for the GlassWorm Unicode scanner primitive.
//
// Validates ICD-0.4.1 §Performance budget:
//   * 100 MB/s single-thread on the v0.4.0 CI builder image.
//
// Three cases:
//   BM_UnicodeScanner_CleanAscii_1MiB
//       The throughput acceptance gate. 1 MiB of pure ASCII; should hit
//       at least 100 MB/s (the ICD-mandated floor).
//   BM_UnicodeScanner_HeavyVS
//       Adversarial payload: dense variation selectors. Confirms that
//       the early-exit ceiling (2× threshold) keeps the worst-case
//       walk bounded.
//   BM_UnicodeScanner_LegitimateEmoji
//       Realistic mixed source: a small emoji proportion in ASCII.
//       Sanity check that legitimate inputs don't pay scanner cost.
//
// Build: cmake -B build -DPLINTH_BENCHMARKS=ON

#include <benchmark/benchmark.h>

#include "kernel/security/unicode_scanner.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace {

auto encode_utf8(std::uint32_t cp) -> std::string {
  std::string out;
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  return out;
}

auto build_clean_ascii(std::size_t bytes) -> std::string {
  return std::string(bytes, 'a');
}

auto build_heavy_vs(std::size_t copies) -> std::string {
  std::string out;
  auto vs = encode_utf8(0xFE0F);
  out.reserve(copies * vs.size());
  for (std::size_t i = 0; i < copies; ++i) {
    out += vs;
  }
  return out;
}

auto build_legit_emoji(std::size_t kib) -> std::string {
  std::string body;
  body.reserve(kib * 1024);
  while (body.size() < kib * 1024) {
    body += "const x = 'hello'; ";
    body += encode_utf8(0x1F44D);
    body += encode_utf8(0xFE0F);
    body += "\n";
  }
  return body;
}

void BM_UnicodeScanner_CleanAscii_1MiB(benchmark::State& state) {
  auto src = build_clean_ascii(1024UL * 1024UL);
  for (auto _ : state) {
    auto r = plinth::security::scan_for_invisible_unicode(src);
    benchmark::DoNotOptimize(r);
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(src.size()));
}

void BM_UnicodeScanner_HeavyVS(benchmark::State& state) {
  auto src = build_heavy_vs(200); // ≥ 2× threshold; soft-cap kicks in
  for (auto _ : state) {
    auto r = plinth::security::scan_for_invisible_unicode(src);
    benchmark::DoNotOptimize(r);
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(src.size()));
}

void BM_UnicodeScanner_LegitimateEmoji(benchmark::State& state) {
  auto src = build_legit_emoji(64); // 64 KiB
  for (auto _ : state) {
    auto r = plinth::security::scan_for_invisible_unicode(src);
    benchmark::DoNotOptimize(r);
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(src.size()));
}

BENCHMARK(BM_UnicodeScanner_CleanAscii_1MiB);
BENCHMARK(BM_UnicodeScanner_HeavyVS);
BENCHMARK(BM_UnicodeScanner_LegitimateEmoji);

} // namespace

BENCHMARK_MAIN();
