# Drogon patches

Local unified-diff patches applied to the Drogon source tree after
`FetchContent_Declare(drogon ...)` populates it, before
`FetchContent_MakeAvailable` builds it. See the `PATCH_COMMAND` entry
in the top-level `CMakeLists.txt`.

Each patch is a minimal surgical change — we keep these small and
upstreamable. Breaking changes belong in a fork.

## Patches

### `ftype-accessor.patch`

Added **0.5.3** (ICD-0.5.3-db-batch-silent-mode §OID-Driven PG-Type →
JS-Type Mapping).

Adds a public `drogon::orm::Field::oid()` accessor that delegates to
the already-public `drogon::orm::Result::oid(column)` via the existing
`friend class Field` relationship. Lets Plinth read the PG column OID
per-field during result conversion, replacing the 0.3.3 string-parse
heuristic (`"t"/"f"` → bool, integer parse → number, etc.) that
mis-classifies text rows like `SELECT 'true'::text`.

Three lines added to `orm_lib/inc/drogon/orm/Field.h` in the public
section. No .cpp change — the accessor is inline.

Will be filed upstream at <https://github.com/drogonframework/drogon>
on 0.5.3 ship. When merged, this patch can be retired.
