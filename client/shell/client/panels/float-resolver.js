import { canonicalContentType, logicalFloat, pinnedFloatRequest } from './float-wire.js';

export class FloatOpenError extends Error {
    constructor(code) { super('Float could not be opened'); this.name = 'FloatOpenError'; this.code = code; }
}
const fresh = (choice, type) => ({ ...logicalFloat({ application_id: choice.application_id,
    panel_id: choice.panel_id }), kind: 'float', expected_generation: null, content_type: type });

// No manager/frame is retained in asynchronous resolution. Ports return only
// strictly decoded complete data; an exception is UNKNOWN, never absence.
export function createFloatResolver(transport) {
    async function lookup(choice, type, signal) {
        const [result] = await transport.admit([fresh(choice, type)], { signal });
        return result.status === 'available' ? result.target : null;
    }
    return Object.freeze({
        async resolveAutomatic(contentType, { signal } = {}) {
            const type = canonicalContentType(contentType);
            const defaults = await transport.personalDefaults({ signal });
            const personal = defaults.find(choice => choice.content_type === type);
            if (personal) {
                const target = await lookup(personal, type, signal);
                if (target) return target; // In particular: NO administrator/catalog request.
            }
            const page = await transport.catalog({ content_type: type, limit: 1, include_admin_default: true }, { signal });
            if (page.admin_default_candidate) {
                const target = await lookup(page.admin_default_candidate, type, signal);
                if (target) return target;
            }
            if (!page.targets.length) {
                if (page.next_cursor !== null) throw new FloatOpenError('authority-unavailable');
                throw new FloatOpenError('missing-handler');
            }
            return page.targets[0];
        },
        listManual(contentType, cursor = null, { signal } = {}) {
            return transport.catalog({ content_type: canonicalContentType(contentType), cursor, limit: 64 }, { signal });
        },
        async selectManual(choice, contentType, { signal } = {}) {
            const target = await lookup(choice, canonicalContentType(contentType), signal);
            if (!target) throw new FloatOpenError('missing-handler');
            return target;
        },
        async revalidate(target, { signal } = {}) {
            const [result] = await transport.admit([pinnedFloatRequest(target)], { signal });
            if (result.status !== 'available') throw new FloatOpenError('target-unavailable');
            return result.target;
        },
    });
}
