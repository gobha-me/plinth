// SPDX-License-Identifier: MIT
#include "kernel/extension_http/quickjs_codec.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <string_view>
#include <utility>

namespace plinth::extension_http {
namespace {
constexpr std::size_t SCRATCH_BYTES = std::size_t{2} * 1024 * 1024;
constexpr std::size_t RESERVATION_BYTES = 4096;
constexpr std::size_t RESPONSE_METADATA_BYTES = MAX_METADATA_BYTES + 16384;
using Error = QuickJsCodecError;

auto fail(Error error = Error::INVALID) -> std::unexpected<Error> {
  return std::unexpected(error);
}
auto codec_error(CodecError error) -> Error {
  return error == CodecError::TOO_LARGE ? Error::TOO_LARGE : Error::INVALID;
}
struct Value {
  JSContext* context = nullptr;
  JSValue value = JS_UNDEFINED;
  Value() = default;
  Value(JSContext* context_in, JSValue value_in)
      : context(context_in), value(value_in) {}
  ~Value() {
    if (context) JS_FreeValue(context, value);
  }
  Value(Value&& other) noexcept
      : context(other.context),
        value(std::exchange(other.value, JS_UNDEFINED)) {}
  auto operator=(Value&& other) noexcept -> Value& {
    if (this != &other) {
      if (context) JS_FreeValue(context, value);
      context = other.context;
      value = std::exchange(other.value, JS_UNDEFINED);
    }
    return *this;
  }
  Value(const Value&) = delete;
  auto operator=(const Value&) -> Value& = delete;
  auto take() -> JSValue { return std::exchange(value, JS_UNDEFINED); }
};
struct Atom {
  JSContext* context;
  JSAtom atom;
  Atom(JSContext* context_in, JSAtom atom_in)
      : context(context_in), atom(atom_in) {}
  ~Atom() { JS_FreeAtom(context, atom); }
  Atom(const Atom&) = delete;
  auto operator=(const Atom&) -> Atom& = delete;
};
struct Descriptor {
  JSContext* context;
  JSPropertyDescriptor property{0, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED};
  explicit Descriptor(JSContext* context_in) : context(context_in) {}
  ~Descriptor() {
    JS_FreeValue(context, property.value);
    JS_FreeValue(context, property.getter);
    JS_FreeValue(context, property.setter);
  }
  Descriptor(const Descriptor&) = delete;
  auto operator=(const Descriptor&) -> Descriptor& = delete;
  auto take() -> Value {
    return {context, std::exchange(property.value, JS_UNDEFINED)};
  }
};
struct Names {
  JSContext* context;
  JSPropertyEnum* names = nullptr;
  std::uint32_t count = 0;
  explicit Names(JSContext* context_in) : context(context_in) {}
  ~Names() {
    if (names) JS_FreePropertyEnum(context, names, count);
  }
  Names(const Names&) = delete;
  auto operator=(const Names&) -> Names& = delete;
};
struct StringView {
  JSContext* context;
  const char* bytes;
  std::size_t length = 0;
  StringView(JSContext* context_in, JSValueConst value) : context(context_in) {
    bytes = JS_ToCStringLen(context, &length, value);
  }
  ~StringView() {
    if (bytes) JS_FreeCString(context, bytes);
  }
  StringView(const StringView&) = delete;
  auto operator=(const StringView&) -> StringView& = delete;
  auto view() const -> std::string_view { return {bytes, length}; }
};
auto discard_exception(JSContext* context) -> void {
  if (JS_HasException(context)) {
    const Value exception{context, JS_GetException(context)};
  }
}
auto number(JSContext* context, JSValueConst value, int minimum, int maximum)
    -> QuickJsCodecResult<int> {
  if (!JS_IsNumber(value)) return fail();
  double output = 0;
  if (JS_ToFloat64(context, &output, value) < 0)
    return fail(Error::ENGINE_FAILURE);
  if (!std::isfinite(output) || std::trunc(output) != output ||
      output < minimum || output > maximum)
    return fail();
  return static_cast<int>(output);
}
auto define(JSContext* context, JSValueConst object, const char* key,
            Value value) -> bool {
  if (JS_IsException(value.value)) return false;
  return JS_DefinePropertyValueStr(context, object, key, value.take(),
                                   JS_PROP_C_W_E) >= 0;
}
auto text(JSContext* context, std::string_view value) -> Value {
  return {context, JS_NewStringLen(context, value.data(), value.size())};
}
auto integer(JSContext* context, int value) -> Value {
  return {context, JS_NewInt32(context, value)};
}
auto freeze(JSContext* context, JSValueConst value) -> bool {
  return JS_FreezeObject(context, value) >= 0;
}
} // namespace

EngineHeapReservation::EngineHeapReservation(
    std::shared_ptr<AdmissionLease> admission_in,
    ExtraMemoryCharge allocation_in, std::size_t bytes_in)
    : admission(std::move(admission_in)), allocation(std::move(allocation_in)),
      configured_bytes(bytes_in) {
}
auto EngineHeapReservation::create(std::shared_ptr<AdmissionLease> admission,
                                   std::size_t bytes)
    -> QuickJsCodecResult<std::shared_ptr<EngineHeapReservation>> try {
  if (!admission || bytes == 0 ||
      bytes > EXTRA_BYTES_PER_LEASE - RESERVATION_BYTES)
    return fail();
  auto charge = admission->reserve_extra(bytes + RESERVATION_BYTES);
  if (!charge) return fail(Error::RESOURCE_EXHAUSTED);
  return std::shared_ptr<EngineHeapReservation>(new EngineHeapReservation(
      std::move(admission), std::move(*charge), bytes));
} catch (const std::bad_alloc&) {
  return fail(Error::RESOURCE_EXHAUSTED);
} catch (...) {
  return fail(Error::ENGINE_FAILURE);
}
auto EngineHeapReservation::bytes() const noexcept -> std::size_t {
  return configured_bytes;
}
ChargedNativeResponse::ChargedNativeResponse(ExtraMemoryCharge allocation_in,
                                             NativeResponse response_in)
    : allocation(std::move(allocation_in)), response(std::move(response_in)) {
}
auto ChargedNativeResponse::value() const noexcept -> const NativeResponse& {
  return response;
}

struct QuickJsCodecTurn::State {
  JSContext* context;
  std::shared_ptr<AdmissionLease> admission;
  std::shared_ptr<const EngineHeapReservation> engine_heap;
  OwnedLiveCheck live;
  ExtraMemoryCharge scratch;
  JSClassID record_class = 0, array_class = 0;
  State(JSContext& context_in, std::shared_ptr<AdmissionLease> admission_in,
        std::shared_ptr<const EngineHeapReservation> heap_in,
        OwnedLiveCheck live_in, ExtraMemoryCharge scratch_in)
      : context(&context_in), admission(std::move(admission_in)),
        engine_heap(std::move(heap_in)), live(std::move(live_in)),
        scratch(std::move(scratch_in)) {}
  auto current() const noexcept -> bool try { return live(); } catch (...) {
    return false;
  }
  auto engine_failure(Error error = Error::ENGINE_FAILURE)
      -> std::unexpected<Error> {
    discard_exception(context);
    return fail(error);
  }
  auto names(JSValueConst object, Names& result) -> bool {
    return JS_GetOwnPropertyNames(context, &result.names, &result.count, object,
                                  JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) >= 0;
  }
  auto data(JSValueConst object, JSAtom key, bool enumerable)
      -> QuickJsCodecResult<Value> {
    Descriptor descriptor{context};
    const auto found =
        JS_GetOwnProperty(context, &descriptor.property, object, key);
    if (found < 0) return engine_failure();
    if (found != 1 || (descriptor.property.flags & JS_PROP_GETSET) != 0 ||
        ((descriptor.property.flags & JS_PROP_ENUMERABLE) != 0) != enumerable)
      return fail();
    return descriptor.take();
  }
  auto record(JSValueConst object) -> QuickJsCodecResult<std::array<Value, 4>> {
    if (JS_IsProxy(object) || !JS_IsObject(object) ||
        JS_GetClassID(object) != record_class)
      return fail();
    Names keys{context};
    if (!names(object, keys)) return engine_failure();
    if (keys.count != 4) return fail();
    constexpr std::array<std::string_view, 4> REQUIRED = {"schema", "status",
                                                          "headers", "body"};
    std::array<Value, 4> values;
    std::array<bool, 4> seen{};
    for (std::uint32_t index = 0; index < keys.count; ++index) {
      const Value key{context, JS_AtomToValue(context, keys.names[index].atom)};
      if (JS_IsException(key.value)) return engine_failure();
      if (!JS_IsString(key.value)) return fail();
      const StringView name{context, key.value};
      if (!name.bytes) return engine_failure();
      std::size_t slot = 0;
      while (slot < REQUIRED.size() && name.view() != REQUIRED[slot])
        ++slot;
      if (slot == REQUIRED.size() || seen[slot]) return fail();
      auto property = data(object, keys.names[index].atom, true);
      if (!property) return fail(property.error());
      seen[slot] = true;
      values[slot] = std::move(*property);
    }
    return values;
  }
  auto array_length(JSValueConst object, int maximum)
      -> QuickJsCodecResult<int> {
    if (JS_IsProxy(object) || !JS_IsArray(object) ||
        JS_GetClassID(object) != array_class)
      return fail();
    const Atom length_key{context, JS_NewAtom(context, "length")};
    if (length_key.atom == JS_ATOM_NULL) return engine_failure();
    auto length_value = data(object, length_key.atom, false);
    if (!length_value) return fail(length_value.error());
    auto length = number(context, length_value->value, 0, maximum);
    if (!length) return fail(length.error());
    Names keys{context};
    if (!names(object, keys)) return engine_failure();
    if (keys.count != static_cast<std::uint32_t>(*length) + 1) return fail();
    // Count plus every own index and own length proves exact dense membership.
    // Required indices are separately read with own enumerable data
    // descriptors.
    return *length;
  }
  auto index(JSValueConst object, std::uint32_t position)
      -> QuickJsCodecResult<Value> {
    const Atom key{context, JS_NewAtomUInt32(context, position)};
    if (key.atom == JS_ATOM_NULL) return engine_failure();
    return data(object, key.atom, true);
  }
  auto headers(JSValueConst object) -> QuickJsCodecResult<Headers> {
    auto length = array_length(object, static_cast<int>(MAX_HEADER_PAIRS));
    if (!length) return fail(length.error());
    Headers output;
    output.reserve(static_cast<std::size_t>(*length));
    std::size_t total = 0;
    for (int position = 0; position < *length; ++position) {
      if (!current()) return fail(Error::RETIRED);
      auto pair = index(object, static_cast<std::uint32_t>(position));
      if (!pair) return fail(pair.error());
      auto pair_length = array_length(pair->value, 2);
      if (!pair_length || *pair_length != 2) return fail();
      auto name = index(pair->value, 0), value = index(pair->value, 1);
      if (!name || !value) return fail();
      if (!JS_IsString(name->value) || !JS_IsString(value->value))
        return fail();
      const StringView name_text{context, name->value};
      const StringView value_text{context, value->value};
      if (!name_text.bytes || !value_text.bytes) return engine_failure();
      if (name_text.length > 128 || value_text.length > 8192 ||
          name_text.length > MAX_HEADER_BYTES - total ||
          value_text.length > MAX_HEADER_BYTES - total - name_text.length)
        return fail(Error::TOO_LARGE);
      if (!valid_utf8(name_text.view()) || !valid_utf8(value_text.view()))
        return fail();
      total += name_text.length + value_text.length;
      output.emplace_back(std::string{name_text.view()},
                          std::string{value_text.view()});
    }
    if (auto valid = validate_headers(output); !valid)
      return fail(codec_error(valid.error()));
    return output;
  }
  auto snapshot_body(JSValueConst object, NativeResponse& output)
      -> QuickJsCodecResult<ExtraMemoryCharge> {
    if (JS_IsProxy(object)) return fail();
    std::size_t offset = 0, length = 0, element_size = 0;
    Value backing;
    if (JS_IsArrayBuffer(object)) {
      backing = Value{context, JS_DupValue(context, object)};
    } else if (JS_GetTypedArrayType(object) == JS_TYPED_ARRAY_UINT8) {
      backing = Value{context, JS_GetTypedArrayBuffer(context, object, &offset,
                                                      &length, &element_size)};
      if (JS_IsException(backing.value)) return engine_failure(Error::INVALID);
      if (element_size != 1) return fail();
    } else
      return fail();
    if (!JS_IsArrayBuffer(backing.value)) return fail();
    std::size_t backing_length = 0;
    const auto* bytes =
        JS_GetArrayBuffer(context, &backing_length, backing.value);
    if (JS_HasException(context)) return engine_failure(Error::INVALID);
    if (!bytes && backing_length != 0) return fail();
    if (JS_IsArrayBuffer(object)) length = backing_length;
    if (offset > backing_length || length > backing_length - offset)
      return fail();
    if (length > MAX_BODY_BYTES) return fail(Error::TOO_LARGE);
    auto charge = admission->reserve_extra(length + RESPONSE_METADATA_BYTES);
    if (!charge) return fail(Error::RESOURCE_EXHAUSTED);
    // No JS execution/property access between the pointer and exact snapshot.
    output.body.resize(length);
    if (length != 0) std::memcpy(output.body.data(), bytes + offset, length);
    return std::move(*charge);
  }
  auto response(JSValueConst object)
      -> QuickJsCodecResult<ChargedNativeResponse> {
    auto fields = record(object);
    if (!fields) return fail(fields.error());
    auto schema = number(context, (*fields)[0].value, 1, 1);
    auto status = number(context, (*fields)[1].value, 200, 599);
    if (!schema || !status) return fail();
    if (!current()) return fail(Error::RETIRED);
    std::optional<ExtraMemoryCharge> output_charge;
    NativeResponse output;
    auto charge = snapshot_body((*fields)[3].value, output);
    if (!charge) return fail(charge.error());
    output_charge.emplace(std::move(*charge));
    auto decoded_headers = headers((*fields)[2].value);
    if (!decoded_headers) return fail(decoded_headers.error());
    output.status = *status;
    output.headers = std::move(*decoded_headers);
    if (auto valid = validate_response(output); !valid)
      return fail(codec_error(valid.error()));
    if (!current()) return fail(Error::RETIRED);
    return ChargedNativeResponse{std::move(*output_charge), std::move(output)};
  }
  auto encoded_headers(const Headers& headers_in) -> Value {
    Value output{context, JS_NewArray(context)};
    if (JS_IsException(output.value)) return output;
    std::uint32_t position = 0;
    for (const auto& [name, value] : headers_in) {
      Value pair{context, JS_NewArray(context)};
      auto key = text(context, name), item = text(context, value);
      if (JS_IsException(pair.value) || JS_IsException(key.value) ||
          JS_IsException(item.value) ||
          JS_DefinePropertyValueUint32(context, pair.value, 0, key.take(),
                                       JS_PROP_C_W_E) < 0 ||
          JS_DefinePropertyValueUint32(context, pair.value, 1, item.take(),
                                       JS_PROP_C_W_E) < 0 ||
          !freeze(context, pair.value) ||
          JS_DefinePropertyValueUint32(context, output.value, position++,
                                       pair.take(), JS_PROP_C_W_E) < 0)
        return {context, JS_EXCEPTION};
    }
    if (!freeze(context, output.value)) return {context, JS_EXCEPTION};
    return output;
  }
  auto encoded_request(const NativeRequest& input) -> Value {
    Value output{context, JS_NewObject(context)};
    if (JS_IsException(output.value) ||
        !define(context, output.value, "schema", integer(context, 1)) ||
        !define(context, output.value, "request_id",
                text(context, input.data.request_id)) ||
        !define(context, output.value, "method",
                text(context, input.data.method)) ||
        !define(context, output.value, "path",
                text(context, input.data.target.path)) ||
        !define(context, output.value, "original_path",
                text(context, input.data.target.original_path)) ||
        !define(context, output.value, "query",
                text(context, input.data.target.query)) ||
        !define(context, output.value, "headers",
                encoded_headers(input.data.headers)) ||
        !define(context, output.value, "body",
                {context, JS_NewUint8ArrayCopy(context, input.body.data(),
                                               input.body.size())}) ||
        !freeze(context, output.value))
      return {context, JS_EXCEPTION};
    return output;
  }
  auto encoded_context(const ContextData& input,
                       BorrowedContextControls controls) -> Value {
    Value principal{context, JS_NewObject(context)},
        extension{context, JS_NewObject(context)};
    if (JS_IsException(principal.value) || JS_IsException(extension.value) ||
        !define(context, principal.value, "kind",
                text(context, input.principal.kind == Principal::Kind::PAT
                                  ? "pat"
                                  : "anonymous")))
      return {context, JS_EXCEPTION};
    if (input.principal.kind == Principal::Kind::PAT &&
        (!define(context, principal.value, "user_id",
                 text(context, input.principal.user_id)) ||
         !define(context, principal.value, "pat_id",
                 text(context, input.principal.pat_id))))
      return {context, JS_EXCEPTION};
    const auto& ext = input.extension;
    if (!freeze(context, principal.value) ||
        !define(context, extension.value, "package_id",
                text(context, ext.package_id)) ||
        !define(context, extension.value, "name", text(context, ext.name)) ||
        !define(context, extension.value, "version",
                text(context, ext.version)) ||
        !define(context, extension.value, "artifact_sha256",
                text(context, ext.artifact_sha256)) ||
        !define(context, extension.value, "generation_receipt_id",
                text(context, ext.generation_receipt_id)) ||
        !freeze(context, extension.value))
      return {context, JS_EXCEPTION};
    Value output{context, JS_NewObject(context)};
    if (JS_IsException(output.value) ||
        !define(context, output.value, "schema", integer(context, 1)) ||
        !define(context, output.value, "dispatch_id",
                text(context, input.dispatch_id)) ||
        !define(context, output.value, "owner_epoch",
                text(context, input.owner_epoch)) ||
        !define(context, output.value, "adapter_epoch",
                text(context, input.adapter_epoch)) ||
        !define(context, output.value, "clock_domain_id",
                text(context, input.clock_domain_id)) ||
        !define(context, output.value, "admitted_at_ns",
                text(context, std::to_string(input.admitted_at_ns))) ||
        !define(context, output.value, "execution_cutoff_ns",
                text(context, std::to_string(input.execution_cutoff_ns))) ||
        !define(context, output.value, "matched_prefix",
                text(context, input.matched_prefix)) ||
        !define(context, output.value, "principal", std::move(principal)) ||
        !define(context, output.value, "extension", std::move(extension)) ||
        !define(context, output.value, "cancel",
                {context, JS_DupValue(context, controls.cancel)}) ||
        !define(
            context, output.value, "remaining_execution_ms",
            {context, JS_DupValue(context, controls.remaining_execution_ms)}) ||
        !freeze(context, output.value))
      return {context, JS_EXCEPTION};
    return output;
  }
};
QuickJsCodecTurn::QuickJsCodecTurn(std::unique_ptr<State> state_in)
    : state(std::move(state_in)) {
}
QuickJsCodecTurn::~QuickJsCodecTurn() = default;
auto QuickJsCodecTurn::create(JSContext& context,
                              std::shared_ptr<AdmissionLease> admission,
                              std::shared_ptr<const EngineHeapReservation> heap,
                              OwnedLiveCheck live)
    -> QuickJsCodecResult<std::unique_ptr<QuickJsCodecTurn>> try {
  if (!admission || !heap || heap->admission.get() != admission.get() ||
      !live || JS_HasException(&context))
    return fail();
  auto charge = admission->reserve_extra(SCRATCH_BYTES);
  if (!charge) return fail(Error::RESOURCE_EXHAUSTED);
  auto state =
      std::make_unique<State>(context, std::move(admission), std::move(heap),
                              std::move(live), std::move(*charge));
  if (!state->current()) return fail(Error::RETIRED);
  const Value record{&context, JS_NewObject(&context)},
      array{&context, JS_NewArray(&context)};
  if (JS_IsException(record.value) || JS_IsException(array.value))
    return state->engine_failure();
  state->record_class = JS_GetClassID(record.value);
  state->array_class = JS_GetClassID(array.value);
  return std::unique_ptr<QuickJsCodecTurn>(
      new QuickJsCodecTurn(std::move(state)));
} catch (const std::bad_alloc&) {
  return fail(Error::RESOURCE_EXHAUSTED);
} catch (...) {
  return fail(Error::ENGINE_FAILURE);
}

auto QuickJsCodecTurn::with_encoded_call(const NativeRequest& request,
                                         const ContextData& context,
                                         BorrowedContextControls controls,
                                         const EncodedCallConsumer& consume)
    -> QuickJsCodecResult<void> try {
  if (JS_HasException(state->context)) return fail(Error::ENGINE_FAILURE);
  if (!state->current()) return fail(Error::RETIRED);
  if (!consume || JS_IsProxy(controls.cancel) ||
      !JS_IsObject(controls.cancel) ||
      JS_IsProxy(controls.remaining_execution_ms) ||
      !JS_IsFunction(state->context, controls.remaining_execution_ms))
    return fail();
  if (auto valid = validate_call_data(request, context); !valid)
    return fail(codec_error(valid.error()));
  const Value encoded_request = state->encoded_request(request);
  if (JS_IsException(encoded_request.value)) return state->engine_failure();
  if (!state->current()) return fail(Error::RETIRED);
  const Value encoded_context = state->encoded_context(context, controls);
  if (JS_IsException(encoded_context.value)) return state->engine_failure();
  if (!state->current()) return fail(Error::RETIRED);
  auto result = consume(encoded_request.value, encoded_context.value);
  // Consumer owns its actual handler error/result, including pending exception.
  return result;
} catch (const std::bad_alloc&) {
  return state->engine_failure(Error::RESOURCE_EXHAUSTED);
} catch (...) {
  return state->engine_failure();
}

auto QuickJsCodecTurn::inspect_return(JSValueConst value)
    -> QuickJsCodecResult<InspectedReturn> try {
  if (JS_HasException(state->context)) return fail(Error::ENGINE_FAILURE);
  if (!state->current()) return fail(Error::RETIRED);
  if (JS_IsProxy(value)) return fail();
  if (JS_IsPromise(value)) {
    switch (JS_PromiseState(state->context, value)) {
      case JS_PROMISE_PENDING: return InspectedReturn{PendingNativePromise{}};
      case JS_PROMISE_REJECTED: return InspectedReturn{RejectedNativePromise{}};
      case JS_PROMISE_FULFILLED: {
        const Value fulfilled{state->context,
                              JS_PromiseResult(state->context, value)};
        auto response = state->response(fulfilled.value);
        if (!response) return state->engine_failure(response.error());
        return InspectedReturn{std::move(*response)};
      }
      default: return fail();
    }
  }
  auto response = state->response(value);
  if (!response) return state->engine_failure(response.error());
  return InspectedReturn{std::move(*response)};
} catch (const std::bad_alloc&) {
  return state->engine_failure(Error::RESOURCE_EXHAUSTED);
} catch (...) {
  return state->engine_failure();
}
} // namespace plinth::extension_http
