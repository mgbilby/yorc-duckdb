#!/usr/bin/env python3
"""Keep test/sql/orcid_tasks.test in step with examples/tasks/*.sql.

sqllogictest cannot `.read` a file, so the test carries each bundle's
statements verbatim between `# BEGIN <file>` and `# END <file>` markers, each
statement under `statement ok`. This script rewrites those blocks from the
bundle files (default) or, with --check, exits 1 if any block differs.
Statements are the bundle's blank-line-separated chunks; chunks made only of
`--` comment lines are dropped. Standard library only.

    python3 scripts/sync_tasks_test.py           # rewrite the blocks
    python3 scripts/sync_tasks_test.py --check   # verify (release checklist)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TEST = ROOT / "test/sql/orcid_tasks.test"


def block(bundle: Path) -> str:
    chunks = re.split(r"\n\s*\n", bundle.read_text().strip())
    out = []
    for chunk in chunks:
        lines = chunk.splitlines()
        if all(l.lstrip().startswith("--") for l in lines):
            continue
        # leading comment lines stay with the statement (DuckDB ignores them)
        out.append("statement ok\n" + chunk.strip() + "\n")
    rel = bundle.relative_to(ROOT).as_posix()
    return f"# BEGIN {rel}\n\n" + "\n".join(out) + f"\n# END {rel}"


def main() -> int:
    text = TEST.read_text()
    new = text
    for m in re.finditer(r"# BEGIN (\S+)\n.*?# END \1", text, flags=re.S):
        new = new.replace(m.group(0), block(ROOT / m.group(1)))
    if "--check" in sys.argv:
        if new != text:
            print(f"{TEST.relative_to(ROOT)} is out of date: run python3 scripts/sync_tasks_test.py", file=sys.stderr)
            return 1
        print("orcid_tasks.test: in step with examples/tasks")
        return 0
    TEST.write_text(new)
    return 0


if __name__ == "__main__":
    sys.exit(main())
