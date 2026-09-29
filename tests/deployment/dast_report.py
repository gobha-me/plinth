"""Fail-closed, secret-free public summaries of privately reviewed DAST evidence.

This module performs no I/O. Raw scanner records and ``targetOrigin`` are
matching-only inputs: neither free text nor request details enter the report.
Changing the finite policy identifiers requires a deliberate code review.
"""

from __future__ import annotations

import re
from urllib.parse import urlsplit


SCANNER_VERSION = "2.17.0"
_MAX_SAFE_INTEGER = (1 << 53) - 1
REQUIRED_CONTROLS = frozenset({
    "HTTP_SURFACE", "BROWSER_AUTH", "WS_UPGRADE", "SECURITY_HEADERS",
    "RATE_LIMITS", "REQUEST_LIMITS", "TLS_AUTHORITY", "CONCURRENCY_LIMITS",
    "INGRESS_MONITOR",
})
REQUIRED_ROUTES = frozenset({
    "ROOT", "APP", "SESSION", "LOGIN", "LOGOUT", "REGISTER",
    "REGISTRATION", "CAP", "WS", "PACKAGES", "HEALTH",
})
_SEGMENT = r"[A-Za-z0-9_.@+-]+"
_ROUTES = {
    "ROOT": r"/",
    "APP": r"/app/?",
    "SESSION": rf"/api/auth/session(?:/{_SEGMENT})?",
    "LOGIN": r"/api/auth/login",
    "LOGOUT": r"/api/auth/logout",
    "REGISTER": r"/api/auth/register",
    "REGISTRATION": r"/api/auth/registration",
    "CAP": r"/api/cap/[A-Za-z0-9_.:-]+",
    "WS": r"/ws/events",
    "PACKAGES": rf"/api/packages(?:/{_SEGMENT})?",
    "HEALTH": r"/healthz",
    "SDK": r"/api/frontend/sdk\.js",
    "APPLICATIONS": r"/api/frontend/applications",
    "STATIC": rf"/ext/{_SEGMENT}/{_SEGMENT}/(?:{_SEGMENT}/)*{_SEGMENT}",
    "BOOTSTRAP": r"/api/auth/bootstrap",
    "INVITES": rf"/api/auth/invites(?:/{_SEGMENT})?",
    "RECOVERY": r"/api/auth/recovery",
    "PATS": rf"/api/auth/pats(?:/{_SEGMENT})?",
    "GROUPS": rf"/api/groups(?:/{_SEGMENT})?(?:/(?:members|rules)(?:/{_SEGMENT})?)?",
    "SESSIONS": r"/api/auth/sessions",
    # Named negative fixtures only, not a general unknown/traversal route.
    "ERROR_PROBE": (
        r"(?:/app/\.\./\.\./config\.json|/api/unknown|"
        r"/ext/shell/(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\."
        r"(?:0|[1-9][0-9]*)/\.\./\.\./config\.json)"
    ),
}
ROUTE_LABELS = frozenset(_ROUTES)
REVIEW_IDS = frozenset({
    "CSRF_READABLE_COOKIE", "CSRF_HEADER_VERIFIED",
    "CSP_FRAME_ANCESTORS_ENFORCED", "CSP_STYLES_ONLY",
    "IMMUTABLE_STATIC_CACHE", "PRIVATE_FIX_REQUIRED",
})
_IDENTITY_KEYS = frozenset({
    "sourceRevision", "sourceTree", "imageDigest", "scannerDigest", "scannerVersion",
})
_SCANNER_KEYS = frozenset({
    "completed", "version", "passiveRecordsRemaining", "httpMessages",
    "observedRoutes", "targetOrigin",
})
_CLEANUP_BOOLEANS = frozenset({
    "verified", "namespaceAbsent", "clusterAbsent", "registryAbsent",
    "scannerContainerAbsent", "temporaryFilesAbsent",
})
_CLEANUP_COUNTERS = frozenset({
    "remainingContainers", "remainingNetworks", "remainingVolumes",
})
_REVIEW_KEYS = frozenset({
    "pluginId", "alertRef", "risk", "confidence", "routeLabel", "param", "dispositionId", "reviewId",
})
_RISK_NAMES = {"Informational": 0, "Low": 1, "Medium": 2, "High": 3}
_CONFIDENCE_NAMES = {
    "False Positive": 0, "Low": 1, "Medium": 2, "High": 3, "Confirmed": 4,
}


def _record(value, keys):
    return type(value) is dict and value.keys() == keys


def _integer(value, minimum=0):
    return type(value) is int and minimum <= value <= _MAX_SAFE_INTEGER


def _numeric(value, maximum=None, names=None):
    if type(value) is str:
        if names and value in names:
            value = names[value]
        elif re.fullmatch(r"0|[1-9][0-9]*", value):
            # Avoid arbitrary-length untrusted integer conversion.
            if len(value) > 10:
                return None
            value = int(value)
        else:
            return None
    if not _integer(value) or (maximum is not None and value > maximum):
        return None
    return value


def _url(value, *, origin=False):
    if type(value) is not str or any(ord(c) < 33 or ord(c) > 126 for c in value):
        return None
    if "\\" in value:
        return None
    try:
        parsed = urlsplit(value)
        port = parsed.port
        if (parsed.scheme != "https" or not parsed.hostname or
                parsed.username is not None or parsed.password is not None or
                parsed.netloc != parsed.netloc.lower() or
                not re.fullmatch(r"[a-z0-9](?:[a-z0-9.-]*[a-z0-9])?", parsed.hostname) or
                port is not None and not 1 <= port <= 65535 or parsed.fragment):
            return None
        if origin:
            authority = parsed.hostname + (f":{port}" if port is not None else "")
            if value != "https://" + authority or port == 443:
                return None
        return parsed
    except ValueError:
        return None


def _route(value, target):
    parsed = _url(value)
    if parsed is None or target is None or parsed.netloc != target.netloc:
        return None
    if re.fullmatch(_ROUTES["ERROR_PROBE"], parsed.path):
        return "ERROR_PROBE"
    if any(segment in (".", "..") for segment in parsed.path.split("/")):
        return None
    for label, pattern in _ROUTES.items():
        if re.fullmatch(pattern, parsed.path):
            return label
    return None


def classify_route(url, targetOrigin):
    """Return a finite scope label, or None; never return any URL detail.

    Both inputs are matching-only. Origin and named error-fixture validation is
    identical to the public report's alert validation. A label is not proof
    that the route was visited or that an alert is a false positive.
    """
    return _route(url, _url(targetOrigin, origin=True))


def _alert_reference(value, plugin):
    """Validate a matching-only rule/variant identifier, never a wildcard."""
    if type(value) is not str or not 0 < len(value) <= 64:
        return None
    prefix, separator, variant = value.partition("-")
    if prefix != str(plugin) or (separator and (
            len(variant) > 10 or not re.fullmatch(r"0|[1-9][0-9]*", variant))):
        return None
    return value


def _alert_key(record, target):
    if type(record) is not dict:
        return None
    plugin = _numeric(record.get("pluginId"))
    risk = _numeric(record.get("risk"), 3, _RISK_NAMES)
    confidence = _numeric(record.get("confidence"), 4, _CONFIDENCE_NAMES)
    param = record.get("param")
    route = _route(record.get("url"), target)
    reference = _alert_reference(record.get("alertRef"), plugin)
    if (not plugin or risk is None or confidence is None or
            type(param) is not str or not route or reference is None):
        return None
    return plugin, risk, confidence, route, param, reference


def public_report(identity, controls, scanner, raw_alerts, dispositions, cleanup):
    """Return JSON-safe fixed-schema evidence; malformed inputs never count as green.

    Review keys are ``(pluginId, risk, confidence, routeLabel, param, alertRef)``.
    The rule/variant reference remains private and matching-only. Only an exact
    explicit FALSE_POSITIVE review with a finite evidence identifier can qualify
    that variant's group. No legacy or wildcard fallback, default ignore,
    severity cutoff, or raw rationale exists.
    A review for an absent group is rejected, preventing preemptive suppressions.
    """
    reasons = set()
    report = {
        "schemaVersion": 1, "status": "INCOMPLETE", "reasons": [],
        "identity": None, "controls": [], "scanner": None,
        "alertGroups": [], "cleanup": None,
    }

    valid_identity = _record(identity, _IDENTITY_KEYS)
    if valid_identity:
        valid_identity = all(
            type(identity[key]) is str and re.fullmatch(r"[0-9a-f]{40}", identity[key]) and
            identity[key] != "0" * 40
            for key in ("sourceRevision", "sourceTree")
        ) and all(
            type(identity[key]) is str and re.fullmatch(r"sha256:[0-9a-f]{64}", identity[key]) and
            identity[key] != "sha256:" + "0" * 64
            for key in ("imageDigest", "scannerDigest")
        ) and type(identity["scannerVersion"]) is str and identity["scannerVersion"] == SCANNER_VERSION
    if valid_identity:
        report["identity"] = {key: identity[key] for key in sorted(_IDENTITY_KEYS)}
    else:
        reasons.add("INVALID_IDENTITY")

    seen = set()
    valid_controls = type(controls) is list
    for control in controls if valid_controls else []:
        if (not _record(control, {"id", "passed"}) or
                type(control["id"]) is not str or control["id"] not in REQUIRED_CONTROLS or
                control["id"] in seen or type(control["passed"]) is not bool):
            valid_controls = False
            continue
        seen.add(control["id"])
        report["controls"].append({"id": control["id"], "passed": control["passed"]})
        if not control["passed"]:
            reasons.add("CONTROL_FAILED")
    report["controls"].sort(key=lambda item: item["id"])
    if not valid_controls or seen != REQUIRED_CONTROLS:
        reasons.add("INVALID_CONTROLS")

    target = None
    valid_scanner = _record(scanner, _SCANNER_KEYS)
    if valid_scanner:
        target = _url(scanner["targetOrigin"], origin=True)
        routes = scanner["observedRoutes"]
        valid_scanner = (
            type(scanner["completed"]) is bool and type(scanner["version"]) is str and
            scanner["version"] == SCANNER_VERSION and
            _integer(scanner["passiveRecordsRemaining"]) and _integer(scanner["httpMessages"], 1) and
            type(routes) is list and all(type(item) is str and item in ROUTE_LABELS for item in routes) and
            len(set(routes)) == len(routes) and target is not None
        )
    if valid_scanner:
        report["scanner"] = {key: scanner[key] for key in sorted(_SCANNER_KEYS - {"targetOrigin"})}
        report["scanner"]["observedRoutes"] = sorted(scanner["observedRoutes"])
        if not scanner["completed"] or scanner["passiveRecordsRemaining"] != 0:
            reasons.add("SCAN_INCOMPLETE")
        if not REQUIRED_ROUTES.issubset(scanner["observedRoutes"]):
            reasons.add("MISSING_ROUTE")
    else:
        reasons.add("INVALID_SCANNER")
        target = None

    groups = {}
    if type(raw_alerts) is not list:
        reasons.add("PRIVATE_TRIAGE_REQUIRED")
    else:
        for alert in raw_alerts:
            key = _alert_key(alert, target)
            if key is None:
                reasons.add("PRIVATE_TRIAGE_REQUIRED")
            else:
                groups[key] = groups.get(key, 0) + 1

    reviews = {}
    if type(dispositions) is not list:
        reasons.add("PRIVATE_TRIAGE_REQUIRED")
    else:
        for review in dispositions:
            if not _record(review, _REVIEW_KEYS):
                reasons.add("PRIVATE_TRIAGE_REQUIRED")
                continue
            plugin = _numeric(review["pluginId"])
            risk = _numeric(review["risk"], 3, _RISK_NAMES)
            confidence = _numeric(review["confidence"], 4, _CONFIDENCE_NAMES)
            reference = _alert_reference(review["alertRef"], plugin)
            route, param = review["routeLabel"], review["param"]
            disposition, proof = review["dispositionId"], review["reviewId"]
            if (not plugin or risk is None or confidence is None or reference is None or
                    type(route) is not str or route not in ROUTE_LABELS or type(param) is not str or
                    type(proof) is not str or proof not in REVIEW_IDS or
                    type(disposition) is not str or disposition not in {
                        "FALSE_POSITIVE", "PRIVATE_REMEDIATION_REQUIRED"} or
                    (disposition == "PRIVATE_REMEDIATION_REQUIRED") != (proof == "PRIVATE_FIX_REQUIRED")):
                reasons.add("PRIVATE_TRIAGE_REQUIRED")
                continue
            key = plugin, risk, confidence, route, param, reference
            if key in reviews or key not in groups:
                reasons.add("PRIVATE_TRIAGE_REQUIRED")
                continue
            reviews[key] = disposition, proof

    for key, count in sorted(groups.items()):
        disposition, proof = reviews.get(key, ("PRIVATE_TRIAGE_REQUIRED", None))
        report["alertGroups"].append({
            "ruleId": key[0], "risk": key[1], "confidence": key[2], "count": count,
            "dispositionId": disposition, "reviewId": proof,
        })
        if disposition != "FALSE_POSITIVE":
            reasons.add("PRIVATE_TRIAGE_REQUIRED")

    valid_cleanup = _record(cleanup, _CLEANUP_BOOLEANS | _CLEANUP_COUNTERS)
    if valid_cleanup:
        valid_cleanup = all(type(cleanup[key]) is bool for key in _CLEANUP_BOOLEANS) and all(
            _integer(cleanup[key]) for key in _CLEANUP_COUNTERS
        )
    if valid_cleanup:
        report["cleanup"] = {key: cleanup[key] for key in sorted(cleanup)}
        if not all(cleanup[key] for key in _CLEANUP_BOOLEANS) or any(
                cleanup[key] != 0 for key in _CLEANUP_COUNTERS):
            reasons.add("CLEANUP_UNVERIFIED")
    else:
        reasons.add("INVALID_CLEANUP")

    report["reasons"] = sorted(reasons)
    if not reasons:
        report["status"] = "COMPLETE"
    return report
