#!/usr/bin/env python3
"""A chunk must hold its own part of the area of interest, not the points of the other parts.

A tile shared by several parts of an area of interest holds all of their points, and the tile of an
EPT is as coarse as the dataset's own hierarchy: a 40 m plot can sit in a tile that covers kilometres.
Points of the other parts used to be kept in the chunk all the same, so a chunk of a zone of many
scattered plots carried points kilometres away. Every stage sized on the cloud extent (the CSF cloth,
the rasters) then grew with the spread of the zone instead of the size of the plot, which is what made
such a run eat gigabytes and die.
"""

import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from test_utils import write_las

try:
    import pylasr

    PYLASR_AVAILABLE = True
except ImportError as e:
    PYLASR_AVAILABLE = False
    IMPORT_ERROR = str(e)

# Two 20 m clusters 1 km apart, 200 points each, and a few points in the ring a windowed stage reaches
# into: RING sits outside the cluster and inside the 5 m window the buffer test asks for
LEFT, RIGHT = 400.0, 1400.0
Y, HALF, POINTS = 500.0, 10.0, 200
BOX = 15.0  # half the side of a part of the area of interest
RING, RING_POINTS = 17.0, 4


def cluster(centre_x, z):
    step = 2 * HALF / POINTS
    return [centre_x - HALF + i * step for i in range(POINTS)], [Y] * POINTS, [z] * POINTS


def ring(xmin, ymin, xmax, ymax):
    return [(xmin, ymin), (xmax, ymin), (xmax, ymax), (xmin, ymax), (xmin, ymin)]


def multipolygon(*boxes):
    parts = ["((" + ", ".join(f"{x} {y}" for x, y in ring(*b)) + "))" for b in boxes]
    return "MULTIPOLYGON(" + ", ".join(parts) + ")"


def write_ept(directory, las_path, npoints):
    """A one tile EPT: the whole cloud in the root node, the coarsest tile there can be.

    A real EPT is no different for a chunk: the tile it reads holds the points of every part of the
    area of interest that falls inside that tile.
    """
    os.makedirs(os.path.join(directory, "ept-data"), exist_ok=True)
    os.makedirs(os.path.join(directory, "ept-hierarchy"), exist_ok=True)
    shutil.copy(las_path, os.path.join(directory, "ept-data", "0-0-0-0.laz"))

    with open(os.path.join(directory, "ept-hierarchy", "0-0-0-0.json"), "w") as fh:
        json.dump({"0-0-0-0": npoints}, fh)

    with open(os.path.join(directory, "ept.json"), "w") as fh:
        json.dump(
            {
                "dataType": "laszip",
                "points": npoints,
                "bounds": [LEFT - 15, Y - 15, 0, RIGHT + 15, Y + 15, 2 * (RIGHT - LEFT)],
                "boundsConforming": [LEFT - HALF, Y - HALF, 100, RIGHT + HALF, Y + HALF, 110],
                "span": 128,
                "srs": {"authority": "EPSG", "horizontal": "3857"},
            },
            fh,
        )
    return os.path.join(directory, "ept.json")


class TestAOIPartsOwnTheirChunks(unittest.TestCase):
    def setUp(self):
        if not PYLASR_AVAILABLE:
            self.skipTest("pylasr not available")

        self.tmp = tempfile.mkdtemp()
        self.las = os.path.join(self.tmp, "two_clusters.las")

        left, right = cluster(LEFT, 100.0), cluster(RIGHT, 110.0)
        xs, ys, zs = left[0] + right[0], left[1] + right[1], left[2] + right[2]

        for centre in (LEFT, RIGHT):
            for side in (-1, 1):
                xs += [centre + side * RING] * RING_POINTS
                ys += [Y] * RING_POINTS
                zs += [100.0] * RING_POINTS

        self.npoints = len(xs)
        write_las(self.las, xs, ys, zs)

        self.ept = write_ept(os.path.join(self.tmp, "ept"), self.las, self.npoints)
        self.aoi = multipolygon(
            (LEFT - BOX, Y - BOX, LEFT + BOX, Y + BOX),
            (RIGHT - BOX, Y - BOX, RIGHT + BOX, Y + BOX),
        )

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def chunks(self, pipeline, source):
        """Run *pipeline* and return one (xs, ys) pair per chunk it processed."""
        seen = []

        def collect(data):
            seen.append((data["X"], data["Y"]))
            return None

        pipeline += pylasr.callback(collect)
        pipeline += pylasr.summarise()
        result = pipeline.execute([source])
        self.assertTrue(result["success"], "Pipeline execution failed")
        return seen

    def assert_own_part_only(self, seen, tolerance=1.0):
        self.assertEqual(len(seen), 2, "one chunk per part of the area of interest")
        for xs, ys in seen:
            spread = max(xs.max() - xs.min(), ys.max() - ys.min())
            self.assertLess(
                spread,
                2 * HALF + tolerance,
                f"chunk holds points over {spread:.0f} m: the other part's points leaked into it",
            )
            centre = LEFT if abs(xs.mean() - LEFT) < abs(xs.mean() - RIGHT) else RIGHT
            self.assertLess(abs(xs.mean() - centre), HALF)

    def test_an_ept_tile_does_not_hand_the_other_parts_to_a_chunk(self):
        """The regression: one coarse tile, two parts, each chunk must keep its own cluster"""
        self.assert_own_part_only(self.chunks(pylasr.reader_polygons(aoi=self.aoi), self.ept))

    def test_a_las_file_does_not_hand_the_other_parts_to_a_chunk(self):
        self.assert_own_part_only(self.chunks(pylasr.reader_polygons(aoi=self.aoi), self.las))

    def test_a_buffer_ring_is_still_kept(self):
        """A stage with a window keeps the ring around its part that it asks for, and nothing further"""
        window = 5
        seen = self.chunks(
            pylasr.reader_polygons(aoi=self.aoi) + pylasr.rasterize(res=1, window=window),
            self.ept,
        )
        self.assertEqual(len(seen), 2, "one chunk per part of the area of interest")
        for xs, _ in seen:
            centre = LEFT if abs(xs.mean() - LEFT) < abs(xs.mean() - RIGHT) else RIGHT
            reach = max(abs(xs.min() - centre), abs(xs.max() - centre))
            self.assertGreater(reach, BOX, "the ring the window asks for is missing")
            self.assertLessEqual(reach, BOX + window, "the chunk reaches past the ring")


if __name__ == "__main__":
    unittest.main()
