const APP_ID = /^[a-z][a-z0-9-]{1,63}$/;
const PANEL_ID = /^[a-z][a-z0-9_-]{0,63}$/;

function boundedString(value, max) {
    return typeof value === 'string' && value.length > 0 && [...value].length <= max;
}

export function normalizeCatalog(value) {
    if (!value || value.schema_version !== 1 || !Array.isArray(value.applications)) {
        throw new TypeError('invalid application catalog');
    }
    const seenApplications = new Set();
    const applications = [];
    for (const application of value.applications) {
        if (!application || typeof application.id !== 'string' || !APP_ID.test(application.id) ||
            seenApplications.has(application.id) ||
            !boundedString(application.generation, 256) || !boundedString(application.version, 128) ||
            !boundedString(application.title, 128) || !Array.isArray(application.panels)) {
            throw new TypeError('invalid application catalog entry');
        }
        seenApplications.add(application.id);
        const seenPanels = new Set();
        const panels = application.panels.map(panel => {
            if (!panel || typeof panel.id !== 'string' || !PANEL_ID.test(panel.id) ||
                seenPanels.has(panel.id) ||
                !boundedString(panel.title, 256) || !boundedString(panel.module_url, 2048) ||
                !panel.module_url.startsWith('/ext/')) {
                throw new TypeError('invalid panel catalog entry');
            }
            seenPanels.add(panel.id);
            return Object.freeze({ id: panel.id, title: panel.title,
                icon: typeof panel.icon === 'string' ? panel.icon : null,
                module_url: panel.module_url });
        });
        if (!panels.length) throw new TypeError('application has no panels');
        applications.push(Object.freeze({
            id: application.id,
            generation: application.generation,
            version: application.version,
            title: application.title,
            description: typeof application.description === 'string' ? application.description : '',
            icon: typeof application.icon === 'string' ? application.icon : null,
            panels: Object.freeze(panels),
        }));
    }
    return Object.freeze(applications);
}

export function normalizeLauncherPreference(value) {
    const empty = { version: 1, last_application: null, last_panels: {}, application_order: [] };
    if (!value || typeof value !== 'object' || Array.isArray(value) || value.version !== 1) return empty;
    const lastPanels = {};
    if (value.last_panels && typeof value.last_panels === 'object' && !Array.isArray(value.last_panels)) {
        for (const [application, panel] of Object.entries(value.last_panels).slice(0, 256)) {
            if (APP_ID.test(application) && typeof panel === 'string' && PANEL_ID.test(panel)) {
                lastPanels[application] = panel;
            }
        }
    }
    const order = [];
    const seen = new Set();
    if (Array.isArray(value.application_order)) {
        for (const application of value.application_order.slice(0, 256)) {
            if (typeof application === 'string' && APP_ID.test(application) && !seen.has(application)) {
                seen.add(application);
                order.push(application);
            }
        }
    }
    return {
        version: 1,
        last_application: typeof value.last_application === 'string' && APP_ID.test(value.last_application)
            ? value.last_application : null,
        last_panels: lastPanels,
        application_order: order,
    };
}

export function orderApplications(applications, preference) {
    const positions = new Map(preference.application_order.map((id, index) => [id, index]));
    return [...applications].sort((left, right) => {
        const leftPosition = positions.get(left.id);
        const rightPosition = positions.get(right.id);
        if (leftPosition !== undefined && rightPosition !== undefined) return leftPosition - rightPosition;
        if (leftPosition !== undefined) return -1;
        if (rightPosition !== undefined) return 1;
        return left.id < right.id ? -1 : left.id > right.id ? 1 : 0;
    });
}

export function choosePanel(application, preference) {
    const remembered = preference.last_panels[application.id];
    return application.panels.find(panel => panel.id === remembered) || application.panels[0];
}

export function catalogTargetKeys(applications) {
    const keys = new Set();
    for (const application of applications) {
        for (const panel of application.panels) keys.add(`${application.generation}\u0000${panel.id}`);
    }
    return keys;
}

export function makeTarget(application, panel) {
    const prefix = `launcher-${application.id}-${panel.id}`;
    return {
        applicationId: application.id,
        generation: application.generation,
        version: application.version,
        panel,
        tabId: application.panels.length > 1 ? `${prefix}-tab` : 'launcher-app-identity',
        paneId: `${prefix}-pane`,
    };
}

export function updatePreference(preference, applicationId, panelId) {
    const lastPanels = { ...preference.last_panels, [applicationId]: panelId };
    const entries = Object.entries(lastPanels);
    const boundedPanels = Object.fromEntries(entries.length > 256 ? entries.slice(-256) : entries);
    return {
        version: 1,
        last_application: applicationId,
        last_panels: boundedPanels,
        application_order: [...preference.application_order],
    };
}
