"""Pure controls for the DAST report boundary; no network or scanner execution."""

from __future__ import annotations

import copy
import json
import unittest

from dast_report import (
    REQUIRED_CONTROLS, REQUIRED_ROUTES, REVIEW_IDS, SCANNER_VERSION, classify_route, public_report,
)


ORIGIN = "https://plinth.test:8443"
PRIVATE_MARKER = "fake-private-body-token-never-public"


def inputs():
    return {
        "identity": {
            "sourceRevision": "a" * 40, "sourceTree": "b" * 40,
            "imageDigest": "sha256:" + "c" * 64,
            "scannerDigest": "sha256:" + "d" * 64, "scannerVersion": SCANNER_VERSION,
        },
        "controls": [{"id": code, "passed": True} for code in sorted(REQUIRED_CONTROLS)],
        "scanner": {
            "completed": True, "version": SCANNER_VERSION,
            "passiveRecordsRemaining": 0, "httpMessages": 30,
            "observedRoutes": sorted(REQUIRED_ROUTES), "targetOrigin": ORIGIN,
        },
        "raw_alerts": [], "dispositions": [],
        "cleanup": {
            "verified": True, "namespaceAbsent": True, "clusterAbsent": True,
            "registryAbsent": True, "scannerContainerAbsent": True,
            "temporaryFilesAbsent": True, "remainingContainers": 0,
            "remainingNetworks": 0, "remainingVolumes": 0,
        },
    }


def alert(**changes):
    value = {
        "pluginId": "10038", "alertRef": "10038-1", "risk": "2", "confidence": "3",
        "url": ORIGIN + "/ws/events?query=" + PRIVATE_MARKER,
        "param": "", "evidence": PRIVATE_MARKER,
        "name": PRIVATE_MARKER, "description": PRIVATE_MARKER,
        "solution": PRIVATE_MARKER, "headers": {"Authorization": PRIVATE_MARKER},
        "attack": PRIVATE_MARKER,
    }
    value.update(changes)
    if "pluginId" in changes and "alertRef" not in changes:
        value["alertRef"] = str(changes["pluginId"])
    return value


def review(**changes):
    value = {
        "pluginId": 10038, "alertRef": "10038-1", "risk": 2, "confidence": 3, "routeLabel": "WS",
        "param": "", "dispositionId": "NOT_APPLICABLE",
        "reviewId": "WEBSOCKET_UPGRADE_NO_DOCUMENT",
    }
    value.update(changes)
    if "pluginId" in changes and "alertRef" not in changes:
        value["alertRef"] = str(changes["pluginId"])
    return value


class DastReportTest(unittest.TestCase):
    def assert_incomplete(self, value, reason=None):
        report = public_report(**value)
        self.assertEqual(report["status"], "INCOMPLETE")
        if reason:
            self.assertIn(reason, report["reasons"])
        self.assertNotIn(PRIVATE_MARKER, json.dumps(report, allow_nan=False))
        self.assertNotIn(ORIGIN, json.dumps(report))
        return report

    def test_zero_alert_complete_is_json_safe_and_does_not_mutate_inputs(self):
        value = inputs()
        before = copy.deepcopy(value)
        report = public_report(**value)
        self.assertEqual(report["status"], "COMPLETE")
        self.assertEqual(report["reasons"], [])
        self.assertEqual(report["alertGroups"], [])
        self.assertEqual(json.loads(json.dumps(report, allow_nan=False)), report)
        self.assertEqual(value, before)
        self.assertNotIn("targetOrigin", report["scanner"])

    def test_reviewed_group_aggregates_only_safe_fields(self):
        value = inputs()
        value["raw_alerts"] = [alert(), alert(url=ORIGIN + "/ws/events")]
        value["dispositions"] = [review()]
        report = public_report(**value)
        self.assertEqual(report["status"], "COMPLETE")
        self.assertEqual(report["alertGroups"], [{
            "ruleId": 10038, "risk": 2, "confidence": 3, "count": 2,
            "dispositionId": "NOT_APPLICABLE", "reviewId": "WEBSOCKET_UPGRADE_NO_DOCUMENT",
        }])
        encoded = json.dumps(report, allow_nan=False)
        for forbidden in (PRIVATE_MARKER, ORIGIN, "Authorization", "/app/", "description", "param", "alertRef"):
            self.assertNotIn(forbidden, encoded)

    def test_malformed_identity_does_not_echo_untrusted_fields(self):
        for field, bad in (
            ("sourceRevision", "a" * 39), ("sourceTree", "B" * 40),
            ("imageDigest", PRIVATE_MARKER), ("scannerDigest", True),
            ("scannerVersion", "2.16.0"), ("extra", PRIVATE_MARKER),
        ):
            with self.subTest(field=field):
                value = inputs()
                value["identity"][field] = bad
                self.assertIsNone(self.assert_incomplete(value, "INVALID_IDENTITY")["identity"])
        for bad in (None, [], PRIVATE_MARKER):
            value = inputs()
            value["identity"] = bad
            self.assert_incomplete(value, "INVALID_IDENTITY")

    def test_zero_placeholder_hashes_never_qualify_an_identity(self):
        for field in ("sourceRevision", "sourceTree", "imageDigest", "scannerDigest"):
            value = inputs()
            value["identity"][field] = (
                "0" * 40 if field.startswith("source") else "sha256:" + "0" * 64
            )
            self.assertIsNone(self.assert_incomplete(value, "INVALID_IDENTITY")["identity"])

    def test_public_route_classifier_matches_only_named_same_origin_scope(self):
        self.assertEqual(classify_route(ORIGIN + "/app/", ORIGIN), "APP")
        for path in (
            "/app/../../config.json", "/api/unknown",
            "/ext/shell/0.6.5/../../config.json",
        ):
            with self.subTest(path=path):
                self.assertEqual(classify_route(ORIGIN + path, ORIGIN), "ERROR_PROBE")
                self.assertIsNone(classify_route("https://other.test:8443" + path, ORIGIN))
        for path in (
            "/app/../../secret", "/api/unknown-secret",
            "/ext/evil/1/../../config.json", "/ext/shell/0.6.5/../../secret",
            "/ext/shell/01.6.5/../../config.json", "/ext/shell/0.6.5/../sdk.js",
            "/unknown", "/app/%2e%2e/%2e%2e/config.json",
        ):
            self.assertIsNone(classify_route(ORIGIN + path, ORIGIN))
        self.assertIsNone(classify_route(ORIGIN + "/app/", ORIGIN + "/"))
        self.assertIsNone(classify_route(None, ORIGIN))
        self.assertIsNone(classify_route(ORIGIN + "/app/", None))

    def test_error_fixture_alerts_still_need_exact_explicit_review(self):
        value = inputs()
        value["scanner"]["observedRoutes"].append("ERROR_PROBE")
        value["raw_alerts"] = [alert(url=ORIGIN + "/app/../../config.json")]
        self.assert_incomplete(value, "PRIVATE_TRIAGE_REQUIRED")
        value["dispositions"] = [review(routeLabel="ERROR_PROBE")]
        self.assert_incomplete(value, "PRIVATE_TRIAGE_REQUIRED")

    def test_controls_require_exact_distinct_boolean_named_set(self):
        for change in ("missing", "duplicate", "unknown", "extra", "not_bool", "failed", "not_list"):
            with self.subTest(change=change):
                value = inputs()
                if change == "missing":
                    value["controls"].pop()
                elif change == "duplicate":
                    value["controls"].append(value["controls"][0])
                elif change == "unknown":
                    value["controls"][0]["id"] = PRIVATE_MARKER
                elif change == "extra":
                    value["controls"][0]["rationale"] = PRIVATE_MARKER
                elif change == "not_bool":
                    value["controls"][0]["passed"] = 1
                elif change == "failed":
                    value["controls"][0]["passed"] = False
                else:
                    value["controls"] = None
                self.assert_incomplete(value)

    def test_scanner_schema_and_terminal_gate_fail_closed(self):
        for field, bad, reason in (
            ("completed", False, "SCAN_INCOMPLETE"),
            ("completed", 1, "INVALID_SCANNER"),
            ("version", "2.16.0", "INVALID_SCANNER"),
            ("passiveRecordsRemaining", 1, "SCAN_INCOMPLETE"),
            ("passiveRecordsRemaining", True, "INVALID_SCANNER"),
            ("passiveRecordsRemaining", -1, "INVALID_SCANNER"),
            ("httpMessages", 0, "INVALID_SCANNER"),
            ("httpMessages", 1.0, "INVALID_SCANNER"),
            ("httpMessages", True, "INVALID_SCANNER"),
            ("httpMessages", 1 << 100, "INVALID_SCANNER"),
            ("observedRoutes", [PRIVATE_MARKER], "INVALID_SCANNER"),
            ("observedRoutes", [], "MISSING_ROUTE"),
            ("targetOrigin", ORIGIN + "/", "INVALID_SCANNER"),
            ("targetOrigin", "http://plinth.test:8443", "INVALID_SCANNER"),
            ("targetOrigin", "https://plinth.test:", "INVALID_SCANNER"),
            ("targetOrigin", "https://plinth.test:08443", "INVALID_SCANNER"),
            ("targetOrigin", "https://plinth.test?", "INVALID_SCANNER"),
            ("targetOrigin", "https://plinth.test:443", "INVALID_SCANNER"),
            ("extra", PRIVATE_MARKER, "INVALID_SCANNER"),
        ):
            with self.subTest(field=field, bad=bad):
                value = inputs()
                value["scanner"][field] = bad
                self.assert_incomplete(value, reason)

    def test_every_required_route_is_individually_required(self):
        for route in REQUIRED_ROUTES:
            value = inputs()
            value["scanner"]["observedRoutes"].remove(route)
            self.assert_incomplete(value, "MISSING_ROUTE")
        value = inputs()
        value["scanner"]["observedRoutes"].append("APP")
        self.assert_incomplete(value, "INVALID_SCANNER")

    def test_cleanup_all_named_absence_checks_and_zero_counts_required(self):
        for field, original in inputs()["cleanup"].items():
            with self.subTest(field=field):
                value = inputs()
                value["cleanup"][field] = False if type(original) is bool else 1
                self.assert_incomplete(value, "CLEANUP_UNVERIFIED")
                value = inputs()
                value["cleanup"][field] = 1 if type(original) is bool else True
                self.assert_incomplete(value, "INVALID_CLEANUP")
        for bad in (None, {}, {"extra": PRIVATE_MARKER}):
            value = inputs()
            value["cleanup"] = bad
            self.assert_incomplete(value, "INVALID_CLEANUP")
        value = inputs()
        value["cleanup"]["remainingVolumes"] = 1 << 100
        self.assert_incomplete(value, "INVALID_CLEANUP")

    def test_unknown_rule_no_review_never_auto_ignored_even_risk_zero(self):
        value = inputs()
        value["raw_alerts"] = [alert(pluginId="987654", risk="0", confidence="0")]
        report = self.assert_incomplete(value, "PRIVATE_TRIAGE_REQUIRED")
        self.assertEqual(report["alertGroups"][0]["ruleId"], 987654)
        self.assertEqual(report["alertGroups"][0]["dispositionId"], "PRIVATE_TRIAGE_REQUIRED")

    def test_malformed_alert_fields_are_generic_private_triage(self):
        for field, bad in (
            ("pluginId", "unknown-rule"), ("pluginId", "01"), ("pluginId", True),
            ("pluginId", "9" * 5000), ("pluginId", 0), ("risk", 4),
            ("risk", True), ("risk", 1.0), ("confidence", 5), ("confidence", "02"),
            ("param", None), ("url", None),
        ):
            with self.subTest(field=field):
                value = inputs()
                value["raw_alerts"] = [alert(**{field: bad})]
                self.assert_incomplete(value, "PRIVATE_TRIAGE_REQUIRED")
        for bad in (None, {}, [PRIVATE_MARKER]):
            value = inputs()
            value["raw_alerts"] = bad
            self.assert_incomplete(value, "PRIVATE_TRIAGE_REQUIRED")

    def test_wrong_origin_and_unknown_routes_never_inherit_a_review(self):
        for url in (
            "https://other.test:8443/app/", "http://plinth.test:8443/app/",
            "https://plinth.test:443/app/", "https://user:pass@plinth.test:8443/app/",
            ORIGIN + "/unknown?secret=" + PRIVATE_MARKER,
            ORIGIN + "/app/#" + PRIVATE_MARKER,
            ORIGIN + "/ext/shell/0.6.6/../sdk.js",
            ORIGIN + "/app/\n" + PRIVATE_MARKER,
            ORIGIN + "\\other.test/app/",
        ):
            with self.subTest(url=url):
                value = inputs()
                value["raw_alerts"] = [alert(url=url)]
                value["dispositions"] = [review()]
                self.assert_incomplete(value, "PRIVATE_TRIAGE_REQUIRED")

    def test_exact_review_key_includes_param_route_risk_and_confidence(self):
        for changes in (
            {"pluginId": 10039}, {"param": "other"}, {"routeLabel": "LOGIN"},
            {"risk": 1}, {"confidence": 2},
        ):
            value = inputs()
            value["raw_alerts"] = [alert()]
            value["dispositions"] = [review(**changes)]
            self.assert_incomplete(value, "PRIVATE_TRIAGE_REQUIRED")

    def test_unused_duplicate_unknown_or_free_text_review_is_rejected(self):
        cases = (
            [review()], [review(), review()], [review(reviewId=PRIVATE_MARKER)],
            [review(dispositionId="EXPECTED_POLICY")], [review(rationale=PRIVATE_MARKER)],
            [review(reviewId="PRIVATE_FIX_REQUIRED")], None, [PRIVATE_MARKER],
        )
        for index, reviews in enumerate(cases):
            value = inputs()
            if index != 0:
                value["raw_alerts"] = [alert()]
            value["dispositions"] = reviews
            self.assert_incomplete(value, "PRIVATE_TRIAGE_REQUIRED")

    def test_private_remediation_disposition_always_prevents_completion(self):
        value = inputs()
        value["raw_alerts"] = [alert()]
        value["dispositions"] = [review(
            dispositionId="PRIVATE_REMEDIATION_REQUIRED", reviewId="PRIVATE_FIX_REQUIRED")]
        report = self.assert_incomplete(value, "PRIVATE_TRIAGE_REQUIRED")
        self.assertEqual(report["alertGroups"][0]["reviewId"], "PRIVATE_FIX_REQUIRED")

    def test_exact_zap_enum_names_and_confirmed_confidence_are_normalized(self):
        value = inputs()
        value["raw_alerts"] = [alert(risk="Low", confidence="Confirmed")]
        value["dispositions"] = [review(risk="Low", confidence="4",
                                       dispositionId="PRIVATE_REMEDIATION_REQUIRED", reviewId="PRIVATE_FIX_REQUIRED")]
        report = public_report(**value)
        self.assertEqual(report["status"], "INCOMPLETE")
        self.assertEqual(report["alertGroups"][0]["confidence"], 4)
        value["raw_alerts"][0]["risk"] = "low"
        self.assert_incomplete(value, "PRIVATE_TRIAGE_REQUIRED")

    def test_output_does_not_alias_inputs(self):
        value = inputs()
        report = public_report(**value)
        value["identity"]["sourceRevision"] = PRIVATE_MARKER
        value["scanner"]["observedRoutes"].append(PRIVATE_MARKER)
        value["cleanup"]["verified"] = False
        self.assertEqual(report["status"], "COMPLETE")
        self.assertNotIn(PRIVATE_MARKER, json.dumps(report))

    def test_canonical_plugin_bound_refs_and_variant_boundary_are_literal_exact_keys(self):
        for ref in ("10038", "10038-0", "10038-4", "10038-9999999999"):
            with self.subTest(ref=ref):
                value = inputs()
                value["raw_alerts"] = [alert(alertRef=ref)]
                value["dispositions"] = [review(alertRef=ref, dispositionId="PRIVATE_REMEDIATION_REQUIRED",
                                                reviewId="PRIVATE_FIX_REQUIRED")]
                before = copy.deepcopy(value)
                report = public_report(**value)
                self.assertEqual(report["status"], "INCOMPLETE")
                self.assertEqual(report["alertGroups"][0]["count"], 1)
                self.assertNotIn("alertRef", json.dumps(report))
                self.assertEqual(value, before)
        value = inputs()
        value["raw_alerts"] = [alert(pluginId="10055")]
        value["dispositions"] = [review(pluginId=10055)]
        self.assertEqual(value["raw_alerts"][0]["alertRef"], "10055")
        self.assertEqual(value["dispositions"][0]["alertRef"], "10055")
        self.assertEqual(public_report(**value)["status"], "INCOMPLETE")

    def test_missing_malformed_foreign_or_unbounded_alert_ref_cannot_inherit_review(self):
        invalid = (
            None, True, 10038, 10038.0, [], {}, "", PRIVATE_MARKER,
            "0", "010038", "10055", "10055-4", "10038-", "10038--4",
            "10038-+4", "10038-01", "10038-00", "10038-1-2", "10038-*",
            "10038-10000000000", "10038-" + "9" * 59,
            " 10038", "10038 ", "10038-4\n", "10038-4\x00", "10038-４",
        )
        for ref in invalid:
            with self.subTest(ref=ref):
                value = inputs()
                value["raw_alerts"] = [alert(alertRef=ref)]
                value["dispositions"] = [review()]
                report = self.assert_incomplete(value, "PRIVATE_TRIAGE_REQUIRED")
                self.assertEqual(report["alertGroups"], [])
        value = inputs()
        raw = alert()
        del raw["alertRef"]
        value["raw_alerts"] = [raw]
        value["dispositions"] = [review()]
        self.assertEqual(self.assert_incomplete(value, "PRIVATE_TRIAGE_REQUIRED")["alertGroups"], [])

    def test_review_schema_requires_exact_canonical_plugin_bound_alert_ref(self):
        for ref in (None, True, 10038, 10038.0, [], {}, "", PRIVATE_MARKER,
                    "10055-4", "010038-4", "10038-", "10038--4", "10038-04",
                    "10038-4-0", "10038-*", "10038-10000000000",
                    "10038-" + "9" * 59, "10038-4 ", "10038-4\x00"):
            with self.subTest(ref=ref):
                value = inputs()
                value["raw_alerts"] = [alert(alertRef="10038-4")]
                value["dispositions"] = [review(alertRef=ref)]
                report = self.assert_incomplete(value, "PRIVATE_TRIAGE_REQUIRED")
                self.assertEqual(report["alertGroups"][0]["dispositionId"], "PRIVATE_TRIAGE_REQUIRED")
                self.assertIsNone(report["alertGroups"][0]["reviewId"])
        value = inputs()
        value["raw_alerts"] = [alert()]
        legacy = review()
        del legacy["alertRef"]
        self.assertEqual(len(legacy), 7)
        value["dispositions"] = [legacy]
        self.assert_incomplete(value, "PRIVATE_TRIAGE_REQUIRED")
        value["dispositions"] = [review(alertRef="10038", variant="4")]
        self.assert_incomplete(value, "PRIVATE_TRIAGE_REQUIRED")
        self.assertEqual(len(review()), 8)

    def test_same_base_mixed_variants_do_not_share_one_synthetic_review(self):
        refs = ("10055-4", "10055-6", "10055-13")
        for reviewed in refs:
            with self.subTest(reviewed=reviewed):
                value = inputs()
                value["raw_alerts"] = [alert(pluginId="10055", alertRef=ref) for ref in refs]
                value["dispositions"] = [review(pluginId=10055, alertRef=reviewed)]
                report = self.assert_incomplete(value, "PRIVATE_TRIAGE_REQUIRED")
                self.assertEqual(len(report["alertGroups"]), 3)
                self.assertEqual([group["count"] for group in report["alertGroups"]], [1, 1, 1])
                self.assertEqual(sum(group["dispositionId"] != "PRIVATE_TRIAGE_REQUIRED"
                                     for group in report["alertGroups"]), 0)
                self.assertEqual(sum(group["dispositionId"] == "PRIVATE_TRIAGE_REQUIRED"
                                     for group in report["alertGroups"]), 3)

    def test_bare_plugin_ref_is_not_a_wildcard_for_any_variant(self):
        value = inputs()
        value["raw_alerts"] = [alert(pluginId="10055", alertRef=ref)
                               for ref in ("10055-4", "10055-6", "10055-13")]
        value["dispositions"] = [review(pluginId=10055, alertRef="10055")]
        report = self.assert_incomplete(value, "PRIVATE_TRIAGE_REQUIRED")
        self.assertTrue(all(group["dispositionId"] == "PRIVATE_TRIAGE_REQUIRED"
                            for group in report["alertGroups"]))
        value["raw_alerts"].append(alert(pluginId="10055", alertRef="10055"))
        report = self.assert_incomplete(value, "PRIVATE_TRIAGE_REQUIRED")
        self.assertEqual(len(report["alertGroups"]), 4)
        self.assertEqual(sum(group["dispositionId"] == "FALSE_POSITIVE"
                             for group in report["alertGroups"]), 0)

    def test_duplicate_missing_or_absent_variant_reviews_are_rejected(self):
        first = review(pluginId=10055, alertRef="10055-4")
        second = review(pluginId=10055, alertRef="10055-6")
        for reviews in ([first], [first, first, second],
                        [first, second, review(pluginId=10055, alertRef="10055-13")]):
            value = inputs()
            value["raw_alerts"] = [alert(pluginId="10055", alertRef=ref)
                                   for ref in ("10055-4", "10055-6")]
            value["dispositions"] = copy.deepcopy(reviews)
            self.assert_incomplete(value, "PRIVATE_TRIAGE_REQUIRED")

    def test_identical_variants_aggregate_but_two_exact_synthetic_reviews_remain_separate(self):
        value = inputs()
        value["raw_alerts"] = [
            alert(pluginId="10055", alertRef="10055-4", url=ORIGIN + "/app/", param="Content-Security-Policy"),
            alert(pluginId="10055", alertRef="10055-4", url=ORIGIN + "/app/?query=" + PRIVATE_MARKER,
                  param="Content-Security-Policy"),
            alert(pluginId="10055", alertRef="10055-6", url=ORIGIN + "/app/", param="Content-Security-Policy"),
        ]
        value["dispositions"] = [
            review(pluginId=10055, alertRef="10055-4", routeLabel="APP", param="Content-Security-Policy",
                   dispositionId="PRIVATE_REMEDIATION_REQUIRED",
                   reviewId="PRIVATE_FIX_REQUIRED"),
            review(pluginId=10055, alertRef="10055-6", routeLabel="APP", param="Content-Security-Policy",
                   dispositionId="DOCUMENTED_POLICY", reviewId="CSP_STYLES_ONLY"),
        ]
        before = copy.deepcopy(value)
        report = public_report(**value)
        self.assertEqual(report["status"], "INCOMPLETE")
        self.assertEqual(len(report["alertGroups"]), 2)
        self.assertEqual({group["reviewId"]: group["count"] for group in report["alertGroups"]},
                         {"PRIVATE_FIX_REQUIRED": 2, "CSP_STYLES_ONLY": 1})
        self.assertTrue(all(group["ruleId"] == 10055 for group in report["alertGroups"]))
        self.assertEqual(value, before)
        encoded = json.dumps(report, allow_nan=False)
        for forbidden in (PRIVATE_MARKER, ORIGIN, "alertRef", "10055-4", "10055-6", "param",
                          "Authorization", "/app/", "description"):
            self.assertNotIn(forbidden, encoded)

    def test_variant_and_other_key_fields_cannot_recombine_two_synthetic_reviews(self):
        value = inputs()
        value["raw_alerts"] = [
            alert(pluginId="10055", alertRef="10055-4"),
            alert(pluginId="10055", alertRef="10055-6", risk=2, confidence=3,
                  url=ORIGIN + "/api/auth/login", param="other-synthetic-param"),
        ]
        value["dispositions"] = [
            review(pluginId=10055, alertRef="10055-6"),
            review(pluginId=10055, alertRef="10055-4", risk=2, confidence=3,
                   routeLabel="LOGIN", param="other-synthetic-param"),
        ]
        report = self.assert_incomplete(value, "PRIVATE_TRIAGE_REQUIRED")
        self.assertEqual(len(report["alertGroups"]), 2)
        self.assertTrue(all(group["dispositionId"] == "PRIVATE_TRIAGE_REQUIRED"
                            and group["reviewId"] is None for group in report["alertGroups"]))

    def test_variant_matching_does_not_expand_public_schema_or_finite_review_ids(self):
        value = inputs()
        value["raw_alerts"] = [alert()]
        value["dispositions"] = [review()]
        report = public_report(**value)
        self.assertEqual(report["status"], "COMPLETE")
        self.assertEqual(set(report), {"schemaVersion", "status", "reasons", "identity",
                                      "controls", "scanner", "alertGroups", "cleanup"})
        self.assertEqual(set(report["alertGroups"][0]), {
            "ruleId", "risk", "confidence", "count", "dispositionId", "reviewId",
        })
        self.assertEqual(REVIEW_IDS, frozenset({
            "CSRF_READABLE_COOKIE", "CSRF_HEADER_VERIFIED", "CSP_FRAME_ANCESTORS_ENFORCED",
            "CSP_STYLES_ONLY", "IMMUTABLE_STATIC_CACHE", "PRIVATE_FIX_REQUIRED",
            "EXPIRED_COOKIE_DELETION", "PUBLIC_SHELL_REVALIDATION", "PUBLIC_CONSTANT_HEALTH",
            "PRIVATE_RESPONSE_NO_STORE", "AUTHORIZED_OWN_SESSION_METADATA",
            "REVIEWED_PUBLIC_SOURCE_COMMENT", "SCANNER_APPLICATION_DISCOVERY",
            "SCANNER_AUTHENTICATION_DISCOVERY", "SCANNER_SESSION_DISCOVERY",
            "WEBSOCKET_UPGRADE_NO_DOCUMENT",
        }))

    def test_documented_style_policy_cannot_qualify_other_variants_or_false_positive_claims(self):
        for ref, kind in (("10055-4", "DOCUMENTED_POLICY"), ("10055-13", "DOCUMENTED_POLICY"),
                          ("10055-6", "FALSE_POSITIVE")):
            value = inputs()
            value["raw_alerts"] = [alert(pluginId="10055", alertRef=ref, url=ORIGIN + "/app/",
                                         param="Content-Security-Policy")]
            value["dispositions"] = [review(pluginId=10055, alertRef=ref, routeLabel="APP",
                                              param="Content-Security-Policy", dispositionId=kind,
                                              reviewId="CSP_STYLES_ONLY")]
            self.assert_incomplete(value, "PRIVATE_TRIAGE_REQUIRED")

    def test_information_class_does_not_ignore_unknown_rules_or_lower_scanner_risk(self):
        value = inputs()
        value["raw_alerts"] = [alert(pluginId="10015", risk=0, confidence=1, url=ORIGIN + "/healthz",
                                     param="Cache-Control")]
        value["dispositions"] = [review(pluginId=10015, risk=0, confidence=1, routeLabel="HEALTH",
                                          param="Cache-Control", dispositionId="INFORMATIONAL",
                                          reviewId="PUBLIC_CONSTANT_HEALTH")]
        self.assertEqual(public_report(**value)["status"], "COMPLETE")
        original = copy.deepcopy(value)
        for changes in ({"pluginId": 98765, "alertRef": "98765"}, {"risk": 1}, {"confidence": 2}):
            value = copy.deepcopy(original)
            for record in (value["raw_alerts"][0], value["dispositions"][0]):
                record.update(changes)
            self.assert_incomplete(value, "PRIVATE_TRIAGE_REQUIRED")


if __name__ == "__main__":
    unittest.main()
