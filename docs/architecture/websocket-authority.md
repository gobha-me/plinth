# WebSocket authority lifetime

A WebSocket authenticates its credential and reads its user and effective rules
from PostgreSQL before announcing `connected`. That snapshot has a lease of at
most two seconds, capped by the credential's remaining lifetime. The deadline
uses the node's monotonic clock and starts before submitting the query, so
connection-pool wait, query time, and event-loop delay consume the lease.

Every node renews its own sockets once per second using the same database. A
revoked or expired session/PAT, disabled user, changed username, or changed rule
set invalidates authority and closes the socket with `auth_failed` (4002). A
client must authenticate a new socket to acquire a changed rule set. The
connection's original identity and rules remain immutable; replay copies share
an atomic lease deadline rather than a mutable permission snapshot.

New calls and subscriptions, live delivery, replay frame sends and buffered
replay flushes check the deadline. Database unavailability or delayed renewal
cannot extend it. No new operation or event delivery is authorized more than
two seconds after a database revocation commits. Work already admitted may
finish; this contract does not retroactively cancel committed side effects or
recall bytes already sent. Credential expiry can invalidate authority earlier.

Initial validation and renewal use the Drogon-owned `ws_authority` database
pool with two connections, a one-second queue/query callback timeout, and a
PostgreSQL statement timeout of one second. Completion transfers its async
lifecycle lease onto the connection's loop exactly once. Late database closures
cannot retain that lease or schedule another completion after timeout. Timers
and outstanding replay work are sealed by connection close and the production
shutdown coordinator before event loops and database clients are destroyed.

This replaces the earlier connection-lifetime cache of authentication and RBAC.
