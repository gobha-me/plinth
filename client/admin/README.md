# Admin package (0.1.1)

This is an ordinary extension, not a first-boot or runtime-bundled package.
Build it locally with `cmake --build build --target plinth_admin_zip`, then
upload `build/packages/admin-0.1.1.zip` through the authenticated package API.
Installing it does not grant access. The Administration application has two
primary panels: `packages` and `groups`. Grant `plinth.admin.packages` or
`plinth.admin.groups` for the corresponding panel's visibility only to intended
groups. Package operations separately require their existing package-route
rules; every groups API operation requires `kernel.admin`. Panel visibility
alone does not authorize API access.

The packages panel lists installed packages, displays selected detail and observed
name-filtered pages, validates/uploads a local ZIP, enables/disables, and
confirms uninstall. A missing or malformed mutation reply is **UNKNOWN**:
the panel does not retry it or infer commitment from a matching row. Consult
authoritative package state and audit records before submitting again.

The groups panel lists and filters groups, edits ordinary group metadata,
confirms deletion by the selected name, and adds/removes explicit members by
complete user UUID. It displays the server's explicit-member count; there is
no member directory or membership-list API. Built-in group metadata and
deletion are protected. `everyone` membership is virtual; explicit `admin`
membership can be changed after repeating the target UUID for confirmation.
Names accept 1–64 lowercase ASCII letters, digits, hyphens or underscores;
descriptions accept at most 256 UTF-8 bytes. A blank description explicitly
clears it. Changes by another administrator are not protected by a revision
check: refresh and review current state before deciding what to submit.

Groups mutations are serialized within one active panel owner. A lost or
malformed reply remains **UNKNOWN**; checking current group state cannot
establish whether that request committed, who committed it, or membership of
a specific user. Consult authoritative state and audit records before
submitting again. Leaving the panel retires pending reads and queued work;
it cannot undo a mutation already sent to the server.

Upgrade a retained `admin` 0.1.0 installation through the normal package API
using the 0.1.1 archive. The packages panel and its existing visibility rule
remain registered, and the new groups rule receives no automatic grants.

See `docs/design/DESIGN-admin-v06x.md` for the contract and deferred scope.
