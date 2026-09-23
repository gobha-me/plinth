#!/usr/bin/env python3
"""Keep the k3d exit observer exact and independent of CRI garbage collection."""

from __future__ import annotations

import json
import unittest

from k3d_lifecycle_test import Harness, parse_containerd_exit_event


CONTAINER_ID = "a" * 64
OTHER_ID = "b" * 64
EXITED_AT = "2026-09-23T19:00:00.123456789Z"
PREFIX = "2026-09-23 19:00:00.123456789 +0000 UTC"


def event(*, container_id=CONTAINER_ID, task_id=CONTAINER_ID,
          exit_status=0, exited_at=EXITED_AT,
          namespace="k8s.io", topic="/tasks/exit"):
    body = {"container_id": container_id, "id": task_id,
            "exit_status": exit_status, "exited_at": exited_at}
    return f"{PREFIX} {namespace} {topic} {json.dumps(body)}"


class ContainerdExitEventTest(unittest.TestCase):
    def test_exact_init_exit_retains_code_and_finish_time(self):
        status = parse_containerd_exit_event(event(), CONTAINER_ID)
        self.assertIsNotNone(status)
        self.assertEqual(status["state"], "CONTAINER_EXITED")
        self.assertEqual(status["exitCode"], 0)
        self.assertGreater(Harness.timestamp_ns(status, "finishedAt"), 0)

    def test_nonzero_init_exit_is_observed_not_recast_as_success(self):
        status = parse_containerd_exit_event(
            event(exit_status=137), CONTAINER_ID)
        self.assertIsNotNone(status)
        self.assertEqual(status["exitCode"], 137)

    def test_omitted_proto3_zero_exit_status_is_success(self):
        payload = {"container_id": CONTAINER_ID, "id": CONTAINER_ID,
                   "exited_at": EXITED_AT}
        status = parse_containerd_exit_event(
            f"{PREFIX} k8s.io /tasks/exit {json.dumps(payload)}", CONTAINER_ID)
        self.assertIsNotNone(status)
        self.assertEqual(status["exitCode"], 0)

    def test_exec_child_and_foreign_container_do_not_count(self):
        self.assertIsNone(parse_containerd_exit_event(
            event(task_id=OTHER_ID), CONTAINER_ID))
        self.assertIsNone(parse_containerd_exit_event(
            event(container_id=OTHER_ID, task_id=OTHER_ID), CONTAINER_ID))

    def test_wrong_namespace_or_topic_does_not_count(self):
        self.assertIsNone(parse_containerd_exit_event(
            event(namespace="default"), CONTAINER_ID))
        self.assertIsNone(parse_containerd_exit_event(
            event(topic="/tasks/start"), CONTAINER_ID))

    def test_incomplete_or_malformed_event_does_not_count(self):
        self.assertIsNone(parse_containerd_exit_event(
            f"{PREFIX} k8s.io /tasks/exit {{bad json", CONTAINER_ID))
        self.assertIsNone(parse_containerd_exit_event(
            event(exited_at="not-a-time"), CONTAINER_ID))
        self.assertIsNone(parse_containerd_exit_event(
            event(exit_status="zero"), CONTAINER_ID))


if __name__ == "__main__":
    unittest.main()
