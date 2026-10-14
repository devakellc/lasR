"""Execution plans inspect metadata without executing or mutating pipelines."""

import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

try:
    import pylasr
except ImportError:
    pylasr = None


@unittest.skipIf(pylasr is None, "pylasr not available")
class TestPipelinePlan(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="lasr-plan-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.fixture = Path(__file__).resolve().parents[2] / "inst/extdata/Example.las"
        self.assertTrue(self.fixture.is_file(), "bundled LAS fixture is required")
        self.input_file = str(self.fixture)

    @staticmethod
    def diagnostics(report, code=None, severity=None):
        return [item for item in report["diagnostics"]
                if (code is None or item["code"] == code)
                and (severity is None or item["severity"] == severity)]

    def assert_error(self, report, code):
        self.assertEqual(report["status"], "invalid")
        self.assertIs(report["valid"], False)
        self.assertTrue(self.diagnostics(report, code, "error"), report)

    def serialized(self, pipeline, filename):
        path = self.root / filename
        pipeline.write_json(str(path))
        configuration = json.loads(path.read_text())
        # JSON export inserts temporary catalog/reader stages with fresh UIDs.
        # Their configuration must stay unchanged, not their temporary IDs.
        for stage in configuration["pipeline"]:
            if stage["algoname"] in ("build_catalog", "reader"):
                stage.pop("uid", None)
        return configuration

    def test_report_contract_and_validation_alias(self):
        pipeline = pylasr.summarise(metrics=["z_max", "i_mean", "count"])
        pipeline.set_sequential_strategy()
        report = pipeline.plan(self.input_file)
        self.assertEqual(report["schema_version"], 1)
        self.assertEqual(report["status"], "valid")
        self.assertIs(report["valid"], True)
        self.assertEqual(report["scope"], "inputs")
        for key in ("inputs", "stages", "outputs", "chunks", "diagnostics"):
            self.assertIsInstance(report[key], list)
        self.assertEqual(len(report["chunks"]), 1)
        self.assertEqual(report["chunks"][0]["id"], 0)
        for key in ("requested", "effective"):
            self.assertTrue({"buffer", "chunk", "strategy", "ncores",
                             "concurrent_files", "concurrent_points"}
                            <= set(report["processing"][key]))
        self.assertEqual(report, pipeline.validate(self.input_file))
        json.dumps(report, allow_nan=False)

    def test_missing_inputs_are_incomplete(self):
        pipeline = pylasr.summarise(metrics=["z_max"])
        report = pipeline.plan()
        self.assertEqual(report["status"], "incomplete")
        self.assertIsNone(report["valid"])
        self.assertIsNone(report["chunks"])
        self.assertEqual(report["scope"], "pipeline")
        self.assertTrue(self.diagnostics(report, "missing_inputs", "deferred"))

    def test_pathlike_and_iterable_inputs_share_execution_conventions(self):
        pipeline = pylasr.summarise(metrics=["z_max"])
        expected = pipeline.plan(self.input_file)
        self.assertEqual(pipeline.plan(self.fixture), expected)
        self.assertEqual(pipeline.plan([self.fixture]), expected)
        self.assertEqual(pipeline.plan((path for path in [self.fixture])), expected)
        self.assertEqual(pipeline.validate((self.fixture,)), expected)

        class BytesPath:
            def __fspath__(self):
                return b"not-a-text-path.las"

        for invalid in ([123], [self.fixture, 123], BytesPath(), b"points.las"):
            with self.subTest(invalid=invalid):
                with self.assertRaises((TypeError, ValueError)):
                    pipeline.plan(invalid)

    def test_plans_do_not_change_configured_inputs_or_processing_options(self):
        pipeline = pylasr.summarise(metrics=["z_max"])
        pipeline.set_files([str(self.fixture)])
        pipeline.set_concurrent_files_strategy(2)
        pipeline.set_buffer(0)
        original_pipeline = pipeline.to_string()
        before = self.serialized(pipeline, "before.json")
        original = pipeline.plan()
        other = self.root / "other.las"
        shutil.copyfile(self.fixture, other)
        override = pipeline.plan([str(other)])
        self.assertNotEqual(original["inputs"], override["inputs"])
        self.assertEqual(original, pipeline.plan())
        self.assertEqual(before, self.serialized(pipeline, "after.json"))
        self.assertEqual(pipeline.to_string(), original_pipeline)

    def test_missing_metrics_filters_and_edit_attributes(self):
        pipelines = [pylasr.summarise(metrics=["HAG_mean"]),
                     pylasr.delete_points(["HAG > 0"]),
                     pylasr.edit_attribute(attribute="HAG", value=1)]
        for pipeline in pipelines:
            with self.subTest(pipeline=pipeline.to_string()):
                report = pipeline.validate(self.input_file)
                self.assert_error(report, "missing_attribute")
                self.assertTrue(any(item.get("attribute") == "HAG"
                                    for item in self.diagnostics(report, "missing_attribute")))

    def test_added_and_removed_attributes_follow_stage_order(self):
        add = pylasr.add_attribute("double", "HAG", "Height above ground")
        use = pylasr.edit_attribute(attribute="HAG", value=7) + pylasr.summarise(metrics=["HAG_mean"])
        self.assertIs((add + use).plan(self.input_file)["valid"], True)
        self.assert_error((use + add).plan(self.input_file), "missing_attribute")
        self.assert_error((add + pylasr.remove_attribute("HAG") + use).plan(self.input_file),
                          "missing_attribute")

    def test_buffer_adjustment_and_pcd_incompatibility(self):
        pipeline = pylasr.triangulate() + pylasr.write_las(str(self.root / "*.las"))
        pipeline.set_buffer(0)
        report = pipeline.plan(self.input_file)
        self.assertIs(report["valid"], True)
        self.assertEqual(report["processing"]["requested"]["buffer"], 0)
        self.assertEqual(report["processing"]["effective"]["buffer"], 20)
        self.assertEqual(report["chunks"][0]["buffer"], 20)
        invalid = pylasr.triangulate() + pylasr.write_pcd(str(self.root / "*.pcd"))
        self.assert_error(invalid.plan(self.input_file), "pcd_buffer")
        self.assertEqual(list(self.root.iterdir()), [])

    def test_writer_planning_does_not_create_or_truncate_files(self):
        existing = self.root / "existing.tif"
        existing.write_bytes(b"preserve this file")
        output = self.root / "result_*.las"
        pipeline = (pylasr.rasterize(1, 1, ["max"], ofile=str(existing))
                    + pylasr.write_las(str(output)))
        report = pipeline.plan(self.input_file)
        self.assertIs(report["valid"], True)
        self.assertTrue(self.diagnostics(report, "output_exists", "warning"))
        self.assertEqual(existing.read_bytes(), b"preserve this file")
        self.assertEqual(list(self.root.iterdir()), [existing])

    def test_callbacks_are_not_executed(self):
        called = []

        def callback(points):
            called.append(True)
            raise AssertionError("planning must not invoke callbacks")

        pipeline = pylasr.callback(callback, expose="xyz") + pylasr.summarise()
        report = pipeline.plan(self.input_file)
        self.assertEqual(called, [])
        self.assertEqual(report["status"], "incomplete")
        self.assertIsNone(report["valid"])
        self.assertTrue(self.diagnostics(report, severity="deferred"))

    def test_normalized_collisions_and_input_overwrites_are_errors(self):
        target = str(self.root / "shared.las")
        alias = os.path.join(str(self.root), ".", "shared.las")
        pipeline = pylasr.write_las(target) + pylasr.write_las(alias)
        self.assert_error(pipeline.plan(self.input_file), "output_collision")
        before = self.fixture.read_bytes()
        self.assert_error(pylasr.write_las(str(self.fixture)).plan(self.input_file),
                          "output_overwrites_input")
        self.assertEqual(before, self.fixture.read_bytes())
        self.assertFalse(Path(target).exists())

    def test_duplicate_tile_names_and_merged_output(self):
        inputs = []
        for name in ("a", "b"):
            directory = self.root / name
            directory.mkdir()
            path = directory / "tile.las"
            shutil.copyfile(self.fixture, path)
            inputs.append(str(path))
        self.assert_error(pylasr.write_las(str(self.root / "*.las")).plan(inputs),
                          "output_collision")
        merged = pylasr.write_las(str(self.root / "merged.las")).plan(inputs)
        self.assertIs(merged["valid"], True)
        self.assertEqual(len(merged["chunks"]), 2)

    def test_bad_configuration_returns_invalid_report(self):
        for value in (-1, float("nan"), float("inf")):
            with self.subTest(buffer=value):
                pipeline = pylasr.summarise()
                pipeline.set_buffer(value)
                report = pipeline.plan(self.input_file)
                self.assertEqual(report["status"], "invalid")
                self.assertIs(report["valid"], False)
                self.assertTrue(self.diagnostics(report, severity="error"))
            with self.subTest(chunk=value):
                with self.assertRaises(ValueError):
                    pylasr.summarise().set_chunk(value)
        self.assertEqual(pylasr.rasterize(0, 0, ["max"]).plan(self.input_file)["status"],
                         "invalid")
        missing = self.root / "missing" / "output.las"
        self.assertEqual(pylasr.write_las(str(missing)).plan(self.input_file)["status"], "invalid")
        self.assertFalse(missing.parent.exists())

    def test_remote_inputs_are_not_downloaded_or_certified(self):
        report = pylasr.summarise().plan("https://example.invalid/points.copc.laz")
        self.assertEqual(report["status"], "incomplete")
        self.assertIsNone(report["valid"])
        self.assertTrue(self.diagnostics(report, severity="deferred"))
        self.assertEqual(list(self.root.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
