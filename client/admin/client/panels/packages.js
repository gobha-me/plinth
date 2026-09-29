import { h, Component } from 'preact';
import { getRealtimeState, onRealtimeState } from '@plinth/frontend/sdk';
import { createPackagesApi } from '../packages/api.js';
import { createPackagesController } from '../packages/controller.js';

const e = h;
const TERMINAL = new Set(['session_ended', 'session_rotated', 'not_authenticated',
    'session_expired', 'session_revoked']);
const button = (id, label, click, disabled = false, extras = {}) =>
    e('button', { id, type: 'button', onClick: click, disabled, ...extras }, label);
const summary = row => `${row.name} ${row.version} — ${row.state}`;
const errorText = error => error ? `${error.code}: ${error.message}` : 'none';

export default function packages(plinthPanel) {
    // PREPARED panels render while hidden. Register callbacks now; request only
    // from activation after the shell commits this exact panel instance.
    let renderState = null;
    const controller = createPackagesController({
        api: createPackagesApi(),
        publish: snapshot => renderState?.(snapshot),
    });
    plinthPanel.onActivate(() => {
        const error = getRealtimeState().error;
        if (TERMINAL.has(error?.code)) controller.sessionEnded();
        else void controller.activate();
    });
    plinthPanel.onDeactivate(() => controller.deactivate());

    return class PackagesPanel extends Component {
        constructor(props) {
            super(props);
            this.state = { view: controller.read() };
            this.file = null;
            this.confirmName = '';
            this.mounted = false;
            this.stopRealtime = null;
        }

        componentDidMount() {
            this.mounted = true;
            renderState = view => { if (this.mounted) this.setState({ view }); };
            this.stopRealtime = onRealtimeState(({ error }) => {
                if (TERMINAL.has(error?.code)) controller.sessionEnded();
            });
        }

        componentWillUnmount() {
            // No onDestroy Panel API exists. Actual component unmount is the
            // permanent instance-retirement boundary; inactive merely pauses.
            controller.destroy();
            this.mounted = false;
            renderState = null;
            this.stopRealtime?.();
        }

        render() {
            const state = this.state.view;
            const active = state.phase === 'active';
            const actor = state.actor;
            const selected = state.detail;
            const busy = state.attempts.some(item => ['queued', 'pending'].includes(item.status));
            const page = state.page;
            const history = state.history;
            const sessionError = getRealtimeState().error;
            const sessionTerminal = TERMINAL.has(sessionError?.code);
            return e('section', { id: 'admin-packages-panel', style: 'padding:1rem;' },
                e('h2', null, 'Administration — packages'),
                e('p', { id: 'admin-phase' }, state.phase),
                e('p', { id: 'admin-actor' }, actor
                    ? `Observed session before requests: ${actor.username} (${actor.id}), session ${actor.sessionId}`
                    : 'Current request actor not established'),
                e('p', null, 'Panel visibility does not grant package API permissions.'),
                e('p', { id: 'admin-session-error', role: 'status' },
                    state.phase === 'session-ended' ? 'Session ended; sign in again.' :
                        errorText(state.readErrors.session)),
                !active && !sessionTerminal && button('admin-retry', 'Retry current session',
                    () => { void controller.activate(); }, state.pending.session),
                active && e('section', { 'aria-label': 'Package list' },
                    e('h3', null, 'Installed packages'),
                    e('label', null,
                        e('input', { id: 'admin-include-failed', type: 'checkbox',
                            checked: page.includeFailed,
                            onChange: event => { void controller.loadList({ ...page,
                                includeFailed: event.currentTarget.checked, offset: 0 }); } }),
                        ' Include failed and retiring records'),
                    button('admin-refresh', 'Refresh list', () => { void controller.loadList(); }),
                    e('p', null, state.pending.list ? 'Loading packages…' :
                        `Page offset ${page.offset}, limit ${page.limit}`),
                    e('p', { id: 'admin-list-error', role: 'status' }, errorText(state.readErrors.list)),
                    e('div', { id: 'admin-package-list' }, state.items.map(row =>
                        button(undefined, summary(row), () => {
                            this.confirmName = ''; // Never carry confirmation to another selected row.
                            void controller.select(row.id);
                        }, false,
                            { key: row.id, 'data-package-id': row.id }))),
                    button('admin-list-prev', 'Previous page', () => {
                        void controller.loadList({ ...page, offset: Math.max(0, page.offset - page.limit) });
                    }, page.offset === 0),
                    button('admin-list-next', 'Next page', () => {
                        void controller.loadList({ ...page, offset: page.offset + page.limit });
                    }, state.items.length < page.limit)),
                active && e('section', { id: 'admin-detail', 'aria-label': 'Selected package' },
                    e('h3', null, selected ? summary(selected) :
                        state.selectedId ? 'Loading selected package' : 'Select a package'),
                    e('p', { id: 'admin-detail-error', role: 'status' }, errorText(state.readErrors.detail)),
                    selected && e('div', null,
                        e('p', null, `ID ${selected.id}; provenance ${selected.provenance || 'unknown'}`),
                        e('p', null, `Installed ${selected.installed_at || 'unknown'}`),
                        e('p', null, 'Last install report is server-reported state, not this viewer’s audit trail.'),
                        e('pre', { id: 'admin-install-report' },
                            selected.last_install_report ? JSON.stringify(selected.last_install_report, null, 2) :
                                'No install report'),
                        button('admin-history-load', 'Show observed versions', () => {
                            void controller.loadHistory(selected.name);
                        }),
                        selected.state === 'DISABLED' ?
                            button('admin-enable', 'Enable', () => {
                                void controller.transition(selected.id, 'enable');
                            }, busy) : button('admin-disable', 'Disable', () => {
                            void controller.transition(selected.id, 'disable');
                        }, busy),
                        e('label', null, `Type ${selected.name} to confirm permanent uninstall: `,
                            e('input', { id: 'admin-uninstall-name', type: 'text',
                                value: this.confirmName,
                                onInput: event => {
                                    this.confirmName = event.currentTarget.value;
                                    this.forceUpdate();
                                } })),
                        button('admin-uninstall', 'Confirm destructive uninstall', () => {
                            void controller.uninstall(selected.id, this.confirmName);
                        }, busy || this.confirmName !== selected.name))),
                active && e('section', { 'aria-label': 'Observed package records' },
                    e('h3', null, history.name ? `Observed versions named ${history.name}` :
                        'Observed versions'),
                    e('p', null, 'These paginated rows do not establish lineage, commit attribution, or an audit actor.'),
                    e('p', { id: 'admin-history-error', role: 'status' }, errorText(state.readErrors.history)),
                    e('div', { id: 'admin-history' }, history.items.map(row =>
                        e('p', { key: row.id }, summary(row)))),
                    button('admin-history-prev', 'Previous observed page', () => {
                        void controller.loadHistory(history.name, { ...history.page,
                            offset: Math.max(0, history.page.offset - history.page.limit) });
                    }, !history.name || history.page.offset === 0),
                    button('admin-history-next', 'Next observed page', () => {
                        void controller.loadHistory(history.name, { ...history.page,
                            offset: history.page.offset + history.page.limit });
                    }, !history.name || !history.hasNext)),
                active && e('section', { 'aria-label': 'Upload package ZIP' },
                    e('h3', null, 'Validate, install, or upgrade ZIP'),
                    e('input', { id: 'admin-package-file', type: 'file', accept: '.zip,application/zip',
                        onChange: event => { this.file = event.currentTarget.files?.[0] || null; } }),
                    button('admin-dry-run', 'Validate ZIP', () => {
                        void controller.dryRun(this.file);
                    }, busy),
                    button('admin-install', 'Install / upgrade ZIP', () => {
                        void controller.install(this.file);
                    }, busy),
                    e('pre', { id: 'admin-validation' }, state.validation ?
                        JSON.stringify(state.validation, null, 2) : 'No validation response yet')),
                e('section', { id: 'admin-attempts', 'aria-label': 'Current session attempts' },
                    e('h3', null, 'Current request receipts'),
                    state.attempts.map(attempt => e('article', {
                        key: attempt.requestId,
                        'data-request-id': attempt.requestId,
                        'data-outcome': attempt.status,
                    },
                    e('p', null, `Request ${attempt.requestId}: ${attempt.operation} — ${attempt.status}`),
                    e('p', null, `Request intent under observed session: ${attempt.actor.username} (${attempt.actor.id})`),
                    attempt.status === 'unknown' && e('p', null,
                        'Outcome unknown. Do not retry automatically. Observed rows cannot prove this request committed.'),
                    attempt.error && e('p', { role: 'status' }, errorText(attempt.error)),
                    attempt.error?.body && e('pre', { 'data-server-report': attempt.requestId },
                        JSON.stringify(attempt.error.body, null, 2)),
                    attempt.reconciliation !== 'none' && e('p', null,
                        `Server observation: ${attempt.reconciliation}`),
                    attempt.status === 'unknown' && button(`admin-reconcile-${attempt.requestId}`,
                        'Observe server state', () => { void controller.reconcile(attempt.requestId); },
                        attempt.reconciliation === 'pending')))));
        }
    };
}
