// Bounded diagnostic identifiers only: never return free-form server messages,
// paths, package/account IDs, unknown object keys or credential/state values.
const responseKeys = ['error', 'state', 'failed_at_stage', 'kind', 'message', 'id', 'report', 'validation_report'];
const reportKeys = ['files_scanned', 'total_bytes', 'disposition', 'messages', 'kind', 'message'];
const codes = new Set(['bad-multipart', 'missing-field', 'validation-errors', 'migration-failed', 'upgrade-migration-failed',
    'name-already-installed', 'advisory-lock-held', 'in-flight-operation', 'upload-too-large']);
const rules = new Set(['rbac-orphan-namespace', 'rbac-json-shape', 'cf4-handler-missing',
    'panel-missing-client-file', 'panel-rbac-rule-unknown', 'capability-namespace-mismatch',
    'typed-panel-capability-not-provided', 'json-structure', 'runtime-state-skipped']);
const stages = new Set(['UPLOADING', 'VALIDATING', 'MIGRATING', 'REGISTERING', 'ACTIVE']);
const states = new Set(['INSTALL_FAILED', 'ACTIVE']);
const record = value => value !== null && typeof value === 'object' && !Array.isArray(value);
const selectedKeys = (value, allowed) => record(value) ? allowed.filter(key => Object.hasOwn(value, key)) : [];
const identifier = (value, allowed) => allowed.has(value) ? value : '[redacted]';

export function installFailureSummary(name, result, report) {
    const value = record(result?.value) ? result.value : {};
    const messages = record(report) && Array.isArray(report.messages) ? report.messages : [];
    return JSON.stringify({
        package: name === 'floatcaller' || name === 'floatprovider' ? name : '[redacted]',
        httpStatus: Number.isInteger(result?.status) && result.status >= 100 && result.status <= 599 ? result.status : 0,
        responseKeys: selectedKeys(value, responseKeys),
        errorKeys: selectedKeys(value.error, ['code', 'message']),
        code: identifier(record(value.error) ? value.error.code : value.kind, codes),
        state: identifier(value.state, states),
        failedAtStage: identifier(value.failed_at_stage, stages),
        reportKeys: selectedKeys(report, reportKeys),
        reportRules: messages.slice(0, 8).map(message => identifier(record(message) ? message.rule : null, rules)),
        reportMessagesCapped: messages.length > 8,
    });
}
