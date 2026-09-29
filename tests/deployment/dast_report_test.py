"""Pure controls for the DAST report boundary; no network or scanner execution."""

from __future__ import annotations

import copy
import json
import unittest

from dast_report import (
    REQUIRED_CONTROLS, REQUIRED_ROUTES, SCANNER_VERSION, classify_route, public_report,
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
        "pluginId": "10038", "risk": "1", "confidence": "2",
        "url": ORIGIN + "/app/?query=" + PRIVATE_MARKER,
        "param": PRIVATE_MARKER, "evidence": PRIVATE_MARKER,
        "name": PRIVATE_MARKER, "description": PRIVATE_MARKER,
        "solution": PRIVATE_MARKER, "headers": {"Authorization": PRIVATE_MARKER},
        "attack": PRIVATE_MARKER,
    }
    value.update(changes)
    return value


def review(**changes):
    value = {
        "pluginId": 10038, "risk": 1, "confidence": 2, "routeLabel": "APP",
        "param": PRIVATE_MARKER, "dispositionId": "FALSE_POSITIVE",
        "reviewId": "CSP_FRAME_ANCESTORS_ENFORCED",
    }
    value.update(changes)
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
        value["raw_alerts"] = [alert(), alert(url=ORIGIN + "/app/")]
        value["dispositions"] = [review()]
        report = public_report(**value)
        self.assertEqual(report["status"], "COMPLETE")
        self.assertEqual(report["alertGroups"], [{
            "ruleId": 10038, "risk": 1, "confidence": 2, "count": 2,
            "dispositionId": "FALSE_POSITIVE", "reviewId": "CSP_FRAME_ANCESTORS_ENFORCED",
        }])
        encoded = json.dumps(report, allow_nan=False)
        for forbidden in (PRIVATE_MARKER, ORIGIN, "Authorization", "/app/", "description", "param"):
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
        self.assertEqual(public_report(**value)["status"], "COMPLETE")

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
            {"risk": 2}, {"confidence": 3},
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
        value["dispositions"] = [review(risk="Low", confidence="4")]
        report = public_report(**value)
        self.assertEqual(report["status"], "COMPLETE")
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


if __name__ == "__main__":
    unittest.main()
