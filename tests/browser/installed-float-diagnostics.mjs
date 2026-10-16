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

export const journeyFailurePrefix = 'PLINTH_FLOAT_FAILURE ';
const failureCodes = new Set(['ERR_ASSERTION', 'ERR_INVALID_ARG_TYPE', 'ERR_INVALID_ARG_VALUE',
    'ABORT_ERR', 'ENOENT', 'EACCES', 'ETIMEDOUT']);
const failureOperators = new Set(['strictEqual', 'deepStrictEqual', 'notStrictEqual', 'notDeepStrictEqual',
    'match', 'doesNotMatch', '==', '===', '!=', '!==', 'ok', 'fail']);
const failureCategories = new Map([['AssertionError', 'assertion'], ['TimeoutError', 'timeout'],
    ['TypeError', 'type'], ['RangeError', 'range'], ['SyntaxError', 'syntax'],
    ['AggregateError', 'aggregate'], ['Error', 'operation']]);

export function journeyFailureSummary(phase, completed, error) {
    // Only a known source basename and finite coordinates leave the private
    // stack. No message, actual/expected value, path, URL or identifier escapes.
    const source = { file: 'unknown', line: 0, column: 0 };
    const stack = typeof error?.stack === 'string' ? error.stack.slice(0, 16384) : '';
    const frame = stack.match(/^\s+at (?:[^\r\n()]*\()?file:\/\/[^\r\n()]*\/installed-float-production\.mjs:([1-9][0-9]{0,5}):([1-9][0-9]{0,5})\)?\s*$/m);
    if (frame) Object.assign(source, { file: 'installed-float-production.mjs', line: Number(frame[1]), column: Number(frame[2]) });
    const count = Array.isArray(completed) && completed.length <= 12 ? completed.length : 0;
    return JSON.stringify({ schema_version: 1, phase: phase === 'install' || phase === 'verify' ? phase : 'unknown',
        completed_count: count, completed_groups: Array.from({ length: count }, (_, index) => index + 1), source,
        category: failureCategories.get(error?.name) ?? 'unknown',
        code: failureCodes.has(error?.code) ? error.code : 'unknown',
        operator: failureOperators.has(error?.operator) ? error.operator : 'unknown' });
}

export function emitJourneyFailure(phase, completed, error, write = line => console.error(line)) {
    try { write(journeyFailurePrefix + journeyFailureSummary(phase, completed, error)); }
    catch {
        // Diagnostic property getters or output failures must never replace
        // the original primary/cleanup AggregateError at the call site.
        try { write(journeyFailurePrefix + '{"diagnostic":"unavailable"}'); }
        catch { /* The private original failure remains the required outcome. */ }
    }
}
