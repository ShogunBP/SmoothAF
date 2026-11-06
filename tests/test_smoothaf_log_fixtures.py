from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests" / "fixtures" / "SMTH001.fixture"

EXPECTED = {
    2: (1, 1, 13, 8, 3, 6351, 6, 6138, 6, None),
    5: (1, 1, 10, 7, 3, 6682, 3, 6147, 3, None),
    6: (1, 1, 12, 8, 3, 6565, 0, 6100, 0, None),
    7: (1, 1, 12, 8, 8, 6628, 0, 6628, 0, 2),
}


def main():
    records = {}
    for raw in FIXTURE.read_text(encoding="utf-8").splitlines():
        if not raw or raw.startswith("#"):
            continue
        fields = raw.split()
        assert fields[0] == "EPISODE"
        episode = int(fields[1])
        values = tuple(map(int, fields[2:11]))
        limit = int(fields[12]) if len(fields) > 11 else None
        records[episode] = values + (limit,)

    assert records == EXPECTED
    assert set(records) == {2, 5, 6, 7}
    print("SmoothAF log fixture semantics passed")


if __name__ == "__main__":
    main()
