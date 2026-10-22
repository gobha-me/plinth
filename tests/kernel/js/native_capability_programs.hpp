// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace native_capability_corpus {

// Input programs are authored separately from the C++ literal observation
// oracles. ASCII source escapes still create the actual NUL/Unicode values.
inline auto data_literal(std::size_t family, std::size_t seed)
    -> std::string_view {
  static constexpr std::array<std::array<std::string_view, 4>, 8> INPUTS{{
      {{"null", "({v:null})", "[null]", "({v:[null]})"}},
      {{"false", "true", "[false,true]", "({f:false,t:true})"}},
      {{"0", "2147483648", "-9007199254740991", "9007199254740991"}},
      {{"-0.5", "0.5", "1.25", "-1024.125"}},
      {{R"JS("")JS", R"JS("A\u0000B")JS", R"JS("\u00e9\u4e2d")JS",
        R"JS("\ud83d\ude80")JS"}},
      {{R"JS(({"":1}))JS", R"JS(({"left\u0000right":2,"left":3}))JS",
        R"JS(({"\u00e9":4,"\u4e2d":5}))JS",
        R"JS(({"\ud83d\ude80":6,"quote\"\\":7}))JS"}},
      {{"[]", R"JS([null,false,0,""])JS",
        R"JS([2147483648,0.5,"A\u0000B",true])JS", "[[1],[],{k:2}]"}},
      {{"({})", R"JS(({a:[1,{b:"x"}]}))JS", "[{k:[true,null]},{z:{}}]",
        "({left:{n:1},right:{n:2},tail:[]})"}},
  }};
  if (family >= INPUTS.size() || seed >= INPUTS.front().size())
    throw std::out_of_range("native capability input program identity");
  return INPUTS[family][seed];
}

inline constexpr std::string_view DESCRIBE = R"JS(
function describe(value) {
  if (value === null) return {type:'null'};
  if (typeof value === 'boolean') return {type:'boolean',value};
  if (typeof value === 'number') return {
    type:'number',value,safe:Number.isSafeInteger(value),finite:Number.isFinite(value)
  };
  const units = text => Array.from({length:text.length},(_,i)=>text.charCodeAt(i));
  if (typeof value === 'string') return {type:'string',units:units(value)};
  if (Array.isArray(value)) return {type:'array',items:value.map(describe)};
  if (typeof value === 'object') return {
    type:'object',entries:Object.getOwnPropertyNames(value).map(key=>({
      key:units(key),value:describe(value[key])
    }))
  };
  throw new Error('unexpected native capability observation kind');
}
)JS";

inline auto data_handler(std::size_t family, std::size_t seed,
                         std::string_view receipt_statement) -> std::string {
  static_cast<void>(data_literal(family, seed));
  // Do not echo the input as the returned value. Independently construct a
  // different literal so a reused input observation cannot satisfy both ends.
  return std::string{DESCRIBE} +
         "\nexport default async function nativeData(args,ctx) {\n" +
         std::string{receipt_statement} +
         "\nreturn {arg:describe(args),ctx:describe(ctx),reply:(" +
         std::string{data_literal(family, (seed + 1) % 4)} + ")};\n}\n";
}

inline auto data_caller(std::size_t family, std::size_t seed, bool batch,
                        std::string_view signature,
                        std::string_view peer_signature) -> std::string {
  const auto argument = std::string{data_literal(family, seed)};
  const auto entry =
      batch ? "cap.batch([['" + std::string{signature} + "'," + argument +
                  "],['" + std::string{peer_signature} + "',null]])"
            : "cap.call('" + std::string{signature} + "'," + argument + ")";
  return "(async()=>{\n" + std::string{DESCRIBE} + "\nconst result=await " +
         entry + ";\n" +
         (batch ? "if(!Array.isArray(result)||result.length!==2||"
                  "Object.getOwnPropertyNames(result).length!==3||"
                  "!Object.prototype.hasOwnProperty.call(result,'0')||"
                  "!Object.prototype.hasOwnProperty.call(result,'1'))"
                  "throw new Error('native capability batch shape');\n"
                : "") +
         "const target=" + (batch ? "result[0]" : "result") +
         ";\nif(target===null||typeof "
         "target!=='object'||Array.isArray(target)||"
         "Object.getOwnPropertyNames(target).length!==3||"
         "!['arg','reply','ctx'].every(key=>Object.prototype.hasOwnProperty."
         "call(target,key)))"
         "throw new Error('native capability target shape');\nreturn "
         "{arg:target.arg,reply:describe(target.reply),ctx:target.ctx,peer:" +
         (batch ? "describe(result[1])" : "null") + "};\n})()";
}

} // namespace native_capability_corpus
