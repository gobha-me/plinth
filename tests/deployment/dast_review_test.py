"""Actual-message proof boundaries, using only disposable fake identities."""

import base64
import copy
import hashlib
import json
from pathlib import Path
import re
import unittest
from unittest.mock import patch

from dast_report import _review_class
from dast_review import COMMENT_DIGESTS, ReviewContext, _csrf_value, reviewed_dispositions


ORIGIN = "https://plinth.test:8443"
USER = "11111111-1111-4111-8111-111111111111"
SESSION = "22222222-2222-4222-8222-222222222222"
TOKEN = "fake-session-only"
CSRF = _csrf_value(TOKEN)
APP = '<html><body><div id="root"></div></body></html>'
CSP = ("script-src 'self' 'sha256-cCDc4AaNiyEAbj29NffEKnWAezVHyPJNEKKLUd8ZTkw='; "
       "default-src 'self'; style-src 'self' 'unsafe-inline'; connect-src 'self'; img-src 'self'; "
       "font-src 'self'; media-src 'self'; frame-src 'self'; frame-ancestors 'self'; "
       "base-uri 'self'; form-action 'self'; object-src 'none'")
IDENTITY = {"user": {"username": "fake-admin", "id": USER},
            "session": {"id": SESSION, "ip_address": "192.0.2.7"}}
PRIVATE_HEADERS = [("Cache-Control", "no-store"), ("Vary", "Cookie, Authorization")]
COOKIE = "plinth_session=" + TOKEN + "; plinth_csrf=" + CSRF
LIVE_SESSION = "plinth_session=" + TOKEN + "; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=86400"
LIVE_CSRF = "plinth_csrf=" + CSRF + "; Path=/; Secure; SameSite=Strict; Max-Age=86400"
ROOT = Path(__file__).resolve().parents[2]
# Exact substrings of tracked public comments, not private scanner fixtures.
# The pinned engine may retain only a suffix; the whole current asset must
# independently match, and each suffix remains filename-scoped.
PUBLIC_OWNERSHIP_COMMENTS = (
    ("interaction-owner.js", "// Fence re-entrant actions before invoking user code.",
     "ons before invoking user code.",
     "614c3f7e51ed11947694df4ab133c21be27f9566f81d39d98d7221e827ed378b"),
    ("float-chrome.js", "// references before the parent Preact frame's later unmount callback.",
     "rent Preact frame's later unmount callback.",
     "afac0b3696d45db69b08f192d63e6589b639f0c5a16eeaf871b93f770dd05b6e"),
)


def context():
    return ReviewContext("fake-admin", "fake-password", APP, {}, CSP,
                         {hashlib.sha256(TOKEN.encode()).hexdigest(): (USER, SESSION)})


def pair(plugin, path, *, ref=None, risk=0, confidence=2, param="", evidence="", method="GET",
         status=200, body=None, response=(), request=(), request_body="", mid="1"):
    alert = {"pluginId": str(plugin), "alertRef": ref or str(plugin), "risk": risk,
             "confidence": confidence, "url": ORIGIN + path, "method": method,
             "param": param, "evidence": evidence, "messageId": mid}
    message = {"id": mid, "requestHeader": method + " " + ORIGIN + path + " HTTP/1.1\r\n" +
               "Host: plinth.test:8443\r\n" + "".join(k + ": " + v + "\r\n" for k, v in request) + "\r\n",
               "responseHeader": "HTTP/1.1 " + str(status) + " Fixture\r\n" +
               "".join(k + ": " + v + "\r\n" for k, v in response) + "\r\n",
               "requestBody": request_body,
               "responseBody": json.dumps(IDENTITY) if body is None else body}
    return alert, message


def run(records, ctx=None):
    return reviewed_dispositions([item[0] for item in records], [item[1] for item in records],
                                 ORIGIN, ctx or context())


class DastReviewTest(unittest.TestCase):
    def public_ownership_records(self):
        version = (ROOT / "VERSION").read_text().strip()
        self.assertRegex(version, r"^[0-9]+\.[0-9]+\.[0-9]+$")
        self.assertEqual(json.loads((ROOT / "client/shell/manifest.json").read_text())["version"], version)
        ctx, records = context(), []
        for number, (filename, comment, evidence, digest) in enumerate(PUBLIC_OWNERSHIP_COMMENTS, 1):
            body = (ROOT / "client/shell/client/panels" / filename).read_text()
            self.assertIn(comment, body)
            self.assertTrue(comment.endswith(evidence))
            self.assertEqual(hashlib.sha256(evidence.encode()).hexdigest(), digest)
            self.assertEqual(COMMENT_DIGESTS[filename], frozenset({digest}))
            path = f"/ext/shell/{version}/panels/{filename}"
            ctx.assets[path] = body
            records.append(pair(10027, path, evidence=evidence, body=body, mid=str(number)))
        return records, ctx

    def session(self, **changes):
        return pair(10010, "/api/auth/session", risk=1, param="plinth_csrf", evidence="Set-Cookie: plinth_csrf",
                    response=[*PRIVATE_HEADERS, ("Set-Cookie", LIVE_CSRF)],
                    request=[("Cookie", COOKIE)], **changes)

    def test_csrf_cookie_is_documented_policy_bound_to_real_session_owner(self):
        result = run([self.session()])
        self.assertEqual((result[0]["dispositionId"], result[0]["reviewId"]),
                         ("DOCUMENTED_POLICY", "CSRF_READABLE_COOKIE"))
        self.assertEqual(run([self.session()], ReviewContext("fake-admin", "fake-password", APP, {}, CSP)), [])
        with_prefix = self.session()
        with_prefix[0]["evidence"] = "Set-Cookie: " + LIVE_CSRF
        self.assertEqual(len(run([with_prefix])), 1)

    def test_live_login_cookie_requires_safe_flags_and_positive_lifetime(self):
        good = pair(10010, "/api/auth/login", risk=1, param="plinth_csrf", evidence="Set-Cookie: " + LIVE_CSRF,
                    method="POST", response=[*PRIVATE_HEADERS, ("Set-Cookie", LIVE_CSRF),
                                              ("Set-Cookie", LIVE_SESSION)])
        self.assertEqual(len(run([good])), 1)
        for replaced in ("; Secure", "; HttpOnly", "; SameSite=Strict", "; Path=/", "; Max-Age=86400"):
            bad = copy.deepcopy(good)
            bad[1]["responseHeader"] = bad[1]["responseHeader"].replace(replaced, "")
            self.assertEqual(run([bad]), [])

    def test_csrf_flags_evidence_and_session_response_cookie_are_exact(self):
        for replacement in (LIVE_CSRF.replace("; Secure", ""), LIVE_CSRF.replace("Path=/", "Path=/other"),
                            LIVE_CSRF + "; Domain=plinth.test", LIVE_CSRF + "; HttpOnly"):
            bad = self.session()
            bad[0]["evidence"] = "Set-Cookie: " + replacement
            bad[1]["responseHeader"] = bad[1]["responseHeader"].replace(LIVE_CSRF, replacement)
            self.assertEqual(run([bad]), [])
        bad = self.session()
        bad[1]["responseHeader"] = bad[1]["responseHeader"].replace("\r\n\r\n", "\r\nSet-Cookie: " + LIVE_SESSION + "\r\n\r\n")
        self.assertEqual(run([bad]), [])
        bad[0]["evidence"] = LIVE_SESSION
        self.assertEqual(run([bad]), [])

    def test_duplicate_or_forged_identity_never_matches_database_receipt(self):
        for body in (json.dumps(IDENTITY).replace(USER, "33333333-3333-4333-8333-333333333333"),
                     json.dumps(IDENTITY).replace(SESSION, "33333333-3333-4333-8333-333333333333"),
                     '{"user":{},"user":' + json.dumps(IDENTITY["user"]) + ',"session":' + json.dumps(IDENTITY["session"]) + '}',
                     json.dumps(IDENTITY)[:-1] + ',"extra":NaN}',
                     json.dumps(IDENTITY)[:-1] + ',"extra":Infinity}'):
            self.assertEqual(run([self.session(body=body)]), [])

    def test_expired_cookie_not_applicable_requires_owned_csrf_logout(self):
        deleted = "plinth_csrf=; Path=/; SameSite=Strict; Max-Age=0"
        good = pair(10011, "/api/auth/logout", risk=1, param="plinth_csrf", evidence="Set-Cookie: " + deleted,
                    method="POST", body='{"status":"logged_out"}',
                    response=[*PRIVATE_HEADERS, ("Set-Cookie", deleted)],
                    request=[("Cookie", COOKIE), ("Origin", ORIGIN), ("X-Plinth-CSRF", CSRF)])
        self.assertEqual(run([good])[0]["dispositionId"], "NOT_APPLICABLE")
        for before, after in (("Max-Age=0", "Max-Age=60"), ("plinth_csrf=;", "plinth_csrf=live;"),
                              ("X-Plinth-CSRF: " + CSRF, "X-Plinth-CSRF: wrong"),
                              ("Origin: " + ORIGIN, "Origin: https://other.test"),
                              ("Cookie: " + COOKIE, "Cookie: plinth_session=; plinth_csrf=")):
            bad = copy.deepcopy(good)
            for key in ("requestHeader", "responseHeader"):
                bad[1][key] = bad[1][key].replace(before, after)
            self.assertEqual(run([bad]), [])

    def test_private_cache_prompt_is_informational_only_with_actual_no_store(self):
        good = pair(10015, "/api/auth/session", confidence=1, param="Cache-Control", evidence="no-store",
                    response=PRIVATE_HEADERS, request=[("Cookie", COOKIE)])
        self.assertEqual(run([good])[0]["reviewId"], "PRIVATE_RESPONSE_NO_STORE")
        for before, after in (("no-store", "no-cache"), ("Cookie, Authorization", "Cookie")):
            bad = copy.deepcopy(good)
            bad[1]["responseHeader"] = bad[1]["responseHeader"].replace(before, after)
            self.assertEqual(run([bad]), [])

    def test_public_cache_prompts_match_only_reviewed_document_or_constant_health(self):
        shell = pair(10015, "/app/", confidence=1, param="Cache-Control", evidence="no-cache", body=APP,
                     response=[("Cache-Control", "no-cache")])
        health = pair(10015, "/healthz", confidence=1, param="Cache-Control", body='{"status":"ok"}', mid="2")
        self.assertEqual(len(run([shell, health])), 2)
        shell[1]["responseBody"] += "unreviewed"
        health[1]["responseBody"] = '{"status":"ok","private":"fake-private"}'
        self.assertEqual(run([shell, health]), [])

    def test_own_session_metadata_requires_exact_field_owner_and_no_store(self):
        good = pair(2, "/api/auth/session", risk=1, evidence="192.0.2.7", response=PRIVATE_HEADERS,
                    request=[("Cookie", COOKIE)])
        self.assertEqual(run([good])[0]["reviewId"], "AUTHORIZED_OWN_SESSION_METADATA")
        good[0]["evidence"] = "192.0.2.8"
        self.assertEqual(run([good]), [])

    def test_same_route_label_does_not_accept_other_session_path(self):
        good = self.session()
        for key in ("url",):
            good[0][key] += "/other"
        good[1]["requestHeader"] = good[1]["requestHeader"].replace("/api/auth/session ", "/api/auth/session/other ")
        self.assertEqual(run([good]), [])

    def test_comments_require_individual_allowlist_and_entire_current_asset(self):
        evidence = "// fake reviewed preference comment"
        path = "/ext/shell/0.6.6/prepaint.js"
        source = evidence + "\nexport const example = 1;"
        ctx = context()
        ctx.assets[path] = source
        good = pair(10027, path, evidence=evidence, body=source)
        with patch("dast_review.COMMENT_DIGESTS", {"prepaint.js": {hashlib.sha256(evidence.encode()).hexdigest()}}):
            self.assertEqual(run([good], ctx)[0]["reviewId"], "REVIEWED_PUBLIC_SOURCE_COMMENT")
            bad = copy.deepcopy(good)
            bad[1]["responseBody"] += "\n// fake credential"
            self.assertEqual(run([bad], ctx), [])
            bad = copy.deepcopy(good)
            bad[0]["evidence"] = "export const example = 1;"
            self.assertEqual(run([bad], ctx), [])

    def test_unknown_comment_in_other_exact_asset_rejects_entire_static_group(self):
        known = "// fake reviewed preference comment"
        unknown = "// fake unreviewed query comment"
        first_path = "/ext/shell/0.6.6/prepaint.js"
        second_path = "/ext/shell/0.6.6/sdk.js"
        ctx = context()
        ctx.assets.update({first_path: known, second_path: unknown})
        records = [pair(10027, first_path, evidence=known, body=known),
                   pair(10027, second_path, evidence=unknown, body=unknown, mid="2")]
        allow = {"prepaint.js": {hashlib.sha256(known.encode()).hexdigest()}}
        with patch("dast_review.COMMENT_DIGESTS", allow):
            self.assertEqual(run(records, ctx), [])
        allow["sdk.js"] = {hashlib.sha256(unknown.encode()).hexdigest()}
        with patch("dast_review.COMMENT_DIGESTS", allow):
            self.assertEqual(len(run(records, ctx)), 1)
            changed = copy.deepcopy(records)
            changed[1][0]["evidence"] = "// fake unreviewed query"
            self.assertEqual(run(changed, ctx), [])

    def test_actual_public_ownership_comment_suffixes_match_exact_source_hashes_and_whole_group(self):
        records, ctx = self.public_ownership_records()
        result = run(records, ctx)
        self.assertEqual(len(result), 1)
        self.assertEqual((result[0]["dispositionId"], result[0]["reviewId"]),
                         ("INFORMATIONAL", "REVIEWED_PUBLIC_SOURCE_COMMENT"))
        self.assertEqual(result[0]["risk"], 0)

    def test_public_ownership_comment_changed_truncated_unknown_or_empty_evidence_rejects_group(self):
        records, ctx = self.public_ownership_records()
        for index, record in enumerate(records):
            evidence = record[0]["evidence"]
            for altered in (evidence[:-1], evidence[1:], evidence + " changed", "// unknown comment", ""):
                with self.subTest(member=index, change=altered):
                    bad = copy.deepcopy(records)
                    bad[index][0]["evidence"] = altered
                    self.assertEqual(run(bad, ctx), [])

    def test_public_ownership_comment_changed_full_asset_stale_version_or_wrong_file_rejects_group(self):
        records, ctx = self.public_ownership_records()
        for index in range(len(records)):
            with self.subTest(member=index, change="asset"):
                bad = copy.deepcopy(records)
                bad[index][1]["responseBody"] += "\n// unreviewed addition"
                self.assertEqual(run(bad, ctx), [])
            for change in ("version", "file"):
                with self.subTest(member=index, change=change):
                    bad = copy.deepcopy(records)
                    old = bad[index][0]["url"]
                    path = old.removeprefix(ORIGIN)
                    if change == "version":
                        new_path = re.sub(r"/shell/[^/]+/", "/shell/0.0.0/", path)
                    else:
                        new_path = records[1 - index][0]["url"].removeprefix(ORIGIN)
                        bad[index][1]["responseBody"] = ctx.assets[new_path]
                    bad[index][0]["url"] = ORIGIN + new_path
                    bad[index][1]["requestHeader"] = bad[index][1]["requestHeader"].replace(old, ORIGIN + new_path)
                    self.assertEqual(run(bad, ctx), [])

    def test_public_ownership_comment_http_rule_variant_and_severity_boundaries_remain_exact(self):
        records, ctx = self.public_ownership_records()
        changes = (("method", "POST"), ("pluginId", "10028"), ("alertRef", "10027-1"),
                   ("alertRef", "10027-*"), ("risk", 1), ("confidence", 1), ("confidence", 3))
        for index in range(len(records)):
            for field, value in changes:
                with self.subTest(member=index, field=field, value=value):
                    bad = copy.deepcopy(records)
                    bad[index][0][field] = value
                    if field == "method":
                        bad[index][1]["requestHeader"] = bad[index][1]["requestHeader"].replace("GET ", "POST ", 1)
                    self.assertEqual(run([bad[index]], ctx), [])
                    # Method stays in the original group: one unmatched member
                    # rejects that whole group. Other key changes do not erase
                    # an independently reviewed group; public_report separately
                    # rejects malformed or unresolved observations.
                    expected = [] if field == "method" else run([records[1 - index]], ctx)
                    self.assertEqual(run(bad, ctx), expected)
            for change in ("status", "origin", "host", "route"):
                with self.subTest(member=index, change=change):
                    bad = copy.deepcopy(records)
                    if change == "status":
                        bad[index][1]["responseHeader"] = bad[index][1]["responseHeader"].replace("200 Fixture", "404 Fixture")
                    elif change == "origin":
                        bad[index][0]["url"] = bad[index][0]["url"].replace(ORIGIN, "https://other.test:8443")
                        bad[index][1]["requestHeader"] = bad[index][1]["requestHeader"].replace(ORIGIN, "https://other.test:8443")
                    elif change == "route":
                        old = bad[index][0]["url"]
                        bad[index][0]["url"] = ORIGIN + "/app/"
                        bad[index][1]["requestHeader"] = bad[index][1]["requestHeader"].replace(old, ORIGIN + "/app/")
                    else:
                        bad[index][1]["requestHeader"] = bad[index][1]["requestHeader"].replace("Host: plinth.test:8443", "Host: other.test:8443")
                    self.assertEqual(run([bad[index]], ctx), [])
                    expected = run([records[1 - index]], ctx) if change in ("origin", "route") else []
                    self.assertEqual(run(bad, ctx), expected)

    def test_public_ownership_comment_message_joins_are_unique_exact_and_required_for_every_member(self):
        records, ctx = self.public_ownership_records()
        alerts, messages = [item[0] for item in records], [item[1] for item in records]
        self.assertEqual(reviewed_dispositions(alerts, messages[:1], ORIGIN, ctx), [])
        self.assertEqual(reviewed_dispositions(alerts, messages + [messages[0]], ORIGIN, ctx), [])
        for index in range(len(records)):
            for identifier in ("99", "01", records[1 - index][1]["id"]):
                with self.subTest(member=index, identifier=identifier):
                    bad = copy.deepcopy(records)
                    bad[index][0]["messageId"] = identifier
                    self.assertEqual(run(bad, ctx), [])
        self.assertEqual(run([], ctx), [])

    def test_public_ownership_comment_nonempty_parameter_still_fails_the_public_policy_boundary(self):
        records, ctx = self.public_ownership_records()
        review = run(records, ctx)[0]
        self.assertTrue(_review_class(review["dispositionId"], review["reviewId"],
                                      review["pluginId"], review["risk"], review["confidence"],
                                      review["routeLabel"], "", review["alertRef"]))
        for parameter in ("fake-field", "username", " "):
            self.assertFalse(_review_class(review["dispositionId"], review["reviewId"],
                                           review["pluginId"], review["risk"], review["confidence"],
                                           review["routeLabel"], parameter, review["alertRef"]))

    def test_discovery_markers_require_actual_expected_request_or_response(self):
        app = pair(10109, "/app/", body=APP, evidence='id="root"')
        login = pair(10111, "/api/auth/login", method="POST", confidence=3, param="username",
                     evidence="username", request_body=json.dumps({"username": "fake-admin", "password": "fake-password"}),
                     response=[("Set-Cookie", LIVE_SESSION)], mid="2")
        session = pair(10112, "/api/auth/session", param="plinth_csrf", evidence=LIVE_CSRF,
                       response=[("Set-Cookie", LIVE_CSRF)], request=[("Cookie", COOKIE)], mid="3")
        self.assertEqual(len(run([app, login, session])), 3)
        login[1]["requestBody"] = '{"username":"other","password":"fake-password"}'
        session[0]["evidence"] = "not in cookie"
        app[1]["responseBody"] += "unreviewed"
        self.assertEqual(run([app, login, session]), [])

    def test_known_cookie_marker_requires_exact_name_format_and_unique_target_cookie(self):
        good = self.session()
        self.assertEqual(len(run([good])), 1)
        for evidence in ("Set-Cookie:", "Set-Cookie: ", "set-cookie: plinth_csrf", "Set-Cookie: plinth_csr",
                         "Set-Cookie: plinth_csrf-other", "Set-Cookie: plinth_session",
                         "Set-Cookie: plinth_csrf ", "plinth_csrf", LIVE_CSRF):
            bad = copy.deepcopy(good)
            bad[0]["evidence"] = evidence
            self.assertEqual(run([bad]), [])
        bad = copy.deepcopy(good)
        bad[1]["responseHeader"] = bad[1]["responseHeader"].replace(
            "\r\n\r\n", "\r\nSet-Cookie: " + LIVE_CSRF + "\r\n\r\n")
        self.assertEqual(run([bad]), [])
        bad = copy.deepcopy(good)
        bad[1]["responseHeader"] = bad[1]["responseHeader"].replace("plinth_csrf=", "other_cookie=")
        self.assertEqual(run([bad]), [])

    def test_authentication_discovery_is_only_the_identified_username_parameter(self):
        good = pair(10111, "/api/auth/login", method="POST", confidence=3, param="username",
                    evidence="username", request_body=json.dumps({"username": "fake-admin", "password": "fake-password"}),
                    response=[("Set-Cookie", LIVE_SESSION)])
        self.assertEqual(len(run([good])), 1)
        for parameter in ("password", "username,password", "username, password", "", "other"):
            bad = copy.deepcopy(good)
            bad[0]["param"] = parameter
            self.assertEqual(run([bad]), [])

    def test_styles_policy_keeps_scanner_medium_risk_and_never_qualifies_other_variants(self):
        good = pair(10055, "/app/", ref="10055-6", risk=2, confidence=3, param="Content-Security-Policy",
                    evidence=CSP, body=APP, response=[("Content-Security-Policy", CSP)])
        result = run([good])
        self.assertEqual((result[0]["risk"], result[0]["dispositionId"]), (2, "DOCUMENTED_POLICY"))
        mixed = [good]
        for index, ref in enumerate(("10055-4", "10055-13"), 2):
            member = copy.deepcopy(good)
            member[0].update(alertRef=ref, messageId=str(index))
            member[1]["id"] = str(index)
            mixed.append(member)
        self.assertEqual([review["alertRef"] for review in run(mixed)], ["10055-6"])
        for ref in ("10055-4", "10055-13", "10055"):
            bad = copy.deepcopy(good)
            bad[0]["alertRef"] = ref
            self.assertEqual(run([bad]), [])
        for before, after in (("default-src 'self'; ", ""), ("object-src 'none'", "object-src *")):
            ctx = context()
            altered = CSP.replace(before, after)
            ctx = ReviewContext(ctx.username, ctx.password, APP, {}, altered, ctx.session_owners)
            bad = copy.deepcopy(good)
            bad[0]["evidence"] = altered
            bad[1]["responseHeader"] = bad[1]["responseHeader"].replace(CSP, altered)
            self.assertEqual(run([bad], ctx), [])

    def test_bodyless_websocket_requires_actual_matching_upgrade(self):
        key = base64.b64encode(b"0123456789abcdef").decode()
        accepted = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode(),
                                                 usedforsecurity=False).digest()).decode()
        good = pair(10038, "/ws/events", ref="10038-1", risk=2, confidence=3, status=101, body="",
                    request=[("Origin", ORIGIN), ("Upgrade", "websocket"), ("Connection", "Upgrade"),
                             ("Sec-WebSocket-Version", "13"), ("Sec-WebSocket-Key", key)],
                    response=[("Upgrade", "websocket"), ("Connection", "Upgrade"), ("Sec-WebSocket-Accept", accepted)])
        self.assertEqual(run([good])[0]["reviewId"], "WEBSOCKET_UPGRADE_NO_DOCUMENT")
        for header, before, after in (("responseHeader", accepted, "wrong"), ("requestHeader", "Upgrade\r\n", "close\r\n")):
            bad = copy.deepcopy(good)
            bad[1][header] = bad[1][header].replace(before, after)
            self.assertEqual(run([bad]), [])
        good[1]["responseBody"] = "document"
        self.assertEqual(run([good]), [])

    def test_every_member_must_pass_and_group_reviews_are_never_preemptive(self):
        first = self.session()
        second = self.session(mid="2")
        self.assertEqual(len(run([first, second])), 1)
        second[1]["responseBody"] = '{"user":{"username":"other"},"session":{}}'
        self.assertEqual(run([first, second]), [])
        self.assertEqual(run([]), [])

    def test_missing_duplicate_or_noncanonical_message_id_rejects_proof(self):
        good = self.session()
        self.assertEqual(reviewed_dispositions([good[0]], [], ORIGIN, context()), [])
        self.assertEqual(run([good, good]), [])
        for mid in ("", "01", "0", "-1", "99999999999"):
            bad = self.session(mid=mid)
            self.assertEqual(run([bad]), [])

    def test_cross_origin_mismatched_method_url_and_duplicate_headers_reject_proof(self):
        for before, after in (("Host: plinth.test:8443", "Host: other.test"), ("GET ", "POST "),
                              ("/api/auth/session ", "/api/auth/session?other=1 "),
                              ("Cookie: " + COOKIE, "Cookie: " + COOKIE + "; plinth_session=other")):
            bad = self.session()
            bad[1]["requestHeader"] = bad[1]["requestHeader"].replace(before, after)
            self.assertEqual(run([bad]), [])
        bad = self.session()
        bad[1]["responseHeader"] = bad[1]["responseHeader"].replace("\r\n\r\n", "\r\nCache-Control: no-store\r\n\r\n")
        self.assertEqual(run([bad]), [])

    def test_unknown_low_risk_and_actual_omissions_are_not_accepted(self):
        records = [pair(plugin, "/app/", body=APP, mid=str(index + 1))
                   for index, plugin in enumerate((98765, 10020, 10021, 10035, 10036, 10019))]
        self.assertEqual(run(records), [])


if __name__ == "__main__":
    unittest.main()
