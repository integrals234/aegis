#!/usr/bin/env python3
"""Run a producer more than once and require byte-identical canonical output.

AEGIS-005: identical event input and seed must produce byte-identical canonical
output before performance work begins. What this tool establishes at M0 is
narrower, and the milestone report says so in the same words: **the harness
detects nondeterminism**. There is no engine yet whose determinism could be
claimed.

Two design choices carry the weight:

**Separate processes per run.** Running twice in one process shares module
state, interned strings and a single PYTHONHASHSEED, which hides exactly the
class of nondeterminism that later bites — output that depends on dict ordering,
address-derived hashes or process-wide caches. Each run is spawned with a
different PYTHONHASHSEED so a hash-order dependency fails here rather than in
CI six months from now.

**A negative fixture.** ``--producer nondeterministic`` must fail. A harness
that has only ever seen stable output cannot show it would notice unstable
output, and the milestone would be claiming a property it never tested.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Per-producer evidence claim text (ADR-0012): the M0 platform wording
# ("not a claim that AEGIS is deterministic") is accurate for a stub producer
# with no engine behind it, and would be false for the exchange producer,
# whose whole point is an engine-determinism claim. Keeping the text
# producer-specific is what keeps M0's narrow wording from silently
# traveling onto M1 evidence it does not describe.
CLAIMS: dict[str, str] = {
    "platform": (
        "This run shows that the harness compares canonical output across "
        "processes. It is not a claim that AEGIS is deterministic: at M0 there "
        "is no engine, and the producer emits platform records only."
    ),
    "nondeterministic": (
        "This run shows that the harness compares canonical output across "
        "processes. It is not a claim that AEGIS is deterministic: at M0 there "
        "is no engine, and the producer emits platform records only."
    ),
    "exchange": (
        "This run shows that aegis_exchange_replay produces byte-identical "
        "canonical event output across independent processes with different "
        "PYTHONHASHSEED, for the committed M1 scenario. This is the AEGIS-005 "
        "discharge for the exchange core: identical event input produces "
        "byte-identical canonical output (ADR-0012)."
    ),
    "futures_replay": (
        "This run shows that aegis_replay_run produces byte-identical canonical "
        "output across independent processes with different PYTHONHASHSEED, for "
        "the committed canonical futures stream under accelerated pacing with "
        "two committed fault rules. This is the M2 replay core's AEGIS-058 "
        "evidence -- repeated runs produce identical outputs -- covering "
        "canonical ordering, record_index stability, pacing arithmetic and "
        "deterministic fault application together (ADR-0018, ADR-0019). It is "
        "not a performance claim."
    ),
}


def claim_for(producer: str) -> str:
    return CLAIMS.get(
        producer,
        f"This run shows that the harness compares canonical output across processes "
        f"for producer {producer!r}. No specific claim text is registered for it in "
        "tools/determinism_check.py's CLAIMS mapping.",
    )


RUNNER = """
import sys
sys.path.insert(0, {python_dir!r})
from pathlib import Path
from common.determinism import run_producer
sys.stdout.write(run_producer({name!r}, {seed!r}, Path({root!r})))
"""


def run_once(root: Path, producer: str, seed: int, hash_seed: int) -> str:
    """Execute one producer run in a fresh interpreter and return its output."""
    script = RUNNER.format(python_dir=str(root / "python"), name=producer, seed=seed, root=str(root))
    result = subprocess.run(
        [sys.executable, "-c", script],
        capture_output=True,
        text=True,
        check=False,
        # A different hash seed per run: if canonical output ever depends on
        # dict iteration order, this is where it shows up.
        env={"PYTHONHASHSEED": str(hash_seed), "PATH": "/usr/bin:/bin"},
        cwd=root,
    )
    if result.returncode != 0:
        raise SystemExit(f"producer {producer!r} failed:\n{result.stderr}")
    return result.stdout


def digest(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--producer", default="platform")
    parser.add_argument("--runs", type=int, default=2)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--expect-failure", action="store_true",
                        help="invert the result: for the negative fixture, instability is success")
    parser.add_argument("--write-evidence", type=Path,
                        help="directory to write per-run hashes and the canonical output into")
    args = parser.parse_args(argv)

    if args.runs < 2:
        print("ERROR: --runs must be at least 2; one run cannot disagree with itself",
              file=sys.stderr)
        return 2

    root = args.root.resolve()
    outputs = [run_once(root, args.producer, args.seed, 1 + index) for index in range(args.runs)]
    digests = [digest(output) for output in outputs]
    stable = len(set(digests)) == 1

    for index, value in enumerate(digests, start=1):
        print(f"run {index}: sha256 {value}  ({len(outputs[index - 1])} bytes)")

    if args.write_evidence:
        args.write_evidence.mkdir(parents=True, exist_ok=True)
        for index, value in enumerate(digests, start=1):
            (args.write_evidence / f"run{index}.hash").write_text(
                f"{value}  {args.producer}(seed={args.seed})\n", encoding="utf-8"
            )
        (args.write_evidence / "canonical_output.txt").write_text(outputs[0], encoding="utf-8")
        (args.write_evidence / "summary.json").write_text(
            json.dumps(
                {
                    "producer": args.producer,
                    "seed": args.seed,
                    "runs": args.runs,
                    "digests": digests,
                    "stable": stable,
                    "claim": claim_for(args.producer),
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        print(f"wrote evidence to {args.write_evidence}")

    if args.expect_failure:
        if stable:
            print(
                f"ERROR: producer {args.producer!r} was expected to be unstable but every run "
                "agreed. The negative fixture is no longer proving that the harness can detect "
                "nondeterminism.",
                file=sys.stderr,
            )
            return 2
        print(f"Negative fixture behaved as required: {args.producer!r} is unstable across runs")
        return 0

    if not stable:
        print(
            f"ERROR: producer {args.producer!r} produced different canonical output across "
            f"{args.runs} runs at seed {args.seed}",
            file=sys.stderr,
        )
        return 2

    print(f"Determinism check passed: {args.runs} runs of {args.producer!r} agree byte for byte")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
