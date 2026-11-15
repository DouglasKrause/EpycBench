#!/usr/bin/env python3
"""Stage immutable audit reports and pending human-review templates; never approve."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys


def sha256(raw):
    return hashlib.sha256(raw).hexdigest()


def read_report(path):
    raw = path.read_bytes()
    if not raw or len(raw) > 8 * 1024 * 1024:
        raise ValueError(f"empty or oversized report: {path}")
    return raw


def stage(source_root, gcc_build, clang_build, output):
    schemas = source_root / "schemas/kernel-object-qualification-approval-v1.schema.json"
    schema = json.loads(schemas.read_text())
    staged = []
    source_commit = None
    for family in schema["oneOf"]:
        properties = family["properties"]
        issue = properties["issue"]["const"]
        implementation = properties["implementation_id"]["const"]
        policy = properties["policy_version"]["const"]
        items = properties["approval"]["properties"]["semantic_items"]["prefixItems"]
        item_ids = [item["properties"]["id"]["const"] for item in items]
        artifacts = []
        files = []
        for label, compiler, preset, build in (
                ("gcc", "GCC", "release", gcc_build),
                ("clang", "Clang", "release-clang", clang_build)):
            base = build / "qualification" / issue
            machine = read_report(base / "object-code-audit.json")
            readable = read_report(base / "object-code-audit.txt")
            report = json.loads(machine)
            if (report["schema"] != "epycbench.kernel-object-audit-report/v1"
                    or report["framework_version"] != "epycbench.kernel-object-audit/v1"
                    or report["family_policy_version"] != policy
                    or report["implementation_id"] != implementation
                    or report["qualification"]["preset"] != preset
                    or report["qualification"]["configuration"] != "Release"
                    or report["qualification"]["policy_identifier"] != policy
                    or report["compiler"]["family"] != compiler
                    or report["source"]["dirty"] is not False
                    or report["automated_disposition"] != "pass"
                    or report["qualification_status"] != "pending_human_review"
                    or report["human_review"]["status"] != "pending"
                    or report["human_review"]["required"] is not True
                    or [item["id"] for item in report["human_review"]["items"]] != item_ids):
                raise ValueError(f"expected clean automated-pass/pending {compiler} {issue}: {base}")
            commit = report["source"]["commit"]
            if not isinstance(commit, str) or not re.fullmatch(r"[0-9a-f]{40}", commit):
                raise ValueError(f"invalid source commit: {base}")
            if source_commit is not None and source_commit != commit:
                raise ValueError("all four reports must identify the same clean source commit")
            source_commit = commit
            for identity in (report["object"]["sha256"], report["function"]["sha256"]):
                if not isinstance(identity, str) or not re.fullmatch(r"[0-9a-f]{64}", identity):
                    raise ValueError(f"invalid object/function digest: {base}")
            artifacts.append({
                "automated_disposition": "pass", "compiler_family": compiler,
                "qualification_preset": preset, "generated_human_review_status": "pending",
                "object_sha256": report["object"]["sha256"],
                "function_sha256": report["function"]["sha256"],
                "machine_report": {"path": label + "/object-code-audit.json", "sha256": sha256(machine)},
                "human_report": {"path": label + "/object-code-audit.txt", "sha256": sha256(readable)},
            })
            files.extend(((label + "/object-code-audit.json", machine),
                          (label + "/object-code-audit.txt", readable)))
        manifest = {
            "schema": "epycbench.kernel-object-qualification-approval/v1",
            "framework_version": "epycbench.kernel-object-audit/v1",
            "policy_version": policy, "report_schema": "epycbench.kernel-object-audit-report/v1",
            "implementation_id": implementation, "issue": issue,
            "source_commit": source_commit, "title": issue + " own-build semantic review",
            "approval": {
                "actor": "REVIEW_REQUIRED", "recorded_at_utc": "REVIEW_REQUIRED",
                "disposition": "REVIEW_REQUIRED", "statement": "REVIEW_REQUIRED",
                "semantic_items": [{"id": item, "disposition": "REVIEW_REQUIRED"} for item in item_ids],
            },
            "artifacts": artifacts,
        }
        files.append(("approval.template.json", (json.dumps(manifest, indent=2) + "\n").encode()))
        staged.append((issue, files))
    # Validate all inputs before creating anything. An existing destination is refused.
    output.mkdir(mode=0o700, parents=False, exist_ok=False)
    for issue, files in staged:
        family = output / issue
        family.mkdir(mode=0o700)
        for label in ("gcc", "clang"):
            (family / label).mkdir(mode=0o700)
        for name, raw in files:
            fd = os.open(family / name, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, "wb") as stream:
                stream.write(raw)
    return source_commit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--gcc-build", type=Path, required=True)
    parser.add_argument("--clang-build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True,
                        help="fresh directory under an existing parent")
    args = parser.parse_args()
    os.umask(0o077)
    try:
        commit = stage(args.source_root, args.gcc_build, args.clang_build, args.output)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"prepare-kernel-evidence: {error}", file=sys.stderr)
        return 1
    print(f"Staged pending templates for source {commit} in {args.output}")
    print("No approval was issued. REVIEW_REQUIRED templates cannot pass production admission.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
