# Security policy

## Supported versions

Plinth is pre-1.0. Only the latest release and current `main` receive security
fixes. Older releases and private-development snapshots are unsupported.

## Reporting a vulnerability

Do not open a public issue for a suspected vulnerability. Use GitHub's
**Report a vulnerability** action on the repository Security page to create a
private advisory. Include affected versions, reproduction conditions, impact,
and a minimal proof of concept without real credentials or personal data.

The maintainer will acknowledge a complete report as capacity permits, assess
severity and affected versions, coordinate a fix and disclosure, and credit
reporters who want attribution. This project does not currently offer a bug
bounty or guaranteed response time.

## Deployment posture

The built-in server does not terminate TLS. Defaults bind to loopback and keep
registration disabled. Follow [configuration and deployment guidance](docs/CONFIGURATION.md),
keep secrets outside the repository, and place an authenticated TLS reverse
proxy or equivalent controlled ingress in front of any networked deployment.
