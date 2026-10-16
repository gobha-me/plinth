# Restore policy syntax and canonical digest codecs

Scope: the pure C++ codecs supporting the approved
[tray/resolution/navigation delivery](../icd/ICD-shell-trays-resolution-navigation.md#3-protected-local-restore-policy-proposal).
This is a partial implementation/format note, not a ratified living subsystem
contract or a declaration that persistence is available. It does not supersede
ordinary extension dispatch's fresh-per-call source behavior.

No runtime route consumes these codecs to approve persistence. There is no policy
file loader/configuration, reviewer-authority lookup, protected deployment mount,
immutable asset-serving facility, fixed shell storage dispatch or guarded write
in this slice. Those remain required before production persistence, together
with the real installed caller/provider/browser acceptance in issues #44/#46.
Successful syntax or hashing never establishes trust, permission, safe meaning,
current package generation, complete capture or bytes actually served.

## Pure policy parser

`parse_policy(raw_utf8)` returns an owned `ParsedPolicy` or a redacted `CodecError`.
Input is raw JSON: passing an already decoded object would lose duplicate keys.
A bounded SAX pass checks decoded duplicate member names, syntax/UTF-8 and budgets
before DOM construction. The whole document fails; no malformed subset survives.
No input text, reviewer identity or parser exception is returned in an error.

The exact shape is:

```text
{format_version:1, policy_revision:"positive-uint64-decimal",
 review:{admin_user_id, reviewed_at, review_ref, evidence_sha256},
 entries:[{application_id, panel_id, capability,
           projection_contract:{id,version}, artifact_digest, schema_digest,
           fields:[{key,meaning}]}]}
```

Every listed key is required; unknown keys fail at every level. Bounds are
1 MiB raw UTF-8, 256 entries, 16 KiB canonical compact bytes per original entry,
16 unique fields per entry, depth six, and 16,384 JSON value nodes. Root depth
is one; each child value adds one. Scalars and containers each count one node,
including empty containers; object member names do not add value nodes. Their
decoded names are separately retained only within the bounded parse.

Revision is a canonical positive uint64 decimal string, without leading zeroes.
Reviewer UUID uses lowercase 8-4-4-4-12 hex spelling, not a v4-only requirement.
Digests are 64 lowercase hex digits. Contract ID uses panel ID grammar and its
version is an integer JSON token from 1 through 9,007,199,254,740,991. Package
names retain the existing 2..64-byte `[a-z][a-z0-9-]*` grammar; panel/contract IDs
use 1..64-byte `[a-z][a-z0-9_-]*`. Capability signatures retain their existing
parser and 256-byte input cap; native parsed versions normalize to minimal
decimal before conflict checks. Parsing does not prove namespace ownership.

Conflict identity is application/panel/normalized capability/artifact/schema.
Two entries with the same identity, whether identical or disagreeing about
projection/fields, reject the entire policy. Distinct old/new artifact/schema
bindings may coexist. A review reference is at most 256 UTF-8 bytes; meanings
retain the existing schema's 1..256-byte UTF-8 semantics. Field names retain its
1..64-byte/NUL/prototype exclusions. Meaning text is not a secrecy certificate.

`reviewed_at` is at most 32 ASCII bytes. The supported
[RFC 3339](https://datatracker.ietf.org/doc/html/rfc3339#section-5.6) syntax has a
real Gregorian date with four-digit year 0000..9999, `T`/`t`, hours 00..23,
minutes 00..59, seconds 00..59, optional `.` plus one or more fractional digits,
and mandatory `Z`/`z` or signed HH:MM offset (hours 00..23, minutes 00..59).
`-00:00` is accepted; spelling is preserved. Second 60 is accepted only if the
offset-normalized preceding second is UTC 23:59:59 on a month's final day.
This checks a potential leap-second position, not actual IERS announcements,
clock truth, review freshness or anti-rollback. No local-time rollover, space
separator, offset seconds, comma fraction or timezone-less timestamp is accepted.

## Exact canonical JSON

The encoding uses no whitespace or BOM. All object keys sort by unsigned UTF-8
bytes; strings are scalar-valid UTF-8 without Unicode normalization. Quote and
backslash are escaped. Backspace, tab, newline, formfeed and carriage return use
`\b`, `\t`, `\n`, `\f`, `\r`; other U+0000..001F use lowercase `\u00xx`.
Slash, non-ASCII scalars and U+2028/U+2029 stay raw. Booleans are `true`/`false`,
null is `null`, and integers use minimal decimal without exponent or leading
zeroes. Valid policy/schema shapes contain no floating-point values.

Policy entry byte bounds use these bytes *before* capability normalization;
shortening a version alias cannot hide an oversized original logical entry.
Existing panel declaration serialization and `persist_keys` order are unchanged.

## Artifact inventory framing

The synchronous `artifact_digest(records, stricter_byte_cap)` borrows paths and
binary content; every input must remain alive **and unchanged** for the call.
No input views are retained. An owned bounded sort index and incremental EVP
SHA-256 avoid concatenating another package-sized buffer. Errors are explicit;
there is no partial digest, truncation or fallback format.

The exact SHA-256 preimage is:

```text
ASCII "plinth.restore.artifact" | 00 | BE32(1) | BE32(record_count)
for each record in unsigned-bytewise relative-path order:
  type_byte | BE32(path_byte_length) | exact_UTF8_path | BE32(mode)
  if regular: BE64(content_byte_length) | exact_binary_content
```

`BE32`/`BE64` are unsigned big-endian widths; type 1 is directory and type 2
regular file. Mode is exactly 0000..0777; other bits/types fail, not get cleared.
Root is implicit/excluded. All nonroot parents must be explicit directory
records, independent of input order. Empty/hidden files and directories count;
directory content must be empty. Binary file data may contain NUL or any bytes.
No owner, inode, timestamp, compression, ZIP order or host normalization enters
this digest. Changing an included path, mode, type or byte changes its preimage.

Relative slash-separated paths reject leading/trailing/doubled slash, empty,
`.` or `..` components, backslash, invalid UTF-8 and ASCII controls 00..1F/7F.
No Unicode normalization, case folding or locale comparison occurs. Duplicate
paths, file/directory collisions and file-as-parent fail.

The **new codec eligibility profile**, not legacy installer limits, is at most
4,096 total records, 32 components/path, 1,024 UTF-8 bytes/path, 1 MiB encoded
metadata and 50 MiB total content. A caller cap may only lower the content
ceiling. Metadata includes domain/NUL/version/count and every type/path-length/
path/mode/regular-length field, but excludes file payload. Bounds and checked
totals precede sorting/hash work. Oversized legacy packages are not thereby
made uninstallable; this profile does not change installation admission.

The codec cannot detect physical symlinks/hardlinks or establish that the caller
supplied the complete real package. The future owned capture must reject those
links and prove completeness, protected I/O, deadlines and immutable actual-byte
serving. Hashing a list and subsequently reopening mutable paths is insufficient.

## Context schema framing

`canonical_schema` defensively validates publicly constructible
`PanelContextSchema`: version 1, at most 16 fields, unique persisted subset,
max_bytes 0..512, valid keys and 1..256-byte meanings. Empty fields/persisted
sets remain valid; escaped NUL/control *meanings* remain compatible with the
existing parser. This does not imply PostgreSQL JSONB can store every string.
Bounds are checked before constructing copied canonical values.

The schema SHA-256 preimage is ASCII `plinth.restore.schema`, NUL, BE32(1), then
the exact canonical JSON below. Dynamic field keys and the `persist_keys` set
sort unsigned bytewise; meanings preserve their semantic string bytes.

```json
{"fields":{"record_id":{"max_bytes":64,"meaning":"Stable record identifier","required":true}},"persist_keys":["record_id"],"version":1}
```

Required/max_bytes/meaning changes, key changes or persisted-set changes alter
the encoded contract. Reordering input fields or the persisted set does not.
Matching this hash still does not approve any field meaning or reopen mapping.

## Independent reference vectors

These preimages/hashes were computed independently of the C++ encoder, using
explicit framing and standard SHA-256 in Python and Node.js. Hex below encodes
the entire preimage, not a digest of another hash. Broader Unicode/control/tree
vectors live with the focused tests and reference generator.

Empty artifact inventory:

```text
preimage_hex = 706c696e74682e726573746f72652e6172746966616374000000000100000000
sha256 = a161ab513d8f7b37477fbace66e49aeb3e6d8b825ecd08093ea5f619364ef506
```

One regular file `hello.txt`, mode 0644, four content bytes `41 00 42 0a`:

```text
preimage_hex = 706c696e74682e726573746f72652e6172746966616374000000000100000001020000000968656c6c6f2e747874000001a400000000000000044100420a
sha256 = 2980df2ad79e3c7a334cbea99bc241c784f57bafdee55a8c9fb2aecb4a38411b
```

Empty version-1 schema, canonical JSON
`{"fields":{},"persist_keys":[],"version":1}`:

```text
preimage_hex = 706c696e74682e726573746f72652e736368656d6100000000017b226669656c6473223a7b7d2c22706572736973745f6b657973223a5b5d2c2276657273696f6e223a317d
sha256 = 0f2d1ca55a6f3011606caeb5aba93b578f48ffcc7c6391de75e277e8ea0c40b2
```

## Verification scope

Exact positive Catch cases cover policy syntax and canonical bounded byte
formats. Independent vectors specify preimage bytes as well as hashes; their
reference encoder does not call production code. They supplement rather than
replace PostgreSQL/WebSocket, sanitizer, lint, candidate and new exact-merge CI.
They are not a protected-file, browser, resolver, storage or persistence test.
