"""Count vertex shader work for each oak material with one material visible."""
from BenchmarkOakLiveCuts import OUT, shot
from DecodeOakBoughVoxels import write_csv
from PackOakLiveCuts import Package


def main():
    rows = []
    for family in ("sprays", "native"):
        for slot, material in enumerate(("leaf", "twig", "trunk")):
            arm = family + material
            path = OUT / f"{arm}-crown.oakcut"
            package = Package()
            if family == "native":
                package.native("crown", materials=(slot,))
            elif slot < 2:
                package.sprays("crown", materials=(slot,))
                package.replace = 7
            else:
                # Suppress both spray slots and retain the stock trunk hierarchy.
                package.sprays("crown", materials=(0,))
                package.ranges = [[0] * 6 for _ in package.ranges]
            package.save(path)
            for distance in (41, 100):
                _, counters = shot("crown", arm, distance, 132, 99, extra=("--camera-pipeline-stats", "--live-cut-pack-assembly"))
                rows.append(dict(family=family,material=material,d=distance,**counters))
    write_csv(OUT / "material-counters.csv", rows)


if __name__ == "__main__":
    main()
