-- Fixed platform storage contract. Existing 001 and its checksum stay intact.
-- Administrator defaults are preferences, never grants or restore approval.
CREATE TABLE ext_shell.platform_contract (
    singleton BOOLEAN PRIMARY KEY CHECK (singleton),
    storage_version INTEGER NOT NULL CHECK (storage_version = 1)
);

INSERT INTO ext_shell.platform_contract (singleton, storage_version)
VALUES (true, 1);

CREATE TABLE ext_shell.default_apps (
    content_type TEXT PRIMARY KEY,
    application_id TEXT NOT NULL,
    panel_id TEXT NOT NULL,
    CONSTRAINT chk_default_content_type CHECK (
        octet_length(content_type) BETWEEN 3 AND 128),
    CONSTRAINT chk_default_application CHECK (
        application_id ~ '^[a-z][a-z0-9-]{1,63}$'),
    CONSTRAINT chk_default_panel CHECK (
        panel_id ~ '^[a-z][a-z0-9_-]{0,63}$')
);

GRANT SELECT ON ext_shell.platform_contract TO ext_shell_role;
GRANT SELECT, INSERT, UPDATE, DELETE
    ON ext_shell.default_apps TO ext_shell_role;
