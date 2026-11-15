#!/usr/bin/env python3
"""Compare both linked Kernel functions with compiler-matched reviewed reports."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys


TOOL_ROOT = Path(__file__).resolve().parent
if TOOL_ROOT.name == "public-source":
    TOOL_ROOT = TOOL_ROOT.parent
sys.path.insert(0, str(TOOL_ROOT / "kernel-object-audit"))
from kernel_object_audit import bandwidth_policy, latency_policy
from kernel_object_audit.framework import (
    AuditError, CommandResult, SYMBOL_RE, checked_run, demangle_symbols,
    extract_function_bytes, parse_sections, parse_symbols, select_symbol,
    validate_report, validate_target,
)


class BoundedRunner:
    def run(self, arguments, cwd=None):
        try:
            completed = subprocess.run(list(arguments), cwd=cwd, capture_output=True,
                                       text=True, check=False, timeout=30)
        except subprocess.TimeoutExpired as error:
            raise AuditError("metadata tool exceeded its 30-second timeout") from error
        return CommandResult(completed.returncode, completed.stdout, completed.stderr)


def sha256(raw):
    return hashlib.sha256(raw).hexdigest()


def identity(metadata):
    return tuple(getattr(metadata, name) for name in
                 ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns"))


def executable_snapshot(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(descriptor, "rb") as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode):
            raise AuditError("consumer is not a regular file")
        digest = hashlib.sha256()
        while raw := stream.read(65536):
            digest.update(raw)
        if identity(before) != identity(os.fstat(stream.fileno())):
            raise AuditError("consumer changed during hashing")
    return digest.hexdigest(), identity(before)


def read_report(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(descriptor, "rb") as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= 8 * 1024 * 1024:
            raise AuditError("report must be a nonempty regular file within 8MiB")
        raw = stream.read(8 * 1024 * 1024 + 1)
        if len(raw) != before.st_size or identity(before) != identity(os.fstat(stream.fileno())):
            raise AuditError("report changed during reading")
    report = json.loads(raw)
    if not isinstance(report, dict):
        raise AuditError("report must be an object")
    validate_report(report)
    return raw, report


def verify(executable, compiler_family, bandwidth_report, latency_report, runner=None):
    runner = runner or BoundedRunner()
    before = executable_snapshot(executable)
    version_raw = checked_run(runner, [str(executable.resolve()), "version", "--json"])
    version = json.loads(version_raw)
    preset = {"GCC": "release", "Clang": "release-clang"}[compiler_family]
    compiler_id = {"GCC": "GNU", "Clang": "Clang"}[compiler_family]
    commit = version["git"]["revision"]
    if (version["compiler"]["id"] != compiler_id or version["preset"] != preset
            or version["git"]["dirty"] is not False
            or not isinstance(commit, str) or not re.fullmatch(r"[0-9a-f]{40}", commit)):
        raise AuditError("consumer compiler/preset/clean source identity is incompatible")
    compiler_version = version["compiler"]["version"]
    if not isinstance(compiler_version, str) or not compiler_version:
        raise AuditError("consumer compiler version is unavailable")
    if not isinstance(version["build_identity"], str) or not re.fullmatch(
            r"[0-9a-f]{64}", version["build_identity"]):
        raise AuditError("consumer Build identity is invalid")
    reports = []
    for path, policy in ((bandwidth_report, bandwidth_policy), (latency_report, latency_policy)):
        raw, report = read_report(path)
        if (report["framework_version"] != "epycbench.kernel-object-audit/v1"
                or report["family_policy_version"] != policy.POLICY_IDENTIFIER
                or report["implementation_id"] != policy.IMPLEMENTATION_ID
                or report["qualification"]["preset"] != preset
                or report["qualification"]["configuration"] != "Release"
                or report["qualification"]["policy_identifier"] != policy.POLICY_IDENTIFIER
                or report["compiler"]["family"] != compiler_family
                or report["source"]["commit"] != commit or report["source"]["dirty"] is not False
                or report["symbol"]["demangled"] != policy.SEMANTIC_SYMBOL
                or report["automated_disposition"] != "pass"
                or report["qualification_status"] != "pending_human_review"
                or report["human_review"]["status"] != "pending"
                or report["human_review"]["required"] is not True
                or [item["id"] for item in report["human_review"]["items"]]
                   != [item for item, _ in policy.HUMAN_REVIEW]):
            raise AuditError("report compiler/source/family/preset/pending-audit binding is incompatible")
        validate_target(report["compiler"]["target"])
        if not isinstance(report["compiler"]["version"], str) or not report["compiler"]["version"]:
            raise AuditError("report compiler version is unavailable")
        for digest in (report["object"]["sha256"], report["function"]["sha256"]):
            if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
                raise AuditError("report object/function digest is invalid")
        reports.append((path, raw, report, policy))

    sections = parse_sections(checked_run(runner, ["readelf", "--sections", "--wide", str(executable)]))
    symbol_text = checked_run(runner, ["readelf", "--symbols", "--wide", str(executable)])
    # Demangle only candidates containing the two stable semantic method names;
    # parse every symbol so the framework still checks range overlap globally.
    names = []
    for line in symbol_text.splitlines():
        match = SYMBOL_RE.match(line)
        if match and any(method in match[6] for method in
                         ("execute_sequential_read_bandwidth", "execute_dependent_load_latency")):
            names.append(match[6].split("@", 1)[0])
    symbols = parse_symbols(symbol_text, demangle_symbols(runner, names, "c++filt"))
    matches = []
    for path, raw, report, policy in reports:
        symbol, section = select_symbol(symbols, policy.SEMANTIC_SYMBOL, sections)
        digest = sha256(extract_function_bytes(executable, symbol, section))
        if digest != report["function"]["sha256"]:
            raise AuditError("linked function bytes differ from the compiler-matched report")
        matches.append({"implementation_id": policy.IMPLEMENTATION_ID,
                        "semantic_symbol": policy.SEMANTIC_SYMBOL,
                        "symbol": symbol.mangled, "linked_function_bytes": symbol.size,
                        "function_sha256": digest, "report_sha256": sha256(raw),
                        "report_compiler_version": report["compiler"]["version"],
                        "report_compiler_target": report["compiler"]["target"],
                        "reported_object_sha256": report["object"]["sha256"]})
        if read_report(path)[0] != raw:
            raise AuditError("report changed during retention verification")
    if executable_snapshot(executable) != before:
        raise AuditError("consumer changed during retention verification")
    return {"schema": "epycbench.kernel-consumer-retention/v1", "disposition": "pass",
            "executable_sha256": before[0], "executable_bytes": before[1][2],
            "version_json_sha256": sha256(version_raw.encode()),
            "build_identity": version["build_identity"], "source_commit": commit,
            "compiler_family": compiler_family, "compiler_version": compiler_version,
            "qualification_preset": preset, "functions": matches,
            "scope": "Exact linked-function retention only; original reports require separate genuine semantic approval.",
            "semantic_approval_issued": False, "hardware_qualification_issued": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--compiler-family", choices=("GCC", "Clang"), required=True)
    parser.add_argument("--bandwidth-report", type=Path, required=True)
    parser.add_argument("--latency-report", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="new private retention receipt")
    args = parser.parse_args()
    try:
        if args.output.exists() or args.output.is_symlink():
            raise AuditError("existing output is refused")
        result = verify(args.executable, args.compiler_family, args.bandwidth_report, args.latency_report)
        descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(descriptor, "w") as stream:
            json.dump(result, stream, indent=2, sort_keys=True)
            stream.write("\n")
    except (AuditError, OSError, ValueError, KeyError, TypeError) as error:
        print(f"verify-kernel-retention: {error}", file=sys.stderr)
        return 1
    print("Both linked Kernel functions exactly match their compiler-matched reports.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
