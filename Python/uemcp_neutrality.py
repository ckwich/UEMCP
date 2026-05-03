"""Project-neutrality audit for UEMCP tracked files."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Sequence


DEFAULT_CONFIG_PATH = Path(__file__).resolve().parent.parent / "Scripts" / "UEMCPNeutrality.Audit.json"
TEXT_SUFFIXES = {
    ".cpp",
    ".cs",
    ".h",
    ".ini",
    ".json",
    ".md",
    ".py",
    ".ps1",
    ".sh",
    ".txt",
    ".uplugin",
    ".uproject",
    ".xml",
    ".yml",
    ".yaml",
}


@dataclass(frozen=True)
class NeutralityFinding:
    path: str
    line: int
    pattern: str
    text: str


def load_config(path: Path = DEFAULT_CONFIG_PATH) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as config_file:
        return json.load(config_file)


def _repo_path(repo_root: Path, path: Path) -> str:
    return path.resolve().relative_to(repo_root.resolve()).as_posix()


def _rule_matches(rule: dict[str, Any], *, path: str, pattern: str) -> bool:
    exact_path = rule.get("path")
    path_prefix = rule.get("path_prefix")
    if exact_path and path != str(exact_path).replace("\\", "/"):
        return False
    if path_prefix and not path.startswith(str(path_prefix).replace("\\", "/")):
        return False

    allowed_patterns = rule.get("patterns")
    if allowed_patterns and pattern not in {str(item) for item in allowed_patterns}:
        return False
    return True


def is_allowed(config: dict[str, Any], *, path: str, pattern: str) -> bool:
    return any(
        _rule_matches(rule, path=path, pattern=pattern)
        for rule in config.get("allowed_occurrences") or []
        if isinstance(rule, dict)
    )


def scan_paths(
    repo_root: Path,
    paths: Iterable[Path],
    config: dict[str, Any],
) -> list[NeutralityFinding]:
    patterns = sorted(
        [str(pattern) for pattern in config.get("patterns") or []],
        key=len,
        reverse=True,
    )
    findings: list[NeutralityFinding] = []
    for path in paths:
        if path.suffix.lower() not in TEXT_SUFFIXES:
            continue
        repo_path = _repo_path(repo_root, path)
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except UnicodeDecodeError:
            continue
        for line_number, line in enumerate(lines, 1):
            matched_patterns: list[str] = []
            for pattern in patterns:
                if any(pattern in matched_pattern for matched_pattern in matched_patterns):
                    continue
                if pattern not in line:
                    continue
                matched_patterns.append(pattern)
                if not is_allowed(config, path=repo_path, pattern=pattern):
                    findings.append(
                        NeutralityFinding(
                            path=repo_path,
                            line=line_number,
                            pattern=pattern,
                            text=line.strip(),
                        )
                    )
    return findings


def tracked_files(repo_root: Path) -> list[Path]:
    completed = subprocess.run(
        ["git", "ls-files"],
        cwd=repo_root,
        check=True,
        capture_output=True,
        text=True,
    )
    return [
        repo_root / line.strip()
        for line in completed.stdout.splitlines()
        if line.strip()
    ]


def audit_repo(repo_root: Path, config: dict[str, Any]) -> list[NeutralityFinding]:
    return scan_paths(repo_root, tracked_files(repo_root), config)


def _format_findings(findings: Sequence[NeutralityFinding]) -> str:
    lines = ["Project-specific strings found outside neutrality allowlist:"]
    for finding in findings:
        lines.append(
            f"{finding.path}:{finding.line}: {finding.pattern!r}: {finding.text}"
        )
    return "\n".join(lines)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        default=str(Path(__file__).resolve().parent.parent),
        help="Repository root to scan. Defaults to the UEMCP checkout.",
    )
    parser.add_argument(
        "--config",
        default=str(DEFAULT_CONFIG_PATH),
        help="Neutrality audit config JSON.",
    )
    args = parser.parse_args(argv)

    repo_root = Path(args.repo_root).resolve()
    config = load_config(Path(args.config))
    findings = audit_repo(repo_root, config)
    if findings:
        print(_format_findings(findings), file=sys.stderr)
        return 1

    print("NEUTRALITY_OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
