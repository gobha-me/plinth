// libFuzzer harness for plinth::capabilities::parse_signature.
//
// The parser is pure (no PG / Drogon / globals) and therefore suitable
// for in-process fuzzing. The harness simply hands each random byte
// sequence to parse_signature and asserts the function never crashes,
// reads out of bounds, or throws. Semantic outcome (parsed vs error)
// is irrelevant for fuzz coverage — we're validating robustness, not
// correctness.
//
// Build (opt-in; Clang only):
//
//   cmake -B build-fuzz -DPLINTH_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++-18
//   cmake --build build-fuzz --target plinth_fuzz_parser
//   ./build-fuzz/plinth_fuzz_parser -max_total_time=60
//
// Tracked for CI wiring under ROADMAP 0.2.1.1.

#include "kernel/capabilities/parser.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                       std::size_t size) -> int {
  const std::string_view input{
      reinterpret_cast<const char*>(data),
      // libFuzzer gives us a raw byte buffer; reinterpreting to
      // char* is the standard harness pattern.
      size,
  };
  (void)plinth::capabilities::parse_signature(input);
  return 0;
}
