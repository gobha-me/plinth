# Admin package (0.1.0)

This is an ordinary extension, not a first-boot or runtime-bundled package.
Build it locally with `cmake --build build --target plinth_admin_zip`, then
upload `build/packages/admin-0.1.0.zip` through the authenticated package API.
Installing it does not grant access. Grant `plinth.admin.packages` for panel
visibility and the existing package-route rules only to intended groups.

The panel lists installed packages, displays selected detail and observed
name-filtered pages, validates/uploads a local ZIP, enables/disables, and
confirms uninstall. A missing or malformed mutation reply is **UNKNOWN**:
the panel does not retry it or infer commitment from a matching row. Consult
authoritative package state and audit records before submitting again.

See `docs/design/DESIGN-admin-v06x.md` for the contract and deferred scope.
