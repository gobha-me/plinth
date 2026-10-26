import { h, Component } from 'preact';
import { getRealtimeState, onRealtimeState } from '@plinth/frontend/sdk';
import { createGroupsApi } from '../groups/api.js';
import { createGroupsController } from '../groups/controller.js';

const e = h;
const TERMINAL = new Set(['session_ended', 'session_rotated', 'not_authenticated',
    'session_expired', 'session_revoked']);
const errorText = error => error?.message || '';
const phaseText = phase => ({ prepared: 'Ready to open', loading: 'Checking your account…',
    active: 'Ready', inactive: 'Paused', 'session-ended': 'Sign in to continue',
    destroyed: 'Closed' })[phase];
const operationText = operation => ({ create: 'Create group', update: 'Update group',
    delete: 'Delete group', 'add-member': 'Add member', 'remove-member': 'Remove member' })[operation];
const outcomeText = status => ({ queued: 'Waiting', pending: 'Sending', succeeded: 'Acknowledged',
    failed: 'Not completed', unknown: 'Outcome unknown' })[status];
const targetText = target => target.userId ? `User ID ${target.userId}; group ID ${target.id}` :
    `Group: ${target.name}`;
const observedText = observed => {
    const list = observed.list ? `${observed.list.groups.length} groups returned.` : 'Group list unavailable.';
    const detail = observed.detail?.absent ? 'The group is currently absent.' : observed.detail
        ? `Current group: ${observed.detail.name}; explicit members: ${observed.detail.member_count}.`
        : 'Group details were not observed.';
    return `${list} ${detail}`;
};
const button = (id, label, click, disabled = false, extra = {}) =>
    e('button', { id, type: 'button', onClick: click, disabled, ...extra }, label);
const field = (id, label, value, input, extra = {}) => e('label', { htmlFor: id }, label,
    e('input', { id, type: 'text', value, onInput: input, ...extra }));

export default function groups(plinthPanel) {
    let renderState = null;
    let epoch = 0;
    const controller = createGroupsController({
        api: createGroupsApi(),
        publish: view => renderState?.(view),
    });
    plinthPanel.onActivate(() => {
        epoch++;
        if (TERMINAL.has(getRealtimeState().error?.code)) controller.sessionEnded();
        else void controller.activate();
    });
    plinthPanel.onDeactivate(() => { epoch++; controller.deactivate(); });

    return class GroupsPanel extends Component {
        constructor(props) {
            super(props);
            this.state = { view: controller.read(), filter: '', page: 0, memberConfirmation: '' };
            this.mounted = false;
            this.stopRealtime = null;
        }

        componentDidMount() {
            this.mounted = true;
            renderState = view => {
                if (!this.mounted) return;
                const retired = !['active', 'loading'].includes(view.phase);
                this.setState({ view, ...(retired ? { filter: '', page: 0, memberConfirmation: '' } : {}) });
            };
            this.stopRealtime = onRealtimeState(({ error }) => {
                if (TERMINAL.has(error?.code)) { epoch++; controller.sessionEnded(); }
            });
        }

        componentWillUnmount() {
            epoch++;
            this.mounted = false;
            renderState = null;
            this.stopRealtime?.();
            controller.destroy();
        }

        async act(action, focus) {
            const admission = epoch;
            const selected = controller.read().selectedId;
            await action();
            const current = controller.read();
            if (this.mounted && admission === epoch && current.selectedId === selected &&
                current.phase === 'active' && current.formError) {
                focus?.(current.formError)?.focus();
            }
        }

        render() {
            const view = this.state.view;
            const active = view.phase === 'active';
            const detail = view.detail;
            const policy = view.policy;
            const busy = view.attempts.some(attempt => ['queued', 'pending'].includes(attempt.status));
            const filtered = view.items.filter(group => group.name.includes(this.state.filter));
            const lastPage = Math.max(0, Math.ceil(filtered.length / 25) - 1);
            const page = Math.min(this.state.page, lastPage);
            const memberConfirmed = detail?.name !== 'admin' ||
                (view.memberId !== '' && this.state.memberConfirmation === view.memberId);
            const metadata = (prefix, draft, setter, readonly) => [
                field(`groups-${prefix}-name`, prefix === 'create' ? 'New group name' : 'Group name', draft.name,
                    event => setter({ name: event.currentTarget.value }), {
                        maxLength: 64, readOnly: readonly,
                        ref: element => { this[`${prefix}Name`] = element; },
                        'aria-describedby': 'groups-name-help',
                    }),
                e('label', { htmlFor: `groups-${prefix}-description` }, 'Description',
                    e('textarea', { id: `groups-${prefix}-description`, value: draft.description,
                        readOnly: readonly, 'aria-describedby': 'groups-description-help',
                        ref: element => { this[`${prefix}Description`] = element; },
                        onInput: event => setter({ description: event.currentTarget.value }) })),
            ];
            return e('section', { id: 'admin-groups-panel', style: 'padding:1rem;' },
                e('h2', null, 'Administration — groups'),
                e('p', { id: 'groups-phase', 'data-phase': view.phase }, phaseText(view.phase)),
                e('p', { id: 'groups-actor' }, view.actor
                    ? `Account checked before request: ${view.actor.username}` : 'Account not yet checked'),
                e('p', null, 'Being able to open this panel does not grant permission to manage groups. The kernel.admin permission is required.'),
                e('p', { id: 'groups-session-error', role: 'status', 'data-error-code': view.readErrors.session?.code }, view.phase === 'session-ended'
                    ? 'Session ended; sign in again.' : errorText(view.readErrors.session)),
                !active && !TERMINAL.has(getRealtimeState().error?.code) &&
                    button('groups-retry', 'Retry current session', () => { void controller.activate(); },
                        view.pending.session),
                active && e('div', null,
                    e('p', { id: 'groups-name-help' },
                        'Names: 1–64 lowercase ASCII letters, digits, hyphens or underscores.'),
                    e('p', { id: 'groups-description-help' },
                        'Description: at most 256 UTF-8 bytes. Blank explicitly clears the description.'),
                    e('section', { 'aria-label': 'Group list' },
                        e('h3', null, 'Groups'),
                        field('groups-filter', 'Filter groups by name', this.state.filter,
                            event => this.setState({ filter: event.currentTarget.value, page: 0 })),
                        button('groups-refresh', 'Refresh list', () => { void controller.loadList(); }, view.pending.list),
                        e('p', { id: 'groups-list-status' }, view.pending.list ? 'Loading groups…' :
                            `${filtered.length} groups — page ${page + 1} of ${lastPage + 1}`),
                        e('p', { id: 'groups-list-error', role: 'status', 'data-error-code': view.readErrors.list?.code }, errorText(view.readErrors.list)),
                        e('ul', { id: 'groups-list' }, filtered.slice(page * 25, page * 25 + 25).map(group =>
                            e('li', { key: group.id }, button(undefined, `${group.name}${group.built_in ? ' (built-in)' : ''}`,
                                () => {
                                    this.setState({ memberConfirmation: '' });
                                    void controller.select(group.id);
                                }, false, { 'data-group-id': group.id, 'aria-pressed': view.selectedId === group.id })))),
                        button('groups-prev', 'Previous page', () => this.setState({ page: page - 1 }), page === 0),
                        button('groups-next', 'Next page', () => this.setState({ page: page + 1 }), page === lastPage)),
                    e('section', { 'aria-label': 'Create group' },
                        e('h3', null, 'Create group'),
                        e('form', { id: 'groups-create-form', noValidate: true, onSubmit: event => {
                            event.preventDefault();
                            void this.act(() => controller.create(), error => /description/i.test(error.message)
                                ? this.createDescription : this.createName);
                        } }, metadata('create', view.createDraft, controller.setCreateDraft, false),
                        e('button', { id: 'groups-create', type: 'submit', disabled: busy }, 'Create group'))),
                    e('section', { 'aria-label': 'Selected group' },
                        e('h3', { id: 'groups-selected-title' }, detail ? detail.name :
                            view.selectedId ? 'Loading selected group…' : 'Select a group'),
                        e('p', { id: 'groups-detail-error', role: 'status', 'data-error-code': view.readErrors.detail?.code }, errorText(view.readErrors.detail)),
                        detail && e('div', { id: 'groups-detail' },
                            e('p', null, `Group ID ${detail.id}; created ${detail.created_at}`),
                            detail.built_in && e('p', { id: 'groups-builtin-policy' },
                                'Built-in group: all metadata fields are read-only; deletion is unavailable.'),
                            e('form', { id: 'groups-edit-form', noValidate: true, onSubmit: event => {
                                event.preventDefault();
                                void this.act(() => controller.update(), error => /description/i.test(error.message)
                                    ? this.editDescription : this.editName);
                            } }, metadata('edit', view.editDraft, controller.setEditDraft, !policy.editMetadata),
                            e('button', { id: 'groups-update', type: 'submit', disabled: busy || !policy.editMetadata },
                                'Save group metadata')),
                            e('p', null, 'Other administrators may change these details. Saving does not check for changes made since you opened this group. Refresh and review after a conflict.'),
                            field('groups-delete-name', `Type ${detail.name} to confirm permanent deletion`,
                                view.deleteConfirmation,
                                event => controller.setDeleteConfirmation(event.currentTarget.value),
                                { readOnly: !policy.deleteGroup }),
                            button('groups-delete', 'Confirm permanent group deletion', () => {
                                void controller.remove();
                            }, busy || !policy.deleteGroup || view.deleteConfirmation !== detail.name),
                            e('h4', null, 'Explicit membership'),
                            e('p', { id: 'groups-member-count' }, `Explicit members: ${detail.member_count}`),
                            e('p', { id: 'groups-member-help' }, 'Enter the complete user ID (UUID) you intend to change. The member count does not identify individual users.'),
                            policy.virtualMembership && e('p', { id: 'groups-virtual-membership' },
                                'Everyone membership is automatic. This explicit-member count is not a count of all identities; individual member changes are unavailable.'),
                            field('groups-member-id', 'User ID', view.memberId, event => {
                                controller.setMemberId(event.currentTarget.value);
                                this.setState({ memberConfirmation: '' });
                            }, { readOnly: !policy.editMembers, 'aria-describedby': 'groups-member-help',
                                ref: element => { this.memberInput = element; } }),
                            detail.name === 'admin' && field('groups-admin-member-confirm',
                                'Repeat User ID to confirm an admin membership change', this.state.memberConfirmation,
                                event => this.setState({ memberConfirmation: event.currentTarget.value })),
                            button('groups-member-add', 'Add explicit member', () => {
                                void this.act(() => controller.addMember(), () => this.memberInput);
                            }, busy || !policy.editMembers || !memberConfirmed),
                            button('groups-member-remove', 'Remove explicit member', () => {
                                void this.act(() => controller.removeMember(), () => this.memberInput);
                            }, busy || !policy.editMembers || !memberConfirmed))),
                    e('p', { id: 'groups-form-error', role: 'alert', 'data-error-code': view.formError?.code }, errorText(view.formError))),
                e('section', { id: 'groups-attempts', 'aria-label': 'Recent changes' },
                    e('h3', null, 'Recent changes'),
                    busy && e('p', { role: 'status' }, 'A change is being sent. Please wait.'),
                    view.attempts.map(attempt => e('article', { key: attempt.requestId,
                        'data-request-id': attempt.requestId, 'data-outcome': attempt.status,
                        'data-reconciliation': attempt.reconciliation, 'data-error-code': attempt.error?.code },
                    e('p', null, `${operationText(attempt.operation)} — ${outcomeText(attempt.status)}`),
                    e('p', null, `Account checked before request: ${attempt.actor.username}`),
                    e('p', { 'data-attempt-target': attempt.requestId }, targetText(attempt.target)),
                    attempt.error && e('p', { role: 'status' }, errorText(attempt.error)),
                    attempt.response && e('p', { 'data-acknowledgement': attempt.requestId }, 'The server acknowledged this change.'),
                    attempt.status === 'unknown' && e('p', null,
                        'Outcome unknown. Do not retry automatically. Checking current state cannot confirm whether this request completed; a member count cannot confirm membership.'),
                    attempt.reconciliation !== 'none' && e('p', null,
                        attempt.reconciliation === 'pending' ? 'Checking current group state…' :
                            attempt.reconciliation === 'complete' ? 'Current state checked. This does not determine the outcome of the earlier request.' :
                                'Could not check the current group state.'),
                    attempt.reconciliationError && e('p', { role: 'status' }, errorText(attempt.reconciliationError)),
                    attempt.observed && e('p', { 'data-observed-state': attempt.requestId }, observedText(attempt.observed)),
                    attempt.status === 'unknown' && button(`groups-observe-${attempt.requestId}`,
                        'Check current state', () => { void controller.reconcile(attempt.requestId); },
                        attempt.reconciliation === 'pending')))));
        }
    };
}
