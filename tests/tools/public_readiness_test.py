#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Hermetic controls for the actual finite deployment-readiness workflow guard."""

import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("public_readiness", ROOT / "tools/public_readiness.py")
VERIFIER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFIER)
WORKFLOW = ROOT / ".github/workflows/runtime-image.yml"
LIVE = 'python3 tests/deployment/k3d_lifecycle_test.py --image "$image" --kubernetes '
EVIDENCE = '"$RUNNER_TEMP/helper-runtime-identity-{}-$GITHUB_RUN_ID.json"'


class LifecycleReadinessTest(unittest.TestCase):
    def setUp(self):
        self.workflow = WORKFLOW.read_text(encoding="utf-8")
        self.candidate = VERIFIER.workflow_job(self.workflow, "candidate")
        self.minimum = LIVE + 'min \\\n            --runtime-identity-evidence ' + EVIDENCE.format("min")
        self.maximum = LIVE + 'max \\\n            --runtime-identity-evidence ' + EVIDENCE.format("max")
        self.assertIn(self.minimum, self.candidate)
        self.assertIn(self.maximum, self.candidate)

    def reject(self, text):
        errors = VERIFIER.check_live_lifecycle_commands(text)
        self.assertTrue(errors)
        self.assertIn("exact helper evidence paths", errors[0])

    def test_current_continued_commands_and_actual_deployment_verifier_pass(self):
        self.assertEqual(VERIFIER.check_live_lifecycle_commands(self.candidate), [])
        self.assertEqual(VERIFIER.check_kubernetes_deployment(), [])

    def test_straight_line_spelling_whitespace_and_whole_line_comments_pass(self):
        single_line = self.candidate.replace("\\\n            --runtime-identity-evidence", "--runtime-identity-evidence")
        single_line = single_line.replace("          set -euo pipefail", "          # ordinary comment\n          set   -euo   pipefail")
        self.assertEqual(VERIFIER.check_live_lifecycle_commands(single_line), [])

    def test_missing_swapped_reused_or_wrong_evidence_paths_fail(self):
        replacements = [
            (self.minimum, LIVE + "min"),
            (self.maximum, LIVE + "max"),
            (EVIDENCE.format("min"), EVIDENCE.format("max")),
            (EVIDENCE.format("max"), EVIDENCE.format("min")),
            (EVIDENCE.format("min"), '"relative-output.json"'),
            ("--runtime-identity-evidence", "--other-evidence"),
        ]
        for before, after in replacements:
            with self.subTest(before=before, after=after):
                self.reject(self.candidate.replace(before, after, 1))

    def test_commented_echo_heredoc_function_or_if_false_commands_do_not_count(self):
        for replacement in (
            "# " + self.minimum,
            "echo '" + self.minimum + "'",
            "cat <<'DEAD'\n          " + self.minimum + "\n          DEAD",
            "unused() {\n          " + self.minimum + "\n          }",
            "if false; then\n          " + self.minimum + "\n          fi",
            self.minimum + " || true",
        ):
            with self.subTest(replacement=replacement):
                self.reject(self.candidate.replace(self.minimum, replacement, 1))

    def test_dead_old_commands_outside_step_cannot_replace_executed_commands(self):
        missing = self.candidate.replace(self.minimum, "# old min invocation removed", 1)
        dead = "\n      - name: Not executed\n        if: false\n        run: |\n          " + LIVE + "min\n"
        self.reject(missing + dead)
        self.reject(missing + "\n          " + self.minimum + "\n")

    def test_disabled_or_masked_named_step_is_rejected_despite_other_amd_marker(self):
        condition = "if: ${{ matrix.arch == 'amd64' }}"
        start = self.candidate.index("      - name: Exercise the Kubernetes and Traefik lifecycle")
        for replacement in ("if: false", "if: ${{ false }}", condition + "\n        continue-on-error: true",
                            condition + "\n        shell: echo {0}"):
            with self.subTest(replacement=replacement):
                altered = self.candidate[:start] + self.candidate[start:].replace(condition, replacement, 1)
                self.assertIn(condition, altered)  # unrelated candidate marker remains
                self.reject(altered)

    def test_wrong_shell_variable_quoting_or_extra_word_is_not_equivalent(self):
        for before, after in (
            ('--image "$image"', "--image '$image'"),
            (EVIDENCE.format("min"), "'$RUNNER_TEMP/helper-runtime-identity-min-$GITHUB_RUN_ID.json'"),
            ('image="plinth-runtime:amd64-$GITHUB_RUN_ID"', "'image=plinth-runtime:amd64-$GITHUB_RUN_ID'"),
            ("k3d_lifecycle_test.py", "k3d_lifecycle_test.py#not-a-shell-comment"),
        ):
            with self.subTest(before=before):
                self.reject(self.candidate.replace(before, after))

    def test_duplicate_or_wrong_named_step_is_not_accepted(self):
        self.reject(self.candidate.replace("Exercise the Kubernetes and Traefik lifecycle", "Unused lifecycle", 1))
        block = self.candidate[self.candidate.index("      - name: Exercise the Kubernetes and Traefik lifecycle"):]
        block = block[:block.index("      - name: Build the previous exact runtime revision")]
        self.reject(self.candidate + block)

    def test_real_deployment_check_rejects_changed_workflow_not_just_isolated_helper(self):
        changed = self.workflow.replace(EVIDENCE.format("max"), EVIDENCE.format("min"), 1)
        original = Path.read_text
        def read(path, *args, **kwargs):
            return changed if path == WORKFLOW else original(path, *args, **kwargs)
        with patch.object(Path, "read_text", read):
            errors = VERIFIER.check_kubernetes_deployment()
        self.assertTrue(any("exact helper evidence paths" in error for error in errors))


if __name__ == "__main__":
    unittest.main(verbosity=2)
