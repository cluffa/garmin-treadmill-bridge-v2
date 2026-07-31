#!/usr/bin/env python3
"""Validate mock_bridge.py's `uv run --script` metadata block.

Why this exists: the block is TOML wearing a `# ` prefix. uv strips that prefix
and parses what is left, so a line of prose in there is a *syntax error*, not a
comment — and the script then refuses to start with a TOML parse error that
points at the prose, several lines away from anything that looks wrong.

That happened once already. It survived a full review wave because every other
test in this directory imports a module or calls into the shared library; none
of them ever asked whether the script itself can launch. This one does the
cheap half of that (does the header parse, is the dependency pinned) without
needing Bluetooth or a watch.

Run: python3 test_script_header.py
"""
import pathlib
import re
import sys

try:
    import tomllib
except ModuleNotFoundError:  # host Python < 3.11
    tomllib = None

SCRIPT = pathlib.Path(__file__).with_name("mock_bridge.py")
BLOCK_RE = re.compile(r"^# /// script$(.*?)^# ///$", re.M | re.S)


def extract_block(text: str) -> str:
    """Return the TOML inside the `# /// script` block, prefix stripped."""
    m = BLOCK_RE.search(text)
    assert m, "mock_bridge.py has no `# /// script` metadata block"
    lines = []
    for raw in m.group(1).splitlines():
        if not raw:
            continue
        assert raw.startswith("#"), f"line in metadata block lacks a # prefix: {raw!r}"
        # uv strips "# " (or a bare "#" on an empty line).
        lines.append(raw[2:] if raw.startswith("# ") else raw[1:])
    return "\n".join(lines)


def main():
    text = SCRIPT.read_text()
    toml = extract_block(text)

    if tomllib is None:
        print("test_script_header: SKIP (needs Python 3.11+ for tomllib)")
        return 0

    try:
        meta = tomllib.loads(toml)
    except tomllib.TOMLDecodeError as e:
        print("test_script_header: FAIL — the uv metadata block is not valid TOML.")
        print(f"  {e}")
        print("  Prose inside the block is a syntax error, not a comment.")
        print("  Put explanations *below* the closing `# ///` line.")
        return 1

    deps = meta.get("dependencies")
    assert deps is not None, "metadata block declares no dependencies"

    bless = [d for d in deps if d.startswith("bless")]
    assert bless, f"bless is not among the declared dependencies: {deps}"
    # The advertising behaviour the design rests on is a bless implementation
    # detail, so an unpinned upgrade breaks the mock silently — the watch just
    # stops finding it. See the comment under the block in mock_bridge.py.
    assert "==" in bless[0], (
        f"bless must be pinned with '==', got {bless[0]!r}. An unpinned upgrade "
        "can drop the service UUID from the advert with no error anywhere."
    )

    assert "requires-python" in meta, "metadata block declares no requires-python"

    print("test_script_header: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
