"""File isolation must be shared by the Python and JSON execution paths."""

import json
import shutil
import tempfile
import unittest
from pathlib import Path

import pylasr


class TestIndependentFiles(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        source = Path(__file__).resolve().parents[2] / "inst/extdata/Example.las"
        files = [self.root / "survey_2023.las", self.root / "survey_2024.las"]
        for file in files:
            shutil.copyfile(source, file)
        self.files = [str(file) for file in files]

    def stage_result(self, result, name):
        self.assertTrue(result["success"])
        return next(item[name] for item in result["data"] if name in item)

    def test_summary_and_writers(self):
        for parallel in [False, True]:
            with self.subTest(parallel=parallel):
                pipeline = pylasr.summarise() + pylasr.write_las(str(self.root / "*_copy.laz"))
                if parallel and pylasr.has_omp_support():
                    pipeline.set_concurrent_files_strategy(2)
                else:
                    pipeline.set_sequential_strategy()
                pipeline.set_independent_files(True)
                result = pipeline.execute(self.files)
                self.assertEqual(self.stage_result(result, "summary")["npoints"], 60)
                files = self.stage_result(result, "write_las")
                self.assertEqual(sorted(Path(file).name for file in files), [
                    "survey_2023_copy.laz", "survey_2024_copy.laz"
                ])
                for file in files:
                    result = pylasr.summarise().execute(file)
                    self.assertEqual(self.stage_result(result, "summary")["npoints"], 30)

        # Turning the option off restores the existing spatial-collection behavior.
        default = pylasr.summarise()
        default.set_independent_files(False)
        result = default.execute(self.files)
        self.assertEqual(self.stage_result(result, "summary")["npoints"], 120)

    def test_json_option(self):
        pipeline = pylasr.summarise()
        pipeline.set_files(self.files)
        pipeline.set_independent_files(True)
        path = self.root / "pipeline.json"
        pipeline.write_json(str(path))
        config = json.loads(path.read_text())
        self.assertTrue(config["processing"]["independent_files"])
        self.assertTrue(config["pipeline"][0]["independent_files"])

        # The public JSON option works without a duplicate catalog field.
        del config["pipeline"][0]["independent_files"]
        path.write_text(json.dumps(config))
        result = pylasr.execute(str(path))
        self.assertEqual(self.stage_result(result, "summary")["npoints"], 60)

        # Direct catalog configurations are also accepted.
        del config["processing"]["independent_files"]
        config["pipeline"][0]["independent_files"] = True
        path.write_text(json.dumps(config))
        result = pylasr.execute(str(path))
        self.assertEqual(self.stage_result(result, "summary")["npoints"], 60)

    def test_reject_json_conflicts(self):
        for option in ["buffer", "chunk"]:
            for scope in ["processing", "catalog"]:
                with self.subTest(option=option, scope=scope):
                    pipeline = pylasr.summarise()
                    pipeline.set_files(self.files)
                    pipeline.set_independent_files(True)
                    path = self.root / "invalid.json"
                    pipeline.write_json(str(path))
                    config = json.loads(path.read_text())
                    target = config["processing"] if scope == "processing" else config["pipeline"][0]
                    target[option] = 0.5
                    path.write_text(json.dumps(config))
                    with self.assertRaisesRegex(
                        ValueError, "independent_files requires buffer = 0 and chunk = 0"
                    ):
                        pylasr.execute(str(path))

    def test_reject_python_conflicts(self):
        for option in ["buffer", "chunk"]:
            with self.subTest(option=option):
                pipeline = pylasr.summarise()
                pipeline.set_independent_files(True)
                getattr(pipeline, "set_" + option)(1)
                with self.assertRaisesRegex(
                    ValueError, "independent_files requires buffer = 0 and chunk = 0"
                ):
                    pipeline.execute(self.files)

    def test_reject_spatial_queries(self):
        pipeline = pylasr.reader_rectangles([0], [0], [1], [1]) + pylasr.summarise()
        pipeline.set_independent_files(True)
        with self.assertRaisesRegex(
            ValueError, "independent_files cannot be combined with spatial-query readers"
        ):
            pipeline.execute(self.files)


if __name__ == "__main__":
    unittest.main()
