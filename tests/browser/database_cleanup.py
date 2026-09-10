"""Remove a disposable database and its database-scoped extension logins."""

import subprocess


def drop_database(database, pg_env):
    def sql(statement):
        result = subprocess.run(
            ["psql", "-XAt", "-v", "ON_ERROR_STOP=1", "-c", statement],
            env=pg_env, check=True, timeout=20, text=True, capture_output=True)
        return result.stdout.strip()

    literal = "'" + database.replace("'", "''") + "'"
    roles = sql(
        "SELECT DISTINCT r.rolname FROM pg_roles r JOIN pg_shdepend d "
        "ON d.refclassid='pg_authid'::regclass AND d.refobjid=r.oid "
        "JOIN pg_database db ON db.oid=d.dbid WHERE db.datname=" + literal +
        " AND r.rolname ~ '^px_[0-9a-f]{60}$'").splitlines()
    identifier = '"' + database.replace('"', '""') + '"'
    sql("DROP DATABASE " + identifier + " WITH (FORCE)")
    # Legacy role aliases are cluster-wide and may belong to another test or
    # installation. Only the captured database-scoped runtime logins are ours.
    for role in roles:
        sql('DROP ROLE "' + role.replace('"', '""') + '"')
