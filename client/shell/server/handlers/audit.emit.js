// shell.zip/server/handlers/audit.emit.js
//
// Single-purpose audit-emission capability promoted from ICD-0.6.0 §10's
// `frontend.boundary.caught` deferral, per ICD-0.6.3 §6.
//
// Browser-side panel code calls
//   plinth.call("shell.audit.emit",
//               { panel_id, error_message, error_stack?, component_path? })
// and the kernel writes a single audit row pinned to the literal action
// `ext.shell.frontend.boundary.caught`. The action name comes from this
// handler — never from the caller's `detail` — so panels cannot forge
// other audit kinds even with a hand-crafted body. The non-forgeable
// identity payload (`user_id`, `session_id`, `extension_id`,
// `node_id`, `timestamp`) is filled by the audit binding from the
// kernel-bound BridgeContext per `audit_bindings.cpp:44-56`.
//
// Implementation deviations from ICD §6.4 + §11 + §A.4 (recorded in §17):
//
//   1. Capability name is `shell.audit.emit`, not `audit.emit_boundary`
//      per ICD §A.4. Three validator constraints jointly force the
//      redesign: (a) the capabilities-validation namespace-rule match
//      check (`validation.cpp:155-161`) requires `rbac_rule` to start
//      with `<namespace>.`; (b) the rule regex enforced by the
//      validator (`^[a-z][a-z0-9]*(\.[a-z][a-z0-9]*){1,4}$`) does not
//      allow underscores in segments — so the ICD-§11.1 rule name
//      `frontend.boundary.audit_emit` is structurally invalid; (c) the
//      cross-file rule CF7 (`cross_file_validator.cpp:307-330`)
//      requires every capability's namespace to equal `manifest.name`,
//      so the shell extension can only declare caps in the `shell.*`
//      namespace — the ICD-suggested `frontend` namespace is unusable.
//      The combined resolution: cap namespace = `shell` (from CF7),
//      function = `audit.emit` (no underscore for rule-regex compliance).
//
//   2. RBAC rule renamed to `shell.audit.emit` (was
//      `frontend.boundary.audit_emit` in ICD §11.1) for the same set of
//      regex-compliance + namespace-match reasons as deviation #1.
//      Default-grant to `everyone` carries forward unchanged.
//
//   3. The action name is `ext.shell.frontend.boundary.caught`, not
//      `frontend.boundary.caught` per ICD §9.1. Reason: the
//      `audit.log()` host binding requires the `ext.` prefix on every
//      extension audit (`audit_bindings.cpp:168-173`); `frontend.*`
//      would be rejected with `audit.invalid_prefix`. The effective
//      taxonomy is identical — `ext.shell.*` namespace conveys the
//      same provenance — and consumers (admin dashboards, log
//      aggregation) already filter by the `ext.<name>.` shape.
//
// Detail payload length-caps mirror the §6.5 client-side sanitiser:
// error_message 1024, error_stack 8192, component_path 8192. The shell
// applies these client-side; this handler enforces them server-side
// as defense-in-depth.

const ACTION = 'ext.shell.frontend.boundary.caught';

const MESSAGE_LIMIT   = 1024;
const STACK_LIMIT     = 8192;
const COMPONENT_LIMIT = 8192;

// Reserved payload keys per audit_bindings.cpp:52-54 (RESERVED_PAYLOAD_KEYS).
// Stripping them ensures the client cannot smuggle them past the binding
// (which rejects payloads containing any of these with audit.reserved_field).
const RESERVED_KEYS = [
    'user_id', 'session_id', 'ip_address', 'extension_id',
    'node_id', 'call_depth', 'timestamp',
];

function trim(s, n) {
    if (typeof s !== 'string') { return undefined; }
    return s.length > n ? s.slice(0, n) : s;
}

export default async function audit_emit(detail, _ctx) {
    if (detail === null || typeof detail !== 'object' || Array.isArray(detail)) {
        throw { code: 'invalid_argument',
                message: 'detail must be an object' };
    }
    const sanitized = {};
    sanitized.panel_id =
        (typeof detail.panel_id === 'string' || detail.panel_id === null)
            ? detail.panel_id
            : null;
    if (typeof detail.error_message !== 'string'
            || detail.error_message.length === 0) {
        throw { code: 'invalid_argument',
                message: 'error_message must be a non-empty string' };
    }
    sanitized.error_message = trim(detail.error_message, MESSAGE_LIMIT);

    const errStack = trim(detail.error_stack, STACK_LIMIT);
    if (errStack !== undefined) { sanitized.error_stack = errStack; }

    const compPath = trim(detail.component_path, COMPONENT_LIMIT);
    if (compPath !== undefined) { sanitized.component_path = compPath; }

    // Defense-in-depth: strip reserved-by-kernel keys the client may
    // have included (the binding rejects but we don't want the call to
    // fail visibly when the panel sent `user_id`; the kernel's bound
    // identity wins regardless).
    for (const k of RESERVED_KEYS) {
        if (Object.prototype.hasOwnProperty.call(sanitized, k)) {
            delete sanitized[k];
        }
    }

    // Action name pinned to the literal — caller's `detail.action` (if
    // any) is ignored. This is the SC2 forgery prevention from §10.
    await audit.log(ACTION, sanitized);
    return { ok: true };
}
