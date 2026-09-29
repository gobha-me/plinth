"""Mocked DAST driver gates: never start Docker, a scanner, or a browser."""

from __future__ import annotations

import copy
import json
from pathlib import Path
import re
from types import SimpleNamespace
import unittest
from unittest import mock

import ingress_dast_test as driver
from dast_report import REQUIRED_ROUTES


ORIGIN = "https://plinth.test:8443"
HOST = "plinth.test:8443"
OWNER = "fake-exact-task-owner"
ACTIVE_SEED = "/app/?issue40_probe=read_only"
SOURCE_STATUS_COMMAND = ["git", "status", "--porcelain=v1", "--untracked-files=all",
                         "--ignore-submodules=none"]
SOURCE_CHART_COMMAND = ["git", "ls-files", "--others", "--", "deploy/helm/plinth"]


def result(stdout="", *, returncode=0, stderr=""):
    return SimpleNamespace(stdout=stdout, returncode=returncode, stderr=stderr)


def scanner():
    value = object.__new__(driver.Scanner)
    value.harness = SimpleNamespace(run=mock.Mock(), origin=ORIGIN,
                                    request=mock.Mock(return_value=(200, {}, b"")))
    value.name = "plinth-issue40-zap-fake-owned"
    value.owner = OWNER
    value.port = 12345
    value.started = False
    value.creation_attempted = False
    value.scans = []
    value.completed = False
    value.passive_remaining = None
    value.scan_evidence = []
    return value


def harness():
    value = object.__new__(driver.DastHarness)
    value.cluster = "plinth-issue36-fake-owned"
    value.registry = "plinth-issue36-reg-fake-owned"
    value.scanner = scanner()
    value.host = "plinth.test"
    value.https_port = 8443
    value.certificate = Path("/fake-fixture/tls.crt")
    value.initial_volumes = {"preexisting-shared-volume"}
    value.owned_volumes = set()
    value.run = mock.Mock()
    value._exit_watchers = []
    return value


def message(target="/app/", *, host=HOST, method="GET", protocol="HTTP/1.1"):
    return {"requestHeader": f"{method} {target} {protocol}\r\nHost: {host}\r\n\r\n"}


def browser_receipt():
    labels = ("ROOT", "APP", "SESSION", "LOGIN", "LOGOUT", "REGISTRATION", "CAP", "WS")
    return {
        "schema": "plinth.ingress-browser.v1", "completed": True, "stage": "COMPLETE",
        "controls": {"BROWSER_AUTH": True, "WS_UPGRADE": True},
        "routes": {label: {"requests": 1, "statuses": [200]} for label in labels},
        "counts": {"pageErrors": 0, "cspViolations": 0, "failedRequests": 0,
                   "externalRequestsBlocked": 0, "externalResponses": 0, "invalidWsFrames": 0},
        "proof": {
            "websocket": {"connections": 1, "connected": True, "applicationGrant": True},
            "logout": {"sessionStatus": 401, "originalSocketClosed": True},
        },
        "cleanup": {"contextClosed": True, "browserClosed": True}, "elapsedMs": 100,
    }


def scan_progress():
    return (
        {"scans": [{"id": "0", "progress": "100", "state": "FINISHED", "reqCount": "2"}]},
        {"scanProgress": [ORIGIN, {"HostProcess": [
            {"Plugin": ["fake rule", "40012", "release", "Complete", "1", "1", "0"]},
            {"Plugin": ["fake rule", "40018", "release", "Complete", "1", "1", "0"]},
        ]}]},
        {"messagesIds": ["1", "2"]},
    )


def active_messages(path=ACTIVE_SEED):
    endpoint = driver.urllib.parse.urlsplit(path).path
    return [{
        "id": str(index),
        "requestHeader": (f"GET {ORIGIN}{endpoint}?issue40_probe=fake_mutation_{rule} HTTP/1.1\r\nHost: {HOST}\r\n"
                          f"x-zap-scan-id: {rule}\r\n\r\n"),
        "requestBody": "", "responseHeader": "HTTP/1.1 200 OK\r\n\r\n",
        "responseBody": "fake owned HTTP response",
    } for index, rule in enumerate(driver.ACTIVE_RULES, 1)]


def message_query(value, query):
    """Alter only a fresh test message's raw query, retaining all other proof."""
    first, rest = value["requestHeader"].split("\r\n", 1)
    method, target, protocol = first.split(" ")
    parsed = driver.urllib.parse.urlsplit(target)
    target = driver.urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, parsed.path, query, ""))
    value["requestHeader"] = f"{method} {target} {protocol}\r\n{rest}"


class ActiveMessageTest(unittest.TestCase):
    def test_exact_response_inventory_and_both_rule_attacks_are_immutable(self):
        messages = active_messages()
        before = copy.deepcopy(messages)
        scanner().validate_active_messages(ACTIVE_SEED, ["1", "2"], messages)
        self.assertEqual(messages, before)

    def test_missing_duplicate_or_foreign_message_ids_do_not_count(self):
        for invalid in ([], active_messages()[:1], active_messages() + active_messages()[:1]):
            with self.assertRaises(AssertionError):
                scanner().validate_active_messages(ACTIVE_SEED, ["1", "2"], invalid)
        messages = active_messages()
        messages[1]["id"] = "foreign-id"
        with self.assertRaises(AssertionError):
            scanner().validate_active_messages(ACTIVE_SEED, ["1", "2"], messages)

    def test_foreign_host_path_or_non_get_attack_never_qualifies(self):
        for old, new in (
            ("Host: " + HOST, "Host: other.test:8443"),
            (ORIGIN + "/app/", "https://other.test:8443/app/"),
            (ORIGIN + "/app/", ORIGIN + "/api/auth/session"),
            ("GET ", "POST "),
        ):
            messages = active_messages()
            messages[0]["requestHeader"] = messages[0]["requestHeader"].replace(old, new)
            with self.assertRaises(AssertionError):
                scanner().validate_active_messages(ACTIVE_SEED, ["1", "2"], messages)

    def test_request_body_or_absent_invalid_http_response_never_qualifies(self):
        for field, invalid in (("requestBody", "fake attack body"),
                               ("responseHeader", ""), ("responseHeader", None),
                               ("responseHeader", "not an HTTP response"),
                               ("responseHeader", "HTTP/1.1 000 missing\r\n")):
            messages = active_messages()
            messages[0][field] = invalid
            with self.assertRaises(AssertionError):
                scanner().validate_active_messages(ACTIVE_SEED, ["1", "2"], messages)
        messages = active_messages()
        del messages[0]["responseHeader"]
        with self.assertRaises(AssertionError):
            scanner().validate_active_messages(ACTIVE_SEED, ["1", "2"], messages)

    def test_missing_or_unknown_rule_header_never_substitutes_for_selected_attacks(self):
        for change in ("missing", "duplicate-rule", "unknown-rule"):
            messages = active_messages()
            if change == "missing":
                messages[1]["requestHeader"] = messages[1]["requestHeader"].replace(
                    "x-zap-scan-id: 40018\r\n", "")
            else:
                replacement = "40012" if change == "duplicate-rule" else "99999"
                messages[1]["requestHeader"] = messages[1]["requestHeader"].replace("40018", replacement)
            with self.assertRaises(AssertionError):
                scanner().validate_active_messages(ACTIVE_SEED, ["1", "2"], messages)

    def test_missing_or_nonstring_empty_body_is_not_get_body_proof(self):
        for invalid in (None, False, 0, [], {}):
            messages = active_messages()
            messages[0]["requestBody"] = invalid
            with self.assertRaises(AssertionError):
                scanner().validate_active_messages(ACTIVE_SEED, ["1", "2"], messages)
        messages = active_messages()
        del messages[0]["requestBody"]
        with self.assertRaises(AssertionError):
            scanner().validate_active_messages(ACTIVE_SEED, ["1", "2"], messages)

    def test_multiple_rule_markers_in_one_message_are_ambiguous(self):
        for additional in ("40012", "40018"):
            messages = active_messages()
            messages[0]["requestHeader"] = messages[0]["requestHeader"].replace(
                "\r\n\r\n", f"\r\nx-zap-scan-id: {additional}\r\n\r\n")
            with self.assertRaises(AssertionError):
                scanner().validate_active_messages(ACTIVE_SEED, ["1", "2"], messages)

    def test_unmarked_baselines_are_allowed_but_never_supply_rule_credit(self):
        messages = active_messages()
        baseline = copy.deepcopy(messages[0])
        baseline["id"] = "3"
        baseline["requestHeader"] = baseline["requestHeader"].replace("x-zap-scan-id: 40012\r\n", "")
        message_query(baseline, "issue40_probe=read_only")
        messages.append(baseline)
        scanner().validate_active_messages(ACTIVE_SEED, ["1", "2", "3"], messages)
        messages[1]["requestHeader"] = messages[1]["requestHeader"].replace("x-zap-scan-id: 40018\r\n", "")
        with self.assertRaises(AssertionError):
            scanner().validate_active_messages(ACTIVE_SEED, ["1", "2", "3"], messages)

    def test_unchanged_or_percent_equivalent_values_never_receive_mutation_credit(self):
        for query in ("issue40_probe=read_only", "issue40_probe=r%65ad_only"):
            messages = active_messages()
            for item in messages:
                message_query(item, query)
            with self.assertRaises(AssertionError):
                scanner().validate_active_messages(ACTIVE_SEED, ["1", "2"], messages)

    def test_every_request_keeps_exactly_one_decoded_reviewed_query_key(self):
        for query in ("", "other=fake", "issue40_probe=fake&other=fake",
                      "issue40_probe=fake&issue40_probe=other",
                      "issue40_probe=fake&issue40_%70robe=other"):
            with self.subTest(query=query):
                messages = active_messages()
                message_query(messages[0], query)
                with self.assertRaises(AssertionError):
                    scanner().validate_active_messages(ACTIVE_SEED, ["1", "2"], messages)

    def test_changed_value_for_only_one_selected_rule_cannot_complete(self):
        for index in (0, 1):
            messages = active_messages()
            message_query(messages[index], "issue40_probe=read_only")
            with self.assertRaises(AssertionError):
                scanner().validate_active_messages(ACTIVE_SEED, ["1", "2"], messages)

    def test_tagged_baseline_plus_two_actual_changed_values_supplies_both_rule_proofs(self):
        messages = active_messages()
        baseline = copy.deepcopy(messages[0])
        baseline["id"] = "3"
        message_query(baseline, "issue40_probe=r%65ad_only")
        messages.append(baseline)
        before = copy.deepcopy(messages)
        scanner().validate_active_messages(ACTIVE_SEED, ["1", "2", "3"], messages)
        self.assertEqual(messages, before)

    def test_missing_extra_or_duplicate_seed_keys_are_not_reviewed_inputs(self):
        for seed in ("/app/", "/app/?other=read_only",
                     ACTIVE_SEED + "&other=value", ACTIVE_SEED + "&issue40_probe=value"):
            with self.assertRaises(AssertionError):
                scanner().validate_active_messages(seed, ["1", "2"], active_messages())


class ActiveScanApiTest(unittest.TestCase):
    def scripted(self, *, wrong_option=None, context_id="7"):
        value = scanner()
        current_path = None
        current_scan = None
        calls = []
        options = {"PersistTemporaryMessages": "true", "InjectPluginIdInHeader": "true",
                   "TargetParamsInjectable": "1", "AddQueryParam": "false",
                   "ScanHeadersAllRequests": "false"}

        def api(component, kind, action, **params):
            nonlocal current_path, current_scan
            calls.append((component, kind, action, copy.deepcopy(params)))
            if kind == "action":
                if component == "context" and action == "newContext":
                    return {"contextId": context_id} if context_id is not None else {}
                if action == "scan":
                    self.assertEqual(component, "ascan")
                    current_path = params["url"][len(ORIGIN):]
                    self.assertEqual(value.harness.request.call_args, mock.call(current_path))
                    current_scan = str(len(value.scans))
                    return {"scan": current_scan}
                return {"Result": "OK"}
            if action.startswith("option"):
                option = action[len("option"):]
                self.assertIn(option, options)
                return {option: "unexpected" if option == wrong_option else options[option]}
            if action == "status":
                return {"status": "100"}
            if action in ("scans", "scanProgress", "messagesIds"):
                scans, progress, ids = scan_progress()
                scans["scans"][0]["id"] = current_scan
                return {"scans": scans, "scanProgress": progress, "messagesIds": ids}[action]
            if action == "messagesById":
                self.assertEqual(params, {"ids": "1,2"})
                return {"messagesById": active_messages(current_path)}
            if action == "recordsToScan":
                return {"recordsToScan": "0"}
            self.fail("unexpected scanner API call in immutable fixture")

        value.api = mock.Mock(side_effect=api)
        return value, calls

    def test_policy_options_scope_context_and_actual_messages_match_pinned_api_shape(self):
        value, calls = self.scripted()
        value.active_scan()
        self.assertTrue(value.completed)
        self.assertEqual(value.harness.request.call_args_list,
                         [mock.call(path) for path in driver.ACTIVE_PATHS])
        self.assertEqual(len(value.scan_evidence), len(driver.ACTIVE_PATHS))
        self.assertIn(("ascan", "action", "addScanPolicy", {
            "scanPolicyName": "issue40-readonly", "attackStrength": "MEDIUM",
            "alertThreshold": "MEDIUM",
        }), calls)
        self.assertIn(("ascan", "action", "enableScanners", {
            "ids": "40012,40018", "scanPolicyName": "issue40-readonly",
        }), calls)
        self.assertIn(("ascan", "action", "disableAllScanners", {
            "scanPolicyName": "issue40-readonly",
        }), calls)
        for action, params in (
            ("setOptionTargetParamsInjectable", {"Integer": "1"}),
            ("setOptionAddQueryParam", {"Boolean": "false"}),
            ("setOptionScanHeadersAllRequests", {"Boolean": "false"}),
            ("setOptionPersistTemporaryMessages", {"Boolean": "true"}),
            ("setOptionInjectPluginIdInHeader", {"Boolean": "true"}),
            ("setOptionThreadPerHost", {"Integer": "1"}),
            ("setOptionDelayInMs", {"Integer": "10"}),
        ):
            self.assertIn(("ascan", "action", action, params), calls)
        self.assertIn(("core", "action", "setMode", {"mode": "protect"}), calls)
        self.assertIn(("core", "action", "setMode", {"mode": "safe"}), calls)
        self.assertIn(("context", "action", "setContextInScope", {
            "contextName": "issue40-get-probes", "booleanInScope": "true",
        }), calls)
        includes = [params["regex"] for component, kind, action, params in calls
                    if (component, kind, action) == ("context", "action", "includeInContext")]
        self.assertEqual(len(includes), 1)
        self.assertIsNotNone(re.fullmatch(includes[0], ORIGIN + driver.ACTIVE_PATHS[0]))
        self.assertIsNone(re.fullmatch(includes[0], "https://other.test:8443/app/"))
        self.assertIsNone(re.fullmatch(includes[0], ORIGIN + "/api/auth/logout"))
        requests = [params for component, kind, action, params in calls
                    if (component, kind, action) == ("ascan", "action", "scan")]
        self.assertEqual([params["url"] for params in requests],
                         [ORIGIN + path for path in driver.ACTIVE_PATHS])
        for params in requests:
            self.assertEqual(params, {
                "url": params["url"], "recurse": "false", "inScopeOnly": "true",
                "scanPolicyName": "issue40-readonly", "method": "GET", "contextId": "7",
            })

    def test_option_readback_mismatch_never_sets_completed(self):
        for option in ("PersistTemporaryMessages", "InjectPluginIdInHeader",
                       "TargetParamsInjectable", "AddQueryParam", "ScanHeadersAllRequests"):
            value, calls = self.scripted(wrong_option=option)
            with self.assertRaises(AssertionError):
                value.active_scan()
            self.assertFalse(value.completed)
            self.assertFalse(any(action == "scan" for _, _, action, _ in calls))

    def test_missing_context_never_sets_completed_or_dispatches_an_attack(self):
        value, calls = self.scripted(context_id=None)
        with self.assertRaises((AssertionError, KeyError)):
            value.active_scan()
        self.assertFalse(value.completed)
        self.assertFalse(any(action == "scan" for _, _, action, _ in calls))

    def test_failed_get_seed_never_dispatches_an_attack_or_sets_completed(self):
        for status in (401, 403, 404, 429, 500):
            value, calls = self.scripted()
            value.harness.request.return_value = (status, {}, b"")
            with self.assertRaises(AssertionError):
                value.active_scan()
            self.assertFalse(value.completed)
            self.assertFalse(any(action == "scan" for _, _, action, _ in calls))

    def assert_partial_private_capture(self, value, evidence):
        owner = harness()
        owner.scanner = value
        owner.root = mock.MagicMock(spec=Path)
        owner.root.__truediv__.return_value.is_file.return_value = False
        owner.private_details = {}
        owner.raw_alerts = []
        owner.raw_messages = []
        value.started = True
        value.results = mock.Mock(return_value=([
            {"fakeRaw": "fake partial alert retained privately"},
        ], [message()]))
        owner.retain_private_failure_evidence()
        self.assertIs(owner.private_details["activeScans"], value.scan_evidence)
        self.assertIs(owner.private_details["activeScans"][0], evidence)
        self.assertEqual(owner.raw_alerts, [{"fakeRaw": "fake partial alert retained privately"}])
        self.assertEqual(owner.raw_messages, [message()])
        value.results.assert_called_once_with()
        self.assertFalse(value.completed)

    def test_rejected_progress_keeps_original_private_partial_record(self):
        for failure in ("zero_requests", "skipped_rule"):
            with self.subTest(failure=failure):
                value, _ = self.scripted()
                original_api = value.api.side_effect
                returned = {}

                def api(component, kind, action, **params):
                    response = original_api(component, kind, action, **params)
                    if action == "scans" and failure == "zero_requests":
                        response["scans"][0]["reqCount"] = "0"
                    if action == "scanProgress" and failure == "skipped_rule":
                        response["scanProgress"][1]["HostProcess"][1]["Plugin"][3] = "Skipped"
                    returned[action] = response
                    return response

                value.api.side_effect = api
                with self.assertRaises(AssertionError):
                    value.active_scan()
                self.assertEqual(len(value.scan_evidence), 1)
                evidence = value.scan_evidence[0]
                self.assertEqual(evidence["stage"], "PROGRESS_RECEIVED")
                self.assertEqual(evidence["scanId"], "0")
                self.assertEqual(evidence["seed"], driver.ACTIVE_PATHS[0])
                for field, action in (("status", "status"), ("scans", "scans"),
                                      ("progress", "scanProgress"), ("messagesIds", "messagesIds")):
                    self.assertIs(evidence[field], returned[action])
                self.assertNotIn("messages", evidence)
                self.assert_partial_private_capture(value, evidence)

    def test_bad_or_missing_messages_survive_the_rejected_message_gate(self):
        for failure in ("bad_response", "missing_messages"):
            with self.subTest(failure=failure):
                value, _ = self.scripted()
                original_api = value.api.side_effect
                returned = []

                def api(component, kind, action, **params):
                    response = original_api(component, kind, action, **params)
                    if action == "messagesById":
                        if failure == "bad_response":
                            response["messagesById"][0]["responseHeader"] = ""
                        else:
                            response["messagesById"] = []
                        returned.extend(response["messagesById"])
                    return response

                value.api.side_effect = api
                with self.assertRaises(AssertionError):
                    value.active_scan()
                evidence = value.scan_evidence[0]
                self.assertEqual(evidence["stage"], "MESSAGES_RECEIVED")
                self.assertEqual(evidence["messages"], returned)
                self.assertEqual(evidence["messagesIds"], {"messagesIds": ["1", "2"]})
                self.assert_partial_private_capture(value, evidence)

    def test_api_error_after_allocation_keeps_every_preceding_response(self):
        for failing_action, stage in (("scanProgress", "ALLOCATED"),
                                      ("messagesById", "PROGRESS_RECEIVED")):
            with self.subTest(action=failing_action):
                value, _ = self.scripted()
                original_api = value.api.side_effect

                def api(component, kind, action, **params):
                    if action == failing_action:
                        raise OSError("fake private scanner API failure")
                    return original_api(component, kind, action, **params)

                value.api.side_effect = api
                with self.assertRaises(OSError):
                    value.active_scan()
                evidence = value.scan_evidence[0]
                self.assertEqual(evidence["stage"], stage)
                self.assertEqual(evidence["status"], {"status": "100"})
                self.assertEqual(evidence["scans"]["scans"][0]["state"], "FINISHED")
                if failing_action == "scanProgress":
                    self.assertNotIn("progress", evidence)
                else:
                    self.assertEqual(evidence["messages"], [])
                    self.assertEqual(evidence["messagesIds"], {"messagesIds": ["1", "2"]})
                self.assert_partial_private_capture(value, evidence)

    def test_later_batch_api_failure_retains_the_already_received_messages(self):
        value, _ = self.scripted()
        original_api = value.api.side_effect
        ids = [str(index) for index in range(1, 102)]
        received = []

        def api(component, kind, action, **params):
            if action == "messagesIds":
                return {"messagesIds": ids[:]}
            if action == "messagesById":
                batch = params["ids"].split(",")
                if batch == ["101"]:
                    raise OSError("fake second batch unavailable")
                self.assertEqual(batch, ids[:100])
                for identity in batch:
                    item = copy.deepcopy(active_messages(driver.ACTIVE_PATHS[0])[0])
                    item["id"] = identity
                    received.append(item)
                return {"messagesById": received[:]}
            return original_api(component, kind, action, **params)

        value.api.side_effect = api
        with self.assertRaises(OSError):
            value.active_scan()
        evidence = value.scan_evidence[0]
        self.assertEqual(evidence["stage"], "PROGRESS_RECEIVED")
        self.assertEqual(len(evidence["messages"]), 100)
        self.assertEqual(evidence["messages"], received)
        self.assertEqual(evidence["messagesIds"], {"messagesIds": ids})
        self.assert_partial_private_capture(value, evidence)


class ActiveScanProofTest(unittest.TestCase):
    def test_exact_finished_positive_per_rule_proof_is_immutable(self):
        value = scan_progress()
        before = copy.deepcopy(value)
        scanner().validate_scan_progress("0", *value)
        self.assertEqual(value, before)

    def test_stopped_or_zero_attack_scan_is_not_terminal_success(self):
        for field, invalid in (("state", "STOPPED"), ("state", "RUNNING"),
                               ("progress", "99"), ("reqCount", "0"),
                               ("reqCount", 2), ("reqCount", True), ("reqCount", "-1")):
            with self.subTest(field=field, invalid=invalid):
                scans, progress, ids = scan_progress()
                scans["scans"][0][field] = invalid
                with self.assertRaises(AssertionError):
                    scanner().validate_scan_progress("0", scans, progress, ids)

    def test_missing_or_ambiguous_exact_scan_record_is_rejected(self):
        for records in ([], [{"id": "other"}],
                        scan_progress()[0]["scans"] * 2):
            _, progress, ids = scan_progress()
            with self.assertRaises(AssertionError):
                scanner().validate_scan_progress("0", {"scans": records}, progress, ids)

    def test_each_enabled_rule_must_complete_positive_real_requests(self):
        for index in (0, 1):
            for field, invalid in ((3, "Running"), (3, "Skipped"), (5, "0"),
                                   (5, True), (5, "-1"), (5, "01")):
                with self.subTest(rule=index, field=field, invalid=invalid):
                    scans, progress, ids = scan_progress()
                    progress["scanProgress"][1]["HostProcess"][index]["Plugin"][field] = invalid
                    with self.assertRaises(AssertionError):
                        scanner().validate_scan_progress("0", scans, progress, ids)
            scans, progress, ids = scan_progress()
            progress["scanProgress"][1]["HostProcess"].pop(index)
            with self.assertRaises(AssertionError):
                scanner().validate_scan_progress("0", scans, progress, ids)

    def test_wrong_host_or_duplicate_malformed_rule_proof_is_rejected(self):
        scans, progress, ids = scan_progress()
        progress["scanProgress"][0] = "https://other.test:8443"
        with self.assertRaises(AssertionError):
            scanner().validate_scan_progress("0", scans, progress, ids)
        for kind in ("duplicate", "short", "missing"):
            scans, progress, ids = scan_progress()
            plugins = progress["scanProgress"][1]["HostProcess"]
            if kind == "duplicate":
                plugins.append(copy.deepcopy(plugins[0]))
            elif kind == "short":
                plugins[0]["Plugin"].pop()
            else:
                plugins[0] = {}
            with self.assertRaises(AssertionError):
                scanner().validate_scan_progress("0", scans, progress, ids)

    def test_actual_message_ids_must_be_retained_unique_and_positive(self):
        for invalid in ([], ["1", "1"], ["0"], ["-1"], ["01"], [True], [1], None):
            with self.subTest(ids=invalid):
                scans, progress, _ = scan_progress()
                with self.assertRaises(AssertionError):
                    scanner().validate_scan_progress("0", scans, progress, {"messagesIds": invalid})


class ObservedRouteTest(unittest.TestCase):
    def test_actual_absolute_and_relative_request_lines_supply_coverage(self):
        messages = [message("/"), message(ORIGIN + "/app/"),
                    message("/api/auth/session?fake-query=never-public"),
                    message("/api/unknown")]
        before = copy.deepcopy(messages)
        self.assertEqual(driver.observed_routes(messages, ORIGIN),
                         {"ROOT", "APP", "SESSION", "ERROR_PROBE"})
        self.assertEqual(messages, before)

    def test_unknown_owned_path_is_not_invented_as_planned_coverage(self):
        self.assertEqual(driver.observed_routes([message("/unrecognized")], ORIGIN), set())

    def test_request_authority_and_host_must_both_be_exact(self):
        malformed = [
            message("/app/", host="other.test:8443"),
            message("https://other.test:8443/app/"),
            message("http://plinth.test:8443/app/"),
            message("//other.test/app/"),
            message("https://user:pass@plinth.test:8443/app/"),
            {"requestHeader": "GET /app/ HTTP/1.1\r\n\r\n"},
            {"requestHeader": "GET /app/ HTTP/1.1\r\nHost: " + HOST +
             "\r\nHost: " + HOST + "\r\n\r\n"},
        ]
        for value in malformed:
            with self.subTest(value=value):
                with self.assertRaises(AssertionError):
                    driver.observed_routes([value], ORIGIN)

    def test_method_protocol_and_message_shape_are_real_http_not_arbitrary_tokens(self):
        malformed = [
            None, {}, {"requestHeader": None}, message(method="CONNECT"),
            message(method=""), message(method="FAKE"), message(protocol="FAKE/1"),
            {"requestHeader": "GET  /app/ HTTP/1.1\r\nHost: " + HOST + "\r\n\r\n"},
        ]
        for value in malformed:
            with self.subTest(value=value):
                with self.assertRaises(AssertionError):
                    driver.observed_routes([value], ORIGIN)


class BrowserReceiptTest(unittest.TestCase):
    def test_accepted_receipt_is_not_mutated(self):
        value = browser_receipt()
        before = copy.deepcopy(value)
        driver.DastHarness.validate_browser_receipt(value)
        self.assertEqual(value, before)

    def test_nonterminal_stage_or_completed_flag_is_rejected(self):
        for field, invalid in (("completed", False), ("completed", 1),
                               ("stage", "LOGOUT"), ("schema", "unrecognized")):
            value = browser_receipt()
            value[field] = invalid
            with self.assertRaises(AssertionError):
                driver.DastHarness.validate_browser_receipt(value)

    def test_missing_or_false_or_nonboolean_controls_and_cleanup_are_rejected(self):
        for section in ("controls", "cleanup"):
            for field in browser_receipt()[section]:
                for invalid in (False, 1, "true", None):
                    with self.subTest(section=section, field=field, invalid=invalid):
                        value = browser_receipt()
                        value[section][field] = invalid
                        with self.assertRaises(AssertionError):
                            driver.DastHarness.validate_browser_receipt(value)
                value = browser_receipt()
                del value[section][field]
                with self.assertRaises(AssertionError):
                    driver.DastHarness.validate_browser_receipt(value)

    def test_every_error_count_must_be_present_integer_zero(self):
        for field in browser_receipt()["counts"]:
            for invalid in (1, -1, False, 0.0, "0", None):
                with self.subTest(field=field, invalid=invalid):
                    value = browser_receipt()
                    value["counts"][field] = invalid
                    with self.assertRaises(AssertionError):
                        driver.DastHarness.validate_browser_receipt(value)
            value = browser_receipt()
            del value["counts"][field]
            with self.assertRaises(AssertionError):
                driver.DastHarness.validate_browser_receipt(value)

    def test_required_browser_routes_need_actual_positive_request_counts(self):
        for label in browser_receipt()["routes"]:
            for invalid in (0, True, -1, "1", 1.0):
                with self.subTest(label=label, invalid=invalid):
                    value = browser_receipt()
                    value["routes"][label]["requests"] = invalid
                    with self.assertRaises(AssertionError):
                        driver.DastHarness.validate_browser_receipt(value)
            value = browser_receipt()
            del value["routes"][label]
            with self.assertRaises(AssertionError):
                driver.DastHarness.validate_browser_receipt(value)

    def test_missing_or_multiple_unconnected_ungranted_socket_cannot_pass(self):
        for invalid in (None, {}, {"connections": 0, "connected": True, "applicationGrant": True},
                        {"connections": 2, "connected": True, "applicationGrant": True},
                        {"connections": True, "connected": True, "applicationGrant": True},
                        {"connections": 1, "connected": False, "applicationGrant": True},
                        {"connections": 1, "connected": True, "applicationGrant": False}):
            value = browser_receipt()
            value["proof"]["websocket"] = invalid
            with self.assertRaises(AssertionError):
                driver.DastHarness.validate_browser_receipt(value)

    def test_logout_requires_session_rejection_and_original_socket_closed(self):
        for invalid in ({}, {"sessionStatus": 200, "originalSocketClosed": True},
                        {"sessionStatus": 401, "originalSocketClosed": False},
                        {"sessionStatus": 401, "originalSocketClosed": 1}):
            value = browser_receipt()
            value["proof"]["logout"] = invalid
            with self.assertRaises(AssertionError):
                driver.DastHarness.validate_browser_receipt(value)


class ScannerOwnershipTest(unittest.TestCase):
    def test_partial_creation_is_cleaned_even_before_started_flag(self):
        value = scanner()
        value.creation_attempted = True
        owned = [{"Config": {"Labels": {"plinth.test.owner": OWNER}}}]
        value.harness.run.side_effect = [
            result(json.dumps(owned)), result(), result(returncode=1, stderr="No such object"),
        ]
        value.cleanup()
        commands = [call.args[0] for call in value.harness.run.call_args_list]
        self.assertIn(["docker", "rm", "--force", "--volumes", value.name], commands)
        self.assertFalse(value.started)

    def test_owned_label_mismatch_never_removes_another_container(self):
        value = scanner()
        value.creation_attempted = True
        value.harness.run.return_value = result(json.dumps([
            {"Config": {"Labels": {"plinth.test.owner": "other-owner"}}},
        ]))
        with self.assertRaises(AssertionError):
            value.cleanup()
        self.assertFalse(any(call.args[0][1] == "rm" for call in value.harness.run.call_args_list))

    def test_inspect_failure_is_not_recast_as_absence(self):
        value = scanner()
        value.creation_attempted = True
        value.harness.run.return_value = result(returncode=1, stderr="fake inspect unavailable")
        with self.assertRaises(AssertionError):
            value.cleanup()
        self.assertFalse(any(call.args[0][1] == "rm" for call in value.harness.run.call_args_list))


class InfrastructureOwnershipTest(unittest.TestCase):
    def configured(self):
        value = harness()
        value.local_tag = "127.0.0.1:4444/plinth:fake-owned"
        value.infrastructure_attempted = False
        return value

    def test_existing_candidate_tag_never_creates_or_claims_infrastructure(self):
        value = self.configured()
        value.run.return_value = result("fake existing image")
        with mock.patch.object(driver.Harness, "create_infrastructure", autospec=True) as create:
            with self.assertRaises(AssertionError):
                value.create_infrastructure()
        create.assert_not_called()
        self.assertFalse(value.infrastructure_attempted)
        value.run.assert_called_once_with(
            ["docker", "image", "inspect", value.local_tag], check=False, timeout=15)

    def test_unavailable_tag_inspection_never_creates_or_claims_infrastructure(self):
        for failure in (result(returncode=1, stderr="fake daemon unavailable"),
                        RuntimeError("fake inspect transport failure")):
            with self.subTest(failure=type(failure).__name__):
                value = self.configured()
                value.run.side_effect = [failure]
                expected = RuntimeError if isinstance(failure, RuntimeError) else AssertionError
                with mock.patch.object(driver.Harness, "create_infrastructure", autospec=True) as create:
                    with self.assertRaises(expected):
                        value.create_infrastructure()
                create.assert_not_called()
                self.assertFalse(value.infrastructure_attempted)
                value.run.assert_called_once_with(
                    ["docker", "image", "inspect", value.local_tag], check=False, timeout=15)

    def test_missing_transport_file_is_not_candidate_tag_absence(self):
        value = self.configured()
        value.run.return_value = result(
            returncode=1, stderr="dial unix /fake-fixture/docker.sock: no such file or directory")
        with mock.patch.object(driver.Harness, "create_infrastructure", autospec=True) as create:
            with self.assertRaises(AssertionError):
                value.create_infrastructure()
        create.assert_not_called()
        self.assertFalse(value.infrastructure_attempted)
        value.run.assert_called_once_with(
            ["docker", "image", "inspect", value.local_tag], check=False, timeout=15)

    def test_other_missing_image_is_not_exact_candidate_tag_absence(self):
        for tag in ("127.0.0.1:4444/plinth:other-owned", "127.0.0.1:4444/plinth:fake-owned-other"):
            with self.subTest(tag=tag):
                value = self.configured()
                value.run.return_value = result(
                    returncode=1, stderr=f"Error response from daemon: No such image: {tag}")
                with mock.patch.object(driver.Harness, "create_infrastructure", autospec=True) as create:
                    with self.assertRaises(AssertionError):
                        value.create_infrastructure()
                create.assert_not_called()
                self.assertFalse(value.infrastructure_attempted)
                value.run.assert_called_once_with(
                    ["docker", "image", "inspect", value.local_tag], check=False, timeout=15)

    def test_true_tag_absence_and_free_names_claim_before_super_creation(self):
        value = self.configured()
        value.run.side_effect = [result(
            returncode=1, stderr=f"Error response from daemon: No such image: {value.local_tag}"),
                                 result(), result()]

        def create(owned):
            self.assertIs(owned, value)
            self.assertTrue(owned.infrastructure_attempted)

        with mock.patch.object(driver.Harness, "create_infrastructure", autospec=True,
                               side_effect=create) as create_mock:
            value.create_infrastructure()
        create_mock.assert_called_once_with(value)
        self.assertTrue(value.infrastructure_attempted)
        self.assertEqual(value.run.call_args_list, [
            mock.call(["docker", "image", "inspect", value.local_tag], check=False, timeout=15),
            mock.call(["docker", "ps", "-a", "--format", "{{.Names}}"], timeout=15),
            mock.call(["docker", "network", "ls", "--format", "{{.Name}}"], timeout=15),
        ])


class VolumeOwnershipTest(unittest.TestCase):
    def test_inventory_uses_only_mounts_of_exact_owned_containers(self):
        value = harness()
        owned_node = "k3d-" + value.cluster + "-server-0"
        owned_registry = "k3d-" + value.registry
        names = [owned_node, owned_registry, value.scanner.name,
                 "unrelated-new-container", "k3d-plinth-issue36-other-server-0"]
        mounts = [{"Type": "volume", "Name": "owned-new-volume"},
                  {"Type": "volume", "Name": "preexisting-shared-volume"},
                  {"Type": "bind", "Name": "not-a-volume"}]

        def run(argv, **_kwargs):
            if argv[1] == "ps":
                return result("\n".join(names))
            self.assertEqual(argv[:2], ["docker", "inspect"])
            self.assertIn(argv[2], (owned_node, owned_registry, value.scanner.name))
            return result(json.dumps([{"Mounts": copy.deepcopy(mounts)}]))

        value.run.side_effect = run
        value.inventory_owned_volumes()
        self.assertEqual(value.owned_volumes, {"owned-new-volume"})
        inspected = [call.args[0][2] for call in value.run.call_args_list
                     if call.args[0][1] == "inspect"]
        self.assertCountEqual(inspected, [owned_node, owned_registry, value.scanner.name])

    def test_global_new_volume_differences_do_not_authorize_removal(self):
        value = harness()
        value.owned_volumes = {"owned-new-volume"}

        def run(argv, **_kwargs):
            if argv[1:3] == ["volume", "inspect"]:
                return result("[]")
            if argv[1] == "ps":
                return result()
            if argv[1:3] == ["volume", "rm"]:
                self.assertEqual(argv, ["docker", "volume", "rm", "owned-new-volume"])
                return result()
            self.assertEqual(argv[1:3], ["volume", "ls"])
            return result("preexisting-shared-volume\nunrelated-new-volume\n")

        value.run.side_effect = run
        self.assertEqual(value.remove_owned_volumes(), 0)
        removed = [call.args[0][-1] for call in value.run.call_args_list
                   if call.args[0][1:3] == ["volume", "rm"]]
        self.assertEqual(removed, ["owned-new-volume"])

    def test_mounted_volume_is_never_removed(self):
        value = harness()
        value.owned_volumes = {"owned-new-volume"}
        value.run.side_effect = [result("[]"), result("unrelated-container-still-mounted")]
        with self.assertRaises(AssertionError):
            value.remove_owned_volumes()
        self.assertFalse(any(call.args[0][1:3] == ["volume", "rm"]
                             for call in value.run.call_args_list))


class TlsOwnershipTest(unittest.TestCase):
    def test_missing_context_creates_fixture_context_and_owns_socket(self):
        value = harness()
        context, peer, secured = mock.Mock(), mock.Mock(), mock.Mock()
        context.wrap_socket.return_value = secured
        with mock.patch.object(driver.ssl, "create_default_context", return_value=context) as factory, \
                mock.patch.object(driver.socket, "create_connection", return_value=peer) as connect:
            self.assertIs(value.connect_tls(), secured)
        factory.assert_called_once_with(cafile=str(value.certificate))
        connect.assert_called_once_with(("127.0.0.1", 8443), timeout=15)
        context.wrap_socket.assert_called_once_with(peer, server_hostname="plinth.test")
        secured.settimeout.assert_called_once_with(15)

    def test_failed_tls_wrap_closes_only_its_owned_peer(self):
        value = harness()
        context, peer = mock.Mock(), mock.Mock()
        context.wrap_socket.side_effect = RuntimeError("fake wrap failure")
        with mock.patch.object(driver.socket, "create_connection", return_value=peer):
            with self.assertRaises(RuntimeError):
                value.connect_tls(context)
        peer.close.assert_called_once_with()


class IndependentCleanupTest(unittest.TestCase):
    def configured(self, failure=None, *, kubeconfig=True, attempted=True):
        value = harness()
        value.cleanup_complete = False
        value.infrastructure_attempted = attempted
        value.kubeconfig = SimpleNamespace(is_file=mock.Mock(return_value=kubeconfig))
        value.chart_installed = True
        value.namespace_created = True
        value.namespace = "fake-task-namespace"
        value.release = "fake-task-release"
        value.local_tag = "127.0.0.1:4444/plinth:fake-owned"
        value.args = SimpleNamespace(k3d="k3d")
        value.private_details = {}
        events = []

        def step(label):
            events.append(label)
            if label == failure:
                raise RuntimeError("fake cleanup stage failure")
            return result()

        def run(argv, **_kwargs):
            if argv[:3] == ["k3d", "cluster", "delete"]:
                self.assertEqual(argv[3], value.cluster)
                return step("CLUSTER")
            if argv[:3] == ["k3d", "registry", "delete"]:
                self.assertEqual(argv[3], value.registry)
                return step("REGISTRY")
            self.assertEqual(argv, ["docker", "image", "inspect", value.local_tag])
            step("LOCAL_TAG")
            return result(returncode=1, stderr="No such image")

        value.run.side_effect = run
        value.helm = mock.Mock(side_effect=lambda *_args, **_kwargs: step("HELM"))
        value.kubectl = mock.Mock(side_effect=lambda *_args, **_kwargs: step("NAMESPACE"))
        value.temporary = SimpleNamespace(cleanup=mock.Mock(side_effect=lambda: step("TEMPORARY") and None))
        return value, events

    def test_each_failure_still_attempts_later_independently_owned_cleanup(self):
        expected = ["HELM", "NAMESPACE", "CLUSTER", "REGISTRY", "LOCAL_TAG", "TEMPORARY"]
        for failure in expected:
            with self.subTest(failure=failure):
                value, events = self.configured(failure)
                with self.assertRaises(AssertionError):
                    value.cleanup()
                self.assertEqual(events, expected)
                self.assertFalse(value.cleanup_complete)
                self.assertEqual([item["stage"] for item in value.private_details["cleanupErrors"]],
                                 [failure])

    def test_successful_cleanup_is_idempotent(self):
        value, events = self.configured()
        value.cleanup()
        before = events[:]
        self.assertTrue(value.cleanup_complete)
        value.cleanup()
        self.assertEqual(events, before)

    def test_missing_partial_kubeconfig_does_not_skip_cluster_registry_or_temp(self):
        value, events = self.configured(kubeconfig=False)
        value.cleanup()
        self.assertEqual(events, ["CLUSTER", "REGISTRY", "LOCAL_TAG", "TEMPORARY"])
        value.helm.assert_not_called()
        value.kubectl.assert_not_called()

    def test_before_infrastructure_attempt_only_own_temporary_is_cleaned(self):
        value, events = self.configured(attempted=False)
        value.cleanup()
        self.assertEqual(events, ["TEMPORARY"])
        value.run.assert_not_called()

    def test_watchers_are_stopped_before_deleting_their_cluster(self):
        value, events = self.configured()
        first, second = object(), object()
        value._exit_watchers = [first, second]

        def stop(watcher):
            self.assertIn(watcher, (first, second))
            events.append("EXIT_OBSERVER")
            value._exit_watchers.remove(watcher)

        value._stop_container_exit_watch = mock.Mock(side_effect=stop)
        value.cleanup()
        self.assertEqual(events[:2], ["EXIT_OBSERVER", "EXIT_OBSERVER"])
        self.assertLess(events.index("EXIT_OBSERVER"), events.index("CLUSTER"))
        self.assertEqual(value._exit_watchers, [])
        self.assertTrue(value.cleanup_complete)

    def test_watcher_failure_still_attempts_other_watchers_and_all_later_cleanup(self):
        value, events = self.configured()
        first, second = object(), object()
        value._exit_watchers = [first, second]

        def stop(watcher):
            events.append("EXIT_OBSERVER")
            if watcher is first:
                raise RuntimeError("fake watcher teardown failure")
            value._exit_watchers.remove(watcher)

        value._stop_container_exit_watch = mock.Mock(side_effect=stop)
        with self.assertRaises(AssertionError):
            value.cleanup()
        self.assertEqual(events, ["EXIT_OBSERVER", "EXIT_OBSERVER", "HELM", "NAMESPACE",
                                  "CLUSTER", "REGISTRY", "LOCAL_TAG", "TEMPORARY"])
        self.assertEqual(value._exit_watchers, [first])
        self.assertFalse(value.cleanup_complete)
        self.assertEqual([item["stage"] for item in value.private_details["cleanupErrors"]],
                         ["EXIT_OBSERVER", "EXIT_OBSERVER"])

    def test_nominal_stop_that_leaves_owned_watcher_present_remains_red(self):
        value, events = self.configured()
        value._exit_watchers = [object()]
        value._stop_container_exit_watch = mock.Mock(side_effect=lambda _watcher: events.append("EXIT_OBSERVER"))
        with self.assertRaises(AssertionError):
            value.cleanup()
        self.assertEqual(events, ["EXIT_OBSERVER", "HELM", "NAMESPACE", "CLUSTER", "REGISTRY",
                                  "LOCAL_TAG", "TEMPORARY"])
        self.assertFalse(value.cleanup_complete)


class RunCleanupTest(unittest.TestCase):
    def test_partial_failure_never_skips_independent_owned_cleanup(self):
        for failure in ("execute_dast", "inventory_owned_volumes", "scanner",
                        "cleanup", "remove_owned_volumes", "private_retention"):
            with self.subTest(failure=failure):
                args = SimpleNamespace(
                    image="fake-exact-image", helm="helm", kubectl="kubectl", k3d="k3d",
                    report=Path("/tmp/plinth-issue40-unit/report.json"),
                    private_report=Path("/tmp/plinth-issue40-unit/private.json"),
                    dispositions=None,
                )
                value = SimpleNamespace(
                    execute_dast=mock.Mock(), inventory_owned_volumes=mock.Mock(),
                    retain_private_failure_evidence=mock.Mock(), cleanup_complete=True,
                    cleanup=mock.Mock(), remove_owned_volumes=mock.Mock(return_value=0),
                    scanner=SimpleNamespace(name="fake-owned-scanner", cleanup=mock.Mock(),
                                            completed=False, passive_remaining=0),
                    registry="fake-owned-registry", cluster="fake-owned-cluster",
                    namespace_created=False, root=mock.Mock(spec=Path),
                    private_details={}, raw_alerts=[], raw_messages=[], route_labels=set(),
                    controls={code: False for code in driver.REQUIRED_CONTROLS},
                    candidate_digest="sha256:" + "c" * 64, origin=ORIGIN,
                )
                value.root.exists.return_value = False
                target = value.scanner.cleanup if failure == "scanner" else getattr(value, failure, None)
                if target is not None:
                    target.side_effect = RuntimeError("fake partial failure")

                def check_output(argv, **_kwargs):
                    if argv in (SOURCE_STATUS_COMMAND, SOURCE_CHART_COMMAND):
                        return ""
                    if argv == ["git", "rev-parse", "HEAD"]:
                        return "a" * 40
                    if argv == ["git", "rev-parse", "HEAD^{tree}"]:
                        return "b" * 40
                    if argv == ["docker", "image", "inspect", args.image]:
                        return json.dumps([{"Config": {"Labels": {
                            "org.opencontainers.image.revision": "a" * 40,
                            "org.opencontainers.image.version": "0.6.5",
                        }}}])
                    if argv[:2] == ["docker", "ps"] or argv[:3] == ["docker", "network", "ls"]:
                        return ""
                    self.fail("unexpected external command in mocked driver")

                def retain(path, _value):
                    if failure == "private_retention" and path is args.private_report:
                        raise OSError("fake private retention failure")

                with mock.patch.object(driver, "platform_is_amd64", return_value=True), \
                        mock.patch.object(driver.os, "umask"), \
                        mock.patch.object(driver.shutil, "which", return_value="/fake/tool"), \
                        mock.patch.object(driver.subprocess, "check_output", side_effect=check_output), \
                        mock.patch.object(Path, "read_text", return_value="0.6.5"), \
                        mock.patch.object(Path, "exists", return_value=False), \
                        mock.patch.object(Path, "is_dir", return_value=True), \
                        mock.patch.object(Path, "resolve", autospec=True, side_effect=lambda path: path), \
                        mock.patch.object(driver, "DastHarness", return_value=value), \
                        mock.patch.object(driver, "private_json", side_effect=retain), \
                        mock.patch("builtins.print"):
                    self.assertEqual(driver.run(args), 1)
                value.inventory_owned_volumes.assert_called_once_with()
                value.scanner.cleanup.assert_called_once_with()
                value.cleanup.assert_called_once_with()
                value.remove_owned_volumes.assert_called_once_with()

    def assert_destination_rejected(self, report, private_report, *, missing_parent=None):
        args = SimpleNamespace(image="fake-exact-image", helm="helm", kubectl="kubectl", k3d="k3d",
                               report=report, private_report=private_report, dispositions=None)

        def check_output(argv, **_kwargs):
            if argv in (SOURCE_STATUS_COMMAND, SOURCE_CHART_COMMAND):
                return ""
            if argv == ["git", "rev-parse", "HEAD"]:
                return "a" * 40
            if argv == ["git", "rev-parse", "HEAD^{tree}"]:
                return "b" * 40
            self.assertEqual(argv, ["docker", "image", "inspect", args.image])
            return json.dumps([{"Config": {"Labels": {
                "org.opencontainers.image.revision": "a" * 40,
                "org.opencontainers.image.version": "0.6.5",
            }}}])

        with mock.patch.object(driver, "platform_is_amd64", return_value=True), \
                mock.patch.object(driver.os, "umask"), \
                mock.patch.object(driver.shutil, "which", return_value="/fake/tool"), \
                mock.patch.object(driver.subprocess, "check_output", side_effect=check_output), \
                mock.patch.object(Path, "read_text", return_value="0.6.5"), \
                mock.patch.object(Path, "exists", return_value=False), \
                mock.patch.object(Path, "is_dir", autospec=True,
                                  side_effect=lambda path: path != missing_parent), \
                mock.patch.object(Path, "resolve", autospec=True, side_effect=lambda path: path), \
                mock.patch.object(driver, "DastHarness") as construct, \
                mock.patch.object(driver, "private_json") as write:
            with self.assertRaises(AssertionError):
                driver.run(args)
        construct.assert_not_called()
        write.assert_not_called()

    def test_missing_public_or_private_parent_refuses_before_harness_construction(self):
        report = Path("/tmp/plinth-issue40-public-unit/report.json")
        private_report = Path("/tmp/plinth-issue40-private-unit/private.json")
        for parent in (report.parent, private_report.parent):
            with self.subTest(parent=parent):
                self.assert_destination_rejected(report, private_report, missing_parent=parent)

    def test_public_or_private_paths_outside_owned_tmp_refuse_before_harness_construction(self):
        report = Path("/tmp/plinth-issue40-public-unit/report.json")
        private_report = Path("/tmp/plinth-issue40-private-unit/private.json")
        for outside in (Path("/tmp/unrelated-unit/report.json"),
                        Path("/var/tmp/plinth-issue40-unit/report.json"),
                        driver.ROOT / "report.json"):
            for field in ("report", "private_report"):
                with self.subTest(outside=outside, field=field):
                    self.assert_destination_rejected(
                        outside if field == "report" else report,
                        outside if field == "private_report" else private_report)

    def assert_source_rejected(self, status, *, chart_additions=""):
        args = SimpleNamespace(image="fake-exact-image", helm="helm", kubectl="kubectl", k3d="k3d",
                               report=Path("/tmp/plinth-issue40-public-unit/report.json"),
                               private_report=Path("/tmp/plinth-issue40-private-unit/private.json"),
                               dispositions=None)
        commands = []

        def check_output(argv, **kwargs):
            commands.append(argv)
            if argv in (SOURCE_STATUS_COMMAND, SOURCE_CHART_COMMAND):
                self.assertEqual(kwargs, {"cwd": driver.ROOT, "text": True})
                response = status if argv == SOURCE_STATUS_COMMAND else chart_additions
                if isinstance(response, Exception):
                    raise response
                return response
            if argv == ["git", "rev-parse", "HEAD"]:
                return "a" * 40
            if argv == ["git", "rev-parse", "HEAD^{tree}"]:
                return "b" * 40
            self.assertEqual(argv, ["docker", "image", "inspect", args.image])
            return json.dumps([{"Config": {"Labels": {
                "org.opencontainers.image.revision": "a" * 40,
                "org.opencontainers.image.version": "0.6.5",
            }}}])

        with mock.patch.object(driver, "platform_is_amd64", return_value=True), \
                mock.patch.object(driver.os, "umask"), \
                mock.patch.object(driver.shutil, "which", return_value="/fake/tool"), \
                mock.patch.object(driver.subprocess, "check_output", side_effect=check_output), \
                mock.patch.object(Path, "read_text", return_value="0.6.5"), \
                mock.patch.object(Path, "exists", return_value=False), \
                mock.patch.object(Path, "is_dir", return_value=True), \
                mock.patch.object(Path, "resolve", autospec=True, side_effect=lambda path: path), \
                mock.patch.object(driver, "DastHarness",
                                  side_effect=RuntimeError("fake forbidden construction")) as construct, \
                mock.patch.object(driver, "private_json") as write:
            failure = status if isinstance(status, Exception) else chart_additions
            expected = type(failure) if isinstance(failure, Exception) else AssertionError
            with self.assertRaises(expected):
                driver.run(args)
        construct.assert_not_called()
        write.assert_not_called()
        self.assertFalse(any(command[:3] == ["docker", "image", "inspect"] for command in commands))

    def test_unstaged_chart_input_refuses_before_image_inspection_and_harness(self):
        self.assert_source_rejected(" M deploy/helm/plinth/values.yaml\n")

    def test_staged_browser_input_refuses_before_image_inspection_and_harness(self):
        self.assert_source_rejected("M  tests/browser/ingress-dast.mjs\n")

    def test_untracked_chart_input_refuses_before_image_inspection_and_harness(self):
        self.assert_source_rejected("?? deploy/helm/plinth/templates/fake-extra.yaml\n")

    def test_dirty_submodule_input_refuses_before_image_inspection_and_harness(self):
        self.assert_source_rejected(" m third_party/fake-submodule\n")

    def test_failed_source_status_is_not_recast_as_clean(self):
        self.assert_source_rejected(driver.subprocess.CalledProcessError(
            1, SOURCE_STATUS_COMMAND, output="fake unavailable status"))

    def test_ignored_chart_extra_refuses_even_when_standard_status_is_empty(self):
        self.assert_source_rejected(
            "", chart_additions="deploy/helm/plinth/templates/build/fake-extra.yaml\n")

    def test_failed_ignored_chart_inventory_is_not_recast_as_clean(self):
        self.assert_source_rejected("", chart_additions=driver.subprocess.CalledProcessError(
            1, SOURCE_CHART_COMMAND, output="fake unavailable chart inventory"))

    def run_with_source_change(self, change):
        args = SimpleNamespace(image="fake-exact-image", helm="helm", kubectl="kubectl", k3d="k3d",
                               report=Path("/tmp/plinth-issue40-public-unit/report.json"),
                               private_report=Path("/tmp/plinth-issue40-private-unit/private.json"),
                               dispositions=None)
        events, reports, counts = [], {}, {}

        def step(name):
            def invoke():
                events.append(name)
                return 0 if name == "VOLUMES" else None
            return mock.Mock(side_effect=invoke)

        value = SimpleNamespace(
            execute_dast=step("EXECUTE"), inventory_owned_volumes=step("INVENTORY"),
            retain_private_failure_evidence=step("RETAIN"), cleanup_complete=True,
            cleanup=step("CLEANUP"), remove_owned_volumes=step("VOLUMES"),
            scanner=SimpleNamespace(name="fake-owned-scanner", cleanup=step("SCANNER"),
                                    completed=True, passive_remaining=0),
            registry="fake-owned-registry", cluster="fake-owned-cluster", namespace_created=False,
            root=mock.Mock(spec=Path), private_details={}, raw_alerts=[], raw_messages=[message()],
            route_labels=set(REQUIRED_ROUTES), controls={code: True for code in driver.REQUIRED_CONTROLS},
            candidate_digest="sha256:" + "c" * 64, origin=ORIGIN,
        )
        value.root.exists.return_value = False

        def check_output(argv, **kwargs):
            if argv[0] == "git":
                self.assertEqual(kwargs, {"cwd": driver.ROOT, "text": True})
                key = tuple(argv)
                counts[key] = counts.get(key, 0) + 1
                late = counts[key] == 2
                if argv == SOURCE_STATUS_COMMAND:
                    events.append("SOURCE_FINAL" if late else "SOURCE_INITIAL")
                    if late and change == "status_error":
                        raise driver.subprocess.CalledProcessError(1, argv, output="fake raw Git detail")
                    return " M tests/browser/ingress-dast.mjs\n" if late and change == "status" else ""
                if argv == SOURCE_CHART_COMMAND:
                    if late and change == "chart_error":
                        raise driver.subprocess.CalledProcessError(1, argv, output="fake raw chart detail")
                    return ("deploy/helm/plinth/templates/build/fake-extra.yaml\n"
                            if late and change == "chart" else "")
                if argv == ["git", "rev-parse", "HEAD"]:
                    return "d" * 40 if late and change == "HEAD" else "a" * 40
                self.assertEqual(argv, ["git", "rev-parse", "HEAD^{tree}"])
                return "e" * 40 if late and change == "tree" else "b" * 40
            if argv == ["docker", "image", "inspect", args.image]:
                return json.dumps([{"Config": {"Labels": {
                    "org.opencontainers.image.revision": "a" * 40,
                    "org.opencontainers.image.version": "0.6.5",
                }}}])
            self.assertIn(argv, (
                ["docker", "ps", "-a", "--format", "{{.Names}}"],
                ["docker", "network", "ls", "--format", "{{.Name}}"],
            ))
            return ""

        def write(path, report):
            events.append("PRIVATE_WRITE" if path == args.private_report else "PUBLIC_WRITE")
            reports[path] = copy.deepcopy(report)

        with mock.patch.object(driver, "platform_is_amd64", return_value=True), \
                mock.patch.object(driver.os, "umask"), \
                mock.patch.object(driver.shutil, "which", return_value="/fake/tool"), \
                mock.patch.object(driver.subprocess, "check_output", side_effect=check_output), \
                mock.patch.object(Path, "read_text", return_value="0.6.5"), \
                mock.patch.object(Path, "exists", return_value=False), \
                mock.patch.object(Path, "is_dir", return_value=True), \
                mock.patch.object(Path, "resolve", autospec=True, side_effect=lambda path: path), \
                mock.patch.object(driver, "DastHarness", return_value=value) as construct, \
                mock.patch.object(driver, "private_json", side_effect=write), \
                mock.patch("builtins.print"):
            code = driver.run(args)
        construct.assert_called_once_with(args)
        for method in (value.execute_dast, value.inventory_owned_volumes,
                       value.retain_private_failure_evidence, value.scanner.cleanup,
                       value.cleanup, value.remove_owned_volumes):
            method.assert_called_once_with()
        self.assertLess(events.index("VOLUMES"), events.index("SOURCE_FINAL"))
        self.assertLess(events.index("SOURCE_FINAL"), events.index("PRIVATE_WRITE"))
        self.assertLess(events.index("PRIVATE_WRITE"), events.index("PUBLIC_WRITE"))
        return code, reports[args.report], reports[args.private_report], counts

    def test_clean_stable_source_completes_without_inventorying_ignored_browser_caches(self):
        code, public, private, counts = self.run_with_source_change(None)
        self.assertEqual(code, 0)
        self.assertEqual(public["status"], "COMPLETE")
        self.assertIsNone(private["error"])
        self.assertEqual(set(counts), {tuple(SOURCE_STATUS_COMMAND), tuple(SOURCE_CHART_COMMAND),
                                      ("git", "rev-parse", "HEAD"), ("git", "rev-parse", "HEAD^{tree}")})
        self.assertTrue(all(count == 2 for count in counts.values()))

    def test_late_source_mutation_or_query_error_keeps_cleanup_and_private_evidence_but_not_complete(self):
        for change in ("status", "chart", "HEAD", "tree", "status_error", "chart_error"):
            with self.subTest(change=change):
                code, public, private, _counts = self.run_with_source_change(change)
                self.assertEqual(code, 1)
                self.assertNotEqual(public["status"], "COMPLETE")
                self.assertEqual(private["error"], "source-candidate checkout changed during the scan")
                self.assertEqual(private["sourceRevision"], "a" * 40)
                self.assertEqual(private["sourceTree"], "b" * 40)
                self.assertNotIn("fake raw", json.dumps(public))


class WorkflowPrivacyTest(unittest.TestCase):
    def workflow(self):
        return driver.yaml.safe_load((driver.ROOT / ".github/workflows/runtime-image.yml").read_text())

    def assert_private_artifact_boundary(self, workflow):
        self.assertEqual(workflow["permissions"], {"contents": "read"})
        candidate = workflow["jobs"]["candidate"]
        self.assertEqual(candidate.get("permissions", workflow["permissions"]), {"contents": "read"})
        steps = candidate["steps"]
        scans = [step for step in steps if "python3 tests/deployment/ingress_dast_test.py"
                 in step.get("run", "")]
        self.assertEqual(len(scans), 1)
        scan = scans[0]
        self.assertEqual(scan["if"], "${{ matrix.arch == 'amd64' }}")
        self.assertIn("docker pull " + driver.SCANNER_IMAGE + "\n", scan["run"])
        self.assertIn("dast_dir=$(mktemp -d /tmp/plinth-issue40-ci.XXXXXX)", scan["run"])
        self.assertIn('--report "$dast_dir/report.json" --private-report "$dast_dir/private.json"',
                      scan["run"])
        uploads = [step for step in steps if step.get("uses", "").startswith("actions/upload-artifact@")]
        self.assertEqual(len(uploads), 1)
        upload = uploads[0]
        self.assertEqual(upload["if"], "${{ success() && matrix.arch == 'amd64' }}")
        self.assertEqual(upload["with"]["path"], "${{ env.PLINTH_DAST_REPORT_DIR }}/report.json")
        self.assertEqual(upload["with"]["if-no-files-found"], "error")
        cleanup = [step for step in steps if "rmdir --" in step.get("run", "")
                   and "PLINTH_DAST_REPORT_DIR" in step.get("run", "")]
        self.assertEqual(len(cleanup), 1)
        self.assertEqual(cleanup[0]["if"],
                         "${{ always() && matrix.arch == 'amd64' && env.PLINTH_DAST_REPORT_DIR != '' }}")
        self.assertEqual(cleanup[0]["shell"], "bash")
        script = cleanup[0]["run"]
        self.assertIn('[[ "$PLINTH_DAST_REPORT_DIR" =~ ^/tmp/plinth-issue40-ci\\.[[:alnum:]]{6}$ ]]', script)
        self.assertIn('test ! -L "$PLINTH_DAST_REPORT_DIR"', script)
        self.assertIn('rm -f -- "$PLINTH_DAST_REPORT_DIR/private.json" "$PLINTH_DAST_REPORT_DIR/report.json"',
                      script)
        self.assertIn('rmdir -- "$PLINTH_DAST_REPORT_DIR"', script)

    def test_current_candidate_has_readonly_exact_complete_report_only_publication(self):
        self.assert_private_artifact_boundary(self.workflow())

    def test_permission_upload_or_cleanup_scope_regression_is_rejected(self):
        original = self.workflow()
        for failure in ("global_write", "candidate_token", "scan_arm", "mutable_scanner",
                        "upload_directory", "upload_private", "upload_glob", "upload_on_failure",
                        "cleanup_not_always", "cleanup_missing_private"):
            with self.subTest(failure=failure):
                workflow = copy.deepcopy(original)
                candidate = workflow["jobs"]["candidate"]
                steps = candidate["steps"]
                scan = next(step for step in steps if "python3 tests/deployment/ingress_dast_test.py"
                            in step.get("run", ""))
                upload = next(step for step in steps if step.get("uses", "").startswith("actions/upload-artifact@"))
                cleanup = next(step for step in steps if "rmdir --" in step.get("run", ""))
                if failure == "global_write":
                    workflow["permissions"]["contents"] = "write"
                elif failure == "candidate_token":
                    candidate["permissions"] = {"contents": "read", "id-token": "write"}
                elif failure == "scan_arm":
                    scan["if"] = "${{ matrix.arch == 'arm64' }}"
                elif failure == "mutable_scanner":
                    scan["run"] = scan["run"].replace(driver.SCANNER_IMAGE, "ghcr.io/zaproxy/zaproxy:latest")
                elif failure.startswith("upload_") and failure != "upload_on_failure":
                    suffix = {"upload_directory": "", "upload_private": "/private.json", "upload_glob": "/*"}[failure]
                    upload["with"]["path"] = "${{ env.PLINTH_DAST_REPORT_DIR }}" + suffix
                elif failure == "upload_on_failure":
                    upload["if"] = "${{ always() && matrix.arch == 'amd64' }}"
                elif failure == "cleanup_not_always":
                    cleanup["if"] = "${{ success() && matrix.arch == 'amd64' }}"
                else:
                    cleanup["run"] = cleanup["run"].replace('"$PLINTH_DAST_REPORT_DIR/private.json"', "")
                with self.assertRaises(AssertionError):
                    self.assert_private_artifact_boundary(workflow)


if __name__ == "__main__":
    unittest.main()
