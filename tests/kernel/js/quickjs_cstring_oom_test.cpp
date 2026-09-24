// SPDX-License-Identifier: MIT
//
// Deterministic allocator-failure regression for the pinned QuickJS
// JS_ToCStringLen2 path. The test owns a raw runtime so it can fail exactly
// the UTF-8 conversion allocation and account for every remaining block.

#include <catch2/catch_test_macros.hpp>

#include <quickjs.h>

#include <cstddef>
#include <cstdlib>

namespace {

struct AllocationTracker {
  std::size_t live_blocks = 0;
  std::size_t failed_mallocs = 0;
  bool fail_next_malloc = false;
};

auto tracked_calloc(void* opaque, std::size_t count, std::size_t size)
    -> void* {
  auto* tracker = static_cast<AllocationTracker*>(opaque);
  void* result = std::calloc(count, size);
  if (result != nullptr) {
    ++tracker->live_blocks;
  }
  return result;
}

auto tracked_malloc(void* opaque, std::size_t size) -> void* {
  auto* tracker = static_cast<AllocationTracker*>(opaque);
  if (tracker->fail_next_malloc) {
    tracker->fail_next_malloc = false;
    ++tracker->failed_mallocs;
    return nullptr;
  }
  void* result = std::malloc(size);
  if (result != nullptr) {
    ++tracker->live_blocks;
  }
  return result;
}

auto tracked_free(void* opaque, void* ptr) -> void {
  auto* tracker = static_cast<AllocationTracker*>(opaque);
  if (ptr != nullptr) {
    --tracker->live_blocks;
  }
  std::free(ptr);
}

auto tracked_realloc(void* opaque, void* ptr, std::size_t size) -> void* {
  auto* tracker = static_cast<AllocationTracker*>(opaque);
  if (size == 0) {
    tracked_free(opaque, ptr);
    return nullptr;
  }
  void* result = std::realloc(ptr, size);
  if (result != nullptr && ptr == nullptr) {
    ++tracker->live_blocks;
  }
  return result;
}

constexpr JSMallocFunctions MALLOC_FUNCTIONS{
    .js_calloc = &tracked_calloc,
    .js_malloc = &tracked_malloc,
    .js_free = &tracked_free,
    .js_realloc = &tracked_realloc,
    .js_malloc_usable_size = nullptr,
};

struct RuntimeOwner {
  explicit RuntimeOwner(AllocationTracker& tracker)
      : runtime(JS_NewRuntime2(&MALLOC_FUNCTIONS, &tracker)),
        context(runtime != nullptr ? JS_NewContext(runtime) : nullptr) {}

  ~RuntimeOwner() {
    if (context != nullptr) {
      JS_FreeContext(context);
    }
    if (runtime != nullptr) {
      JS_FreeRuntime(runtime);
    }
  }

  RuntimeOwner(const RuntimeOwner&) = delete;
  auto operator=(const RuntimeOwner&) -> RuntimeOwner& = delete;

  JSRuntime* runtime;
  JSContext* context;
};

struct ValueOwner {
  ValueOwner(JSContext* context, JSValue value)
      : context(context), value(value) {}
  ~ValueOwner() { JS_FreeValue(context, value); }

  ValueOwner(const ValueOwner&) = delete;
  auto operator=(const ValueOwner&) -> ValueOwner& = delete;

  JSContext* context;
  JSValue value;
};

} // namespace

TEST_CASE("QuickJS C-string allocation failure releases its source value",
          "[js][quickjs][cstring-oom]") {
  AllocationTracker tracker;
  bool conversion_failed = false;
  bool saw_oom_error = false;
  std::size_t converted_length = 0;

  {
    RuntimeOwner owner(tracker);
    REQUIRE(owner.runtime != nullptr);
    REQUIRE(owner.context != nullptr);

    // JSON.stringify leaves this 8-bit non-ASCII code point unescaped.
    // The resulting string exists before the injected allocation failure.
    ValueOwner input(owner.context,
                     JS_NewStringLen(owner.context, "\xC3\xA9", 2));
    REQUIRE_FALSE(JS_IsException(input.value));
    ValueOwner json(owner.context,
                    JS_JSONStringify(owner.context, input.value, JS_UNDEFINED,
                                     JS_UNDEFINED));
    REQUIRE_FALSE(JS_IsException(json.value));
    REQUIRE(JS_IsString(json.value));

    tracker.fail_next_malloc = true;
    const char* bytes =
        JS_ToCStringLen(owner.context, &converted_length, json.value);
    conversion_failed = bytes == nullptr;
    if (bytes != nullptr) {
      JS_FreeCString(owner.context, bytes);
    }
    ValueOwner error(owner.context, JS_GetException(owner.context));
    saw_oom_error = JS_IsError(error.value);
    tracker.fail_next_malloc = false;
  }

  CHECK(conversion_failed);
  CHECK(converted_length == 0);
  CHECK(tracker.failed_mallocs == 1);
  CHECK(saw_oom_error);
  CHECK(tracker.live_blocks == 0);
}
