"""Bounded reviews of actual scanner messages; request details never leave here.

The context contains the reviewed candidate's public bytes and disposable fake
account, not scanner-supplied assertions. A predicate must hold for every member
of a present group. A changed response or new observation remains unreviewed.
"""

from __future__ import annotations

import base64
from dataclasses import dataclass, field
import hashlib
import hmac
from http.cookies import SimpleCookie
import json
import re
from urllib.parse import urlsplit

from dast_report import _alert_key, _url


@dataclass(frozen=True)
class ReviewContext:
    username: str
    password: str
    app_document: str
    assets: dict[str, str]
    csp: str
    # Independent disposable-database receipt; hashes never become public.
    session_owners: dict[str, tuple[str, str]] = field(default_factory=dict)


# Individually reviewed public comments about prepaint, preferences, runtime
# configuration and ownership. Full candidate asset equality is also required.
# These are excerpts, not general keyword or JavaScript-comment exemptions.
COMMENT_DIGESTS = {
    "prepaint.js": frozenset({
        "15260d174f2811ed8d327d1191f31a7ccbe82daa4c7493fa07d5480c1bc2fb9d",
        "484380871796f1ed03e06e59e99aeee9d1ca15e3ac2fc0ef66a8180155808dcf",
    }),
    "shell.js": frozenset({
        "dfcb5da8cf92e743b40efc0e6187b5ba2a2786696dc678d9b19238875ea0c168",
        "2f311734e5ba5b0e03cf915b707bd8ed82a0b07d577b7fca852409dd608f0ead",
    }),
    "runtime-config.js": frozenset({
        "99844512c709ee6b4b0876da0061f42faaa0ca1f729b3c78ca885d9cdfb5babc",
        "cdc27546056f32471590b52d0bbc4bb1329066d153412f54765fa7c46b330eb6",
    }),
    "sdk.js": frozenset({
        "eb64d4a52df7400ec5f7c9ef731d1c7391dd889fc84f094ff828d442192b30d9",
    }),
}


def _headers(raw):
    if type(raw) is not str or not raw.endswith("\r\n\r\n"):
        return None
    lines = raw.split("\r\n")
    if not lines[0] or re.search(r"[\x00-\x1f\x7f]", lines[0]):
        return None
    fields = {}
    ended = False
    for line in lines[1:]:
        if not line:
            ended = True
            continue
        name, separator, value = line.partition(":")
        if (ended or not separator or
                not re.fullmatch(r"[!#$%&'*+.^_`|~0-9A-Za-z-]+", name) or
                re.search(r"[\x00-\x08\x0a-\x1f\x7f]", value)):
            return None
        fields.setdefault(name.lower(), []).append(value.strip(" \t"))
    return lines[0], fields


def _single(headers, name):
    values = headers.get(name, [])
    return values[0] if len(values) == 1 else None


def _message(alert, message, origin):
    if type(message) is not dict:
        return None
    request = _headers(message.get("requestHeader"))
    response = _headers(message.get("responseHeader"))
    if not request or not response or type(message.get("responseBody")) is not str:
        return None
    method, separator, remainder = request[0].partition(" ")
    target, separator2, protocol = remainder.partition(" ")
    if (not separator or not separator2 or method != alert.get("method") or
            method not in {"GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"} or
            protocol not in {"HTTP/1.0", "HTTP/1.1", "HTTP/2"}):
        return None
    request_url = target if target.startswith("https://") else origin + target
    if (not target.startswith("https://") and
            (not target.startswith("/") or target.startswith("//"))):
        return None
    parsed = _url(request_url)
    if parsed is None or parsed.netloc != urlsplit(origin).netloc or request_url != alert.get("url"):
        return None
    if _single(request[1], "host") != parsed.netloc:
        return None
    status = re.fullmatch(r"HTTP/\d(?:\.\d)? ([1-5][0-9]{2})(?: [^\r\n]*)?", response[0])
    evidence = alert.get("evidence")
    if status is None or type(evidence) is not str:
        return None
    # Ambiguous security headers do not constitute evidence for an exception.
    if any(len(values) != 1 for name, values in response[1].items()
           if name in {"content-type", "cache-control", "vary", "content-security-policy"}):
        return None
    return method, parsed.path, int(status[1]), request[1], response[1], message["responseBody"]


def _cookies(headers, response=False):
    values = headers.get("set-cookie" if response else "cookie", [])
    if not response and len(values) != 1:
        return {}
    result = {}
    for line in values:
        if not response:
            names = [item.partition("=")[0].strip() for item in line.split(";")]
            if len(names) != len(set(names)):
                return {}
        parsed = SimpleCookie()
        try:
            parsed.load(line)
        except Exception:
            return {}
        if not parsed or (response and len(parsed) != 1):
            return {}
        for name, cookie in parsed.items():
            if name in result:
                return {}
            result[name] = cookie
    return result


def _cookie_evidence(headers, name, evidence):
    if not evidence:
        return False
    prefix, separator, value = evidence.partition(":")
    full_header = bool(separator and prefix.lower() == "set-cookie")
    for line in headers.get("set-cookie", []):
        cookie = _cookies({"set-cookie": [line]}, True)
        if name in cookie and (value.strip() == line if full_header else evidence in line):
            return True
    return False


def _reject_constant(_value):
    raise ValueError("nonstandard JSON constant")


def _unique_json(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON field")
        result[key] = value
    return result


def _json(body):
    try:
        return json.loads(body, object_pairs_hook=_unique_json, parse_constant=_reject_constant)
    except (ValueError, TypeError):
        return None


def _owner(token, context):
    if type(token) is not str or not token:
        return None
    owner = context.session_owners.get(hashlib.sha256(token.encode()).hexdigest())
    if (type(owner) not in {tuple, list} or len(owner) != 2 or
            any(type(value) is not str or not re.fullmatch(
                r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}", value)
                for value in owner)):
        return None
    return owner


def _identity(body, context, token):
    value = _json(body)
    owner = _owner(token, context)
    return (owner is not None and type(value) is dict and type(value.get("user")) is dict and
            value["user"].get("username") == context.username and
            value["user"].get("id") == owner[0] and
            type(value.get("session")) is dict and
            value["session"].get("id") == owner[1])


def _csrf_value(token):
    return base64.urlsafe_b64encode(hmac.new(
        token.encode(), b"plinth.csrf.v1", hashlib.sha256).digest()).rstrip(b"=").decode()


def _private_cache(headers):
    control = _single(headers, "cache-control")
    vary = _single(headers, "vary")
    return (control is not None and "no-store" in {item.strip().lower() for item in control.split(",")} and
            vary is not None and {"cookie", "authorization"}.issubset(
                {item.strip().lower() for item in vary.split(",")}))


def _style_policy(policy):
    if type(policy) is not str:
        return False
    directives = {}
    for part in policy.split(";"):
        tokens = part.split()
        if not tokens or tokens[0] in directives:
            return False
        directives[tokens[0]] = tokens[1:]
    expected = {
        "default-src": ["'self'"], "style-src": ["'self'", "'unsafe-inline'"],
        "connect-src": ["'self'"], "img-src": ["'self'"], "font-src": ["'self'"],
        "media-src": ["'self'"], "frame-src": ["'self'"],
        "frame-ancestors": ["'self'"], "base-uri": ["'self'"],
        "form-action": ["'self'"], "object-src": ["'none'"],
    }
    scripts = directives.pop("script-src", None)
    return (directives == expected and scripts is not None and len(scripts) == 2 and
            scripts[0] == "'self'" and
            scripts[1] == "'sha256-cCDc4AaNiyEAbj29NffEKnWAezVHyPJNEKKLUd8ZTkw='")


def _upgrade(request, response):
    key = _single(request, "sec-websocket-key")
    if (key is None or not re.fullmatch(r"[A-Za-z0-9+/]{22}==", key) or
            _single(request, "sec-websocket-version") != "13"):
        return False
    for headers in (request, response):
        connection = _single(headers, "connection")
        if (_single(headers, "upgrade") != "websocket" or connection is None or
                "upgrade" not in {item.strip().lower() for item in connection.split(",")}):
            return False
    expected = base64.b64encode(hashlib.sha1(
        (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode(), usedforsecurity=False).digest()).decode()
    return _single(response, "sec-websocket-accept") == expected


def _predicate(key, alert, message, context, origin):
    joined = _message(alert, message, origin)
    if joined is None:
        return None
    plugin, risk, confidence, route, param, reference = key
    method, path, status, request, response, body = joined
    evidence = alert["evidence"]
    response_cookies = _cookies(response, True)
    request_cookies = _cookies(request)
    credential = (response_cookies.get("plinth_session") if route == "LOGIN" else
                  request_cookies.get("plinth_session"))
    token = credential.value if credential is not None else ""
    if route == "SESSION" and path != "/api/auth/session":
        return None
    if (plugin == 10010 and reference == "10010" and risk == 1 and confidence == 2 and
            route in {"LOGIN", "SESSION"} and param == "plinth_csrf" and status == 200 and
            ((route == "LOGIN" and method == "POST") or (route == "SESSION" and method == "GET"))):
        csrf = response_cookies.get("plinth_csrf")
        session = (response_cookies.get("plinth_session") if route == "LOGIN" else
                   request_cookies.get("plinth_session"))
        if csrf and session and session.value and csrf.value:
            expected_csrf = _csrf_value(session.value)
            live_session_safe = (route != "LOGIN" or
                                 bool(session["secure"]) and bool(session["httponly"]) and
                                 session["samesite"].lower() == "strict" and not session["domain"] and
                                 session["path"] == "/" and
                                 bool(re.fullmatch(r"[1-9][0-9]{0,8}", session["max-age"])))
            if (csrf.value == expected_csrf and not csrf["httponly"] and csrf["secure"] and
                    csrf["samesite"].lower() == "strict" and not csrf["domain"] and csrf["path"] == "/" and
                    live_session_safe and _identity(body, context, session.value) and _private_cache(response) and
                    (route != "SESSION" or "plinth_session" not in response_cookies) and
                    _cookie_evidence(response, param, evidence)):
                return "DOCUMENTED_POLICY", "CSRF_READABLE_COOKIE"
    if (reference in {"10010", "10011", "10054-1"} and plugin in {10010, 10011, 10054} and
            risk == 1 and confidence == 2 and route == "LOGOUT" and method == "POST" and
            status == 200 and param in {"plinth_session", "plinth_csrf"}):
        cookie = response_cookies.get(param)
        if (cookie is not None and cookie.value == "" and cookie["max-age"] == "0" and
                cookie["path"] == "/" and not cookie["domain"] and
                _json(body) == {"status": "logged_out"} and _private_cache(response) and
                _single(request, "origin") == origin and
                "plinth_session" in request_cookies and "plinth_csrf" in request_cookies and
                _owner(request_cookies["plinth_session"].value, context) is not None and
                request_cookies["plinth_csrf"].value == _csrf_value(request_cookies["plinth_session"].value) and
                _single(request, "x-plinth-csrf") == request_cookies["plinth_csrf"].value and
                _cookie_evidence(response, param, evidence)):
            return "NOT_APPLICABLE", "EXPIRED_COOKIE_DELETION"
    if (plugin == 10015 and reference == "10015" and risk == 0 and confidence == 1 and status == 200 and
            (not evidence or evidence == _single(response, "cache-control"))):
        if route == "APP" and method == "GET" and body == context.app_document and (
                _single(response, "cache-control") == "no-cache"):
            return "INFORMATIONAL", "PUBLIC_SHELL_REVALIDATION"
        if route == "HEALTH" and method == "GET" and _json(body) == {"status": "ok"}:
            return "INFORMATIONAL", "PUBLIC_CONSTANT_HEALTH"
        if route in {"REGISTRATION", "SESSION", "APPLICATIONS"} and method == "GET" and _private_cache(response):
            parsed = _json(body)
            if ((route == "REGISTRATION" and parsed == {"mode": "disabled"}) or
                    (route == "SESSION" and _identity(body, context, token)) or
                    (route == "APPLICATIONS" and type(parsed) in {dict, list} and
                     _owner(token, context) is not None)):
                return "INFORMATIONAL", "PRIVATE_RESPONSE_NO_STORE"
    if (plugin == 2 and reference == "2" and risk == 1 and confidence == 2 and
            route == "SESSION" and method == "GET" and status == 200 and
            _identity(body, context, token) and _private_cache(response)):
        if evidence == _json(body)["session"].get("ip_address"):
            return "FALSE_POSITIVE", "AUTHORIZED_OWN_SESSION_METADATA"
    if (plugin == 10027 and reference == "10027" and risk == 0 and confidence == 2 and
            route == "STATIC" and method == "GET" and status == 200 and
            context.assets.get(path) == body and evidence and evidence in body and
            hashlib.sha256(evidence.encode()).hexdigest() in COMMENT_DIGESTS.get(path.rsplit("/", 1)[-1], ())):
        return "INFORMATIONAL", "REVIEWED_PUBLIC_SOURCE_COMMENT"
    if (plugin == 10109 and reference == "10109" and risk == 0 and confidence == 2 and
            route == "APP" and method == "GET" and status == 200 and
            body == context.app_document and evidence and evidence in body):
        return "INFORMATIONAL", "SCANNER_APPLICATION_DISCOVERY"
    if (plugin == 10111 and reference == "10111" and risk == 0 and confidence == 3 and
            route == "LOGIN" and method == "POST" and status == 200 and _identity(body, context, token)):
        payload = message.get("requestBody")
        if (_json(payload) == {"username": context.username, "password": context.password} and
                evidence and evidence in payload and "plinth_session" in response_cookies):
            return "INFORMATIONAL", "SCANNER_AUTHENTICATION_DISCOVERY"
    if (plugin == 10112 and reference == "10112" and risk == 0 and confidence in {2, 3} and
            route in {"LOGIN", "SESSION"} and status == 200 and
            ((route == "LOGIN" and method == "POST") or (route == "SESSION" and method == "GET")) and
            param in {"plinth_session", "plinth_csrf"} and _identity(body, context, token) and
            param in response_cookies and response_cookies[param].value and
            _cookie_evidence(response, param, evidence)):
        return "INFORMATIONAL", "SCANNER_SESSION_DISCOVERY"
    if (plugin == 10038 and reference == "10038-1" and risk == 2 and confidence == 3 and
            route == "WS" and method == "GET" and status == 101 and body == "" and
            _single(request, "origin") == origin and
            _upgrade(request, response)):
        return "NOT_APPLICABLE", "WEBSOCKET_UPGRADE_NO_DOCUMENT"
    if (plugin == 10055 and reference == "10055-6" and risk == 2 and confidence == 3 and
            route == "APP" and method == "GET" and status == 200 and body == context.app_document and
            _single(response, "content-security-policy") == context.csp and _style_policy(context.csp) and
            param.lower() == "content-security-policy" and evidence == context.csp):
        return "DOCUMENTED_POLICY", "CSP_STYLES_ONLY"
    return None


def reviewed_dispositions(alerts, messages, origin, context):
    """Review present groups only; any unmatched member rejects its whole group.

    All URL, parameter and credential values are matching-only. The caller must
    keep the returned records private; public_report emits finite identifiers.
    """
    target = _url(origin, origin=True)
    if (target is None or type(alerts) is not list or type(messages) is not list or
            not isinstance(context, ReviewContext) or not context.username or not context.password or
            not context.app_document or type(context.assets) is not dict):
        return []
    inventory = {}
    for message in messages:
        if (type(message) is not dict or type(message.get("id")) is not str or
                not re.fullmatch(r"[1-9][0-9]{0,9}", message["id"]) or message["id"] in inventory):
            return []
        inventory[message["id"]] = message
    groups = {}
    for alert in alerts:
        key = _alert_key(alert, target)
        if key is None:
            continue
        groups.setdefault(key, []).append(alert)
    result = []
    for key, members in sorted(groups.items()):
        proofs = [_predicate(key, member, inventory.get(member.get("messageId")), context, origin)
                  for member in members]
        if not proofs or proofs[0] is None or any(proof != proofs[0] for proof in proofs):
            continue
        plugin, risk, confidence, route, param, reference = key
        disposition, review = proofs[0]
        result.append({"pluginId": plugin, "risk": risk, "confidence": confidence,
                       "routeLabel": route, "param": param, "alertRef": reference,
                       "dispositionId": disposition, "reviewId": review})
    return result
