-- Intentional PG syntax error — triggers MIGRATING-stage failure on
-- upgrade. Mirrors install_lifecycle/fail-migration but for the upgrade
-- path: the base v1.2.3 install applies 001_init.sql cleanly; the
-- upgrade attempts 002 and fails. ICD-0.4.5 §X.06 expects the new row
-- → INSTALL_FAILED with kind upgrade-migration-failed and the old row
-- left ACTIVE. Phrased without `CREATE TABLE` so the static
-- qualified-DDL guard (ICD-0.4.3) doesn't pre-empt PG's parse-error
-- path.
THIS IS NOT VALID SQL AT ALL;
