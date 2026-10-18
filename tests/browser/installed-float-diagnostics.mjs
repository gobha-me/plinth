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
const failureSteps = new Set(['initial-caller', 'default-state', 'personal-write', 'personal-open',
    'personal-close', 'personal-delete', 'admin-seed', 'admin-open', 'admin-close', 'admin-delete',
    'fallback-open', 'fallback-focus', 'fallback-modules']);
const bodyFailures = new WeakMap();
const resourceTypes = new Set(['document', 'stylesheet', 'image', 'media', 'font', 'script',
    'texttrack', 'xhr', 'fetch', 'eventsource', 'websocket', 'manifest', 'other']);
const bodyAdvice = '\nResponse body is not available for a response that was navigated away from. Read response.body() before triggering any navigation.';

export function bodyErrorKind(error) {
    // Classify only exact known API errors, never publish the original message.
    let message;
    try { message = typeof error?.message === 'string' ? error.message : ''; } catch { return 'unknown'; }
    if (message.length > 4096) return 'unknown';
    if (message.endsWith(bodyAdvice)) message = message.slice(0, -bodyAdvice.length);
    const prefix = 'response.body: Protocol error (Network.getResponseBody): ';
    if (message === prefix + 'No data found for resource with given identifier') return 'cdp-no-data';
    if (message === prefix + 'No resource with given identifier found') return 'cdp-no-resource';
    if (message === prefix + 'Request content was evicted from inspector cache') return 'cdp-evicted';
    if (message === 'response.body: Response body is unavailable') return 'pw-detached';
    if (message === 'response.body: Target page, context or browser has been closed') return 'page-closed';
    return 'unknown';
}

export function observeSelectedBody(page, response) {
    const request = response.request();
    const state = { body_error: 'unknown', status: response.status(), resource_type: 'other',
        frame: 'unavailable', request_state: 'unobserved', request_failure: 'none',
        response_finished: 'pending', main_frame_navigations: 0, frame_detachments: 0 };
    let selectedFrame;
    try { selectedFrame = request.frame(); } catch { /* Service-worker requests may have no frame. */ }
    const frameState = () => {
        try { return selectedFrame ? selectedFrame.isDetached() ? 'detached' : 'attached' : 'unavailable'; }
        catch { return 'unavailable'; }
    };
    try { const type = request.resourceType(); if (resourceTypes.has(type)) state.resource_type = type; } catch { /* Fixed fallback. */ }
    const finishedRequest = actual => { if (actual === request) state.request_state = 'finished'; };
    const failedRequest = actual => {
        if (actual !== request) return;
        state.request_state = 'failed'; state.request_failure = 'other';
        let text;
        try { text = actual.failure()?.errorText; } catch { return; }
        const known = new Map([['net::ERR_ABORTED', 'aborted'], ['net::ERR_FAILED', 'failed'],
            ['net::ERR_TIMED_OUT', 'timeout'], ['net::ERR_CONNECTION_CLOSED', 'connection'],
            ['net::ERR_CONNECTION_RESET', 'connection']]);
        state.request_failure = known.get(text) ?? 'other';
    };
    const navigated = frame => {
        try { if (frame === page.mainFrame()) state.main_frame_navigations = Math.min(16, state.main_frame_navigations + 1); }
        catch { /* A closing page cannot turn diagnostics into a failure. */ }
    };
    const detached = () => { state.frame_detachments = Math.min(16, state.frame_detachments + 1); };
    page.on('requestfinished', finishedRequest); page.on('requestfailed', failedRequest);
    page.on('framenavigated', navigated); page.on('framedetached', detached);
    let pending;
    try { pending = Promise.resolve(response.finished()); } catch (error) { pending = Promise.reject(error); }
    // The caller retains and joins this handled promise after browser closure.
    const finished = pending.then(error => { state.response_finished = error ? 'error' : 'ok'; },
        () => { state.response_finished = 'error'; });
    return Object.freeze({ finished, reject(error) {
        state.body_error = bodyErrorKind(error); state.frame = frameState();
        if ((typeof error === 'object' && error !== null) || typeof error === 'function') bodyFailures.set(error, state);
    }, stop() {
        state.frame = frameState();
        page.off('requestfinished', finishedRequest); page.off('requestfailed', failedRequest);
        page.off('framenavigated', navigated); page.off('framedetached', detached);
    } });
}

export function createJourneyFailureSteps() {
    let failed = null;
    return Object.freeze({ failed: () => failed, run(step, action) {
        const rejected = error => { failed = failureSteps.has(step) ? step : null; throw error; };
        try {
            const value = action();
            return value && typeof value.then === 'function' ? Promise.resolve(value).catch(rejected) : value;
        } catch (error) { return rejected(error); }
    } });
}

export function journeyFailureSummary(phase, completed, error, failedStep = null) {
    // Only a known source basename and finite coordinates leave the private
    // stack. No message, actual/expected value, path, URL or identifier escapes.
    const source = { file: 'unknown', line: 0, column: 0 };
    const stack = typeof error?.stack === 'string' ? error.stack.slice(0, 16384) : '';
    // Playwright may rewrite a known frame to a plain path. Node names timer
    // callbacks with this exact alias; never accept arbitrary bracketed text.
    // This is format support, not an assertion about any particular CI failure.
    const frame = stack.match(/^\s+at (?:async )?(?:[A-Za-z0-9_.$<>]{1,128}(?: \[as _onTimeout\])? \()?(?:file:\/\/[^\r\n()]*\/|\/[^\r\n()]*\/|tests\/browser\/)?installed-float-production\.mjs:([1-9][0-9]{0,5}):([1-9][0-9]{0,5})\)?\s*$/m);
    if (frame) Object.assign(source, { file: 'installed-float-production.mjs', line: Number(frame[1]), column: Number(frame[2]) });
    const count = Array.isArray(completed) && completed.length <= 19 ? completed.length : 0;
    return JSON.stringify({ schema_version: 1, phase: phase === 'install' || phase === 'verify' ? phase : 'unknown',
        completed_count: count, completed_groups: Array.from({ length: count }, (_, index) => index + 1), source,
        failure_step: phase === 'verify' && count === 1 && failureSteps.has(failedStep) ? failedStep : null,
        body_observation: error !== null && (typeof error === 'object' || typeof error === 'function') ? bodyFailures.get(error) ?? null : null,
        category: failureCategories.get(error?.name) ?? 'unknown',
        code: failureCodes.has(error?.code) ? error.code : 'unknown',
        operator: failureOperators.has(error?.operator) ? error.operator : 'unknown' });
}

export function emitJourneyFailure(phase, completed, error, write = line => console.error(line), failedStep = null) {
    try { write(journeyFailurePrefix + journeyFailureSummary(phase, completed, error, failedStep)); }
    catch {
        // Diagnostic property getters or output failures must never replace
        // the original primary/cleanup AggregateError at the call site.
        try { write(journeyFailurePrefix + '{"diagnostic":"unavailable"}'); }
        catch { /* The private original failure remains the required outcome. */ }
    }
}
