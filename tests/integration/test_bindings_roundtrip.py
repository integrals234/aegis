"""AEGIS-229 — the compiled bindings must round-trip against the Python peer.

The acceptance criterion is "round-trip integration tests pass". Two things
make that meaningful rather than decorative:

**The compiled module must actually be the compiled module.** A test that
silently falls back to a pure-Python shim would pass while proving nothing about
the C++ build, which is the single most common way a bindings test becomes
theatre. ``test_the_module_under_test_is_compiled`` checks the marker the
extension sets.

**The comparison is against the other implementation, not a stored file.** The
golden fixture already pins both encoders to committed bytes; here the C++
encoder is run live and compared to the Python encoder, so a divergence shows up
as a disagreement between the two rather than as two tests that were both
updated together.

If the extension is missing, these tests fail rather than skip. A skipped test
is not evidence, and AEGIS-003 forbids treating one as such.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest
from common.clock import EventTime
from common.envelope import ENVELOPE_SCHEMA_VERSION, Envelope, MessageType, decode, encode

pytestmark = pytest.mark.integration

ROOT = Path(__file__).resolve().parents[2]
BUILD_HINT = (
    "The compiled extension was not found. Build it with:\n"
    "    cmake --preset debug && cmake --build --preset debug\n"
    "This test fails rather than skips: a skipped test is not evidence (AEGIS-003)."
)


# Preference order matters: a sanitizer-instrumented module cannot be loaded by
# a plain interpreter (it needs the ASan runtime preloaded), so an alphabetical
# glob that finds build/asan-ubsan first would fail for a reason unrelated to
# the bindings.
PRESET_PREFERENCE = ("debug", "release")


def _load_bindings():
    candidates = [ROOT / f"build/{preset}/cpp/bindings" for preset in PRESET_PREFERENCE]
    candidates += [p for p in sorted(ROOT.glob("build/*/cpp/bindings")) if p not in candidates]
    for directory in candidates:
        if any(directory.glob("aegis_bindings*.so")):
            sys.path.insert(0, str(directory))
            break
    try:
        import aegis_bindings
    except ImportError as exc:  # pragma: no cover - exercised when the build is absent
        pytest.fail(f"{BUILD_HINT}\n\nUnderlying import error: {exc}")
    return aegis_bindings


@pytest.fixture(scope="module")
def bindings():
    """Load the extension per module, not at import time.

    pytest imports every test module during collection, *before* deselecting
    by marker. Loading at module scope therefore turned one missing artifact
    into a collection error in all five layers at once — unit, integration,
    property, replay and research each reporting a single error that named
    none of them. The failure is unchanged in force (still a failure, never a
    skip, per AEGIS-003); it is now attributed to the layer that actually
    depends on the extension.
    """
    return _load_bindings()


def test_the_module_under_test_is_compiled(bindings):
    """A pure-Python fallback would make every test below meaningless."""
    assert getattr(bindings, "__compiled__", False) is True
    assert bindings.__file__.endswith(".so"), bindings.__file__


def test_build_info_is_a_real_computation_not_a_constant(bindings):
    """The value must describe this binary, not a string somebody typed."""
    info = bindings.build_info()
    for expected in ("compiler", "std c++", "build", "assertions", "sanitizers"):
        assert expected in info, info
    assert bindings.version() in info


def test_version_is_dotted(bindings):
    assert bindings.version().count(".") >= 1


def test_both_implementations_agree_on_the_wire_version(bindings):
    assert bindings.envelope_schema_version() == ENVELOPE_SCHEMA_VERSION


CASES = [
    pytest.param(Envelope(), id="empty"),
    pytest.param(Envelope(sequence=42, stream_id=7, payload=b"\xde\xad\xbe\xef"), id="typical"),
    pytest.param(Envelope(event_time=EventTime(-1)), id="pre-epoch"),
    pytest.param(
        Envelope(
            sequence=2**64 - 1,
            stream_id=2**64 - 1,
            event_time=EventTime(2**63 - 1),
            producer_id="exchange.sequencer",
            experiment_id="m0-bindings",
            correlation_id="order-4711",
            payload=bytes(range(256)),
        ),
        id="boundaries",
    ),
    pytest.param(Envelope(producer_id="prodüzent", experiment_id="実験"), id="unicode"),
]


@pytest.mark.parametrize("envelope", CASES)
def test_cpp_encoder_matches_the_python_encoder(bindings, envelope):
    """One wire format, two implementations, checked against each other live."""
    from_cpp = bindings.encode_envelope(
        sequence=envelope.sequence,
        stream_id=envelope.stream_id,
        event_time_ns=envelope.event_time.nanos,
        producer_id=envelope.producer_id,
        experiment_id=envelope.experiment_id,
        correlation_id=envelope.correlation_id,
        payload=envelope.payload,
    )
    assert from_cpp == encode(envelope)


@pytest.mark.parametrize("envelope", CASES)
def test_python_encoded_bytes_decode_in_cpp(bindings, envelope):
    decoded = bindings.decode_envelope(encode(envelope))
    assert decoded["sequence"] == envelope.sequence
    assert decoded["stream_id"] == envelope.stream_id
    assert decoded["event_time_ns"] == envelope.event_time.nanos
    assert decoded["producer_id"] == envelope.producer_id
    assert decoded["experiment_id"] == envelope.experiment_id
    assert decoded["correlation_id"] == envelope.correlation_id
    assert decoded["payload"] == envelope.payload
    assert decoded["message_type"] == int(MessageType.UNSPECIFIED)


@pytest.mark.parametrize("envelope", CASES)
def test_cpp_encoded_bytes_decode_in_python(bindings, envelope):
    from_cpp = bindings.encode_envelope(
        sequence=envelope.sequence,
        stream_id=envelope.stream_id,
        event_time_ns=envelope.event_time.nanos,
        producer_id=envelope.producer_id,
        experiment_id=envelope.experiment_id,
        correlation_id=envelope.correlation_id,
        payload=envelope.payload,
    )
    assert decode(from_cpp) == envelope


def test_cpp_decoder_reports_why_a_message_was_rejected(bindings):
    """A feed handler must be able to count which kind of malformed message it saw."""
    with pytest.raises(ValueError, match="beyond its declared payload"):
        bindings.decode_envelope(encode(Envelope()) + b"\x00")

    with pytest.raises(ValueError, match="schema version"):
        bad = bytearray(encode(Envelope()))
        bad[0] = 9
        bindings.decode_envelope(bytes(bad))

    with pytest.raises(ValueError, match="ends before"):
        bindings.decode_envelope(b"\x01\x00")


def test_bindings_expose_no_mutable_engine_state(bindings):
    """ADR-0005: no binding may mutate engine internals or run on the hot path.

    Enforced structurally at M0 by there being nothing else to expose — the
    surface is four pure functions — and asserted here so an addition that
    breaks the policy has to break a test first.
    """
    public = {name for name in dir(bindings) if not name.startswith("_")}
    assert public == {
        "version",
        "build_info",
        "envelope_schema_version",
        "encode_envelope",
        "decode_envelope",
    }
