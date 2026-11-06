from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "smoothaf_d1.h"
DOC = ROOT / "docs" / "B2-D1.md"


def main():
    text = HEADER.read_text(encoding="utf-8")
    doc = DOC.read_text(encoding="utf-8")
    required_header = (
        "SMOOTHAF_D1_MAX_SAMPLES 30u",
        "SMOOTHAF_D1_OWNER_D1",
        "SMOOTHAF_D1_REJECT_STOP_PANIC",
        "smoothaf_d1_gate_acquire",
        "smoothaf_d1_series_start",
        "smoothaf_d1_series_record",
        "smoothaf_d1_series_finish",
    )
    for token in required_header:
        assert token in text, token
    assert "lens_focus(" not in text
    assert "card I/O" in text
    for token in (
        "Exclusão operacional",
        "Identidade da série",
        "Ring e FIO",
        "Protocolo experimental mínimo",
        "Gate de aprovação",
        "DO NOT RUN PHYSICAL COLLECTION BEFORE APPROVAL",
    ):
        assert token in doc, token
    print("SmoothAF B2-D1 contract passed")


if __name__ == "__main__":
    main()
