#!/usr/bin/env python3
"""
Integration tests for pylasr using actual data processing workflows
"""

import os
import shutil
import sys
import tempfile
import unittest

# Add the parent directory to sys.path to import pylasr
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from test_utils import read_points, read_raster_cells, write_las

try:
    import pylasr

    PYLASR_AVAILABLE = True
except ImportError as e:
    PYLASR_AVAILABLE = False
    IMPORT_ERROR = str(e)


class TestIntegrationWorkflows(unittest.TestCase):
    """Test complete processing workflows"""

    def setUp(self):
        if not PYLASR_AVAILABLE:
            self.skipTest("pylasr not available")

        # Find example LAS file
        self.example_las = None
        example_paths = [
            "../inst/extdata/Example.las",
            "../../inst/extdata/Example.las",
            "../../../inst/extdata/Example.las",
        ]

        for path in example_paths:
            full_path = os.path.join(os.path.dirname(__file__), path)
            if os.path.exists(full_path):
                self.example_las = full_path
                break

        # Create temporary directory for outputs
        self.temp_dir = tempfile.mkdtemp()

    def tearDown(self):
        """Clean up temporary files"""
        if hasattr(self, "temp_dir") and os.path.exists(self.temp_dir):
            shutil.rmtree(self.temp_dir)

    def test_basic_info_pipeline(self):
        """Test basic info pipeline without actual data"""
        pipeline = pylasr.Pipeline()
        pipeline += pylasr.info()

        # Test JSON export
        json_file = os.path.join(self.temp_dir, "info_pipeline.json")
        result_json = pipeline.write_json(json_file)
        self.assertEqual(result_json, json_file)
        self.assertTrue(os.path.exists(json_file))

        # Test pipeline info
        info = pylasr.pipeline_info(json_file)
        self.assertIsNotNone(info)

    def test_complex_pipeline_creation(self):
        """Test creating complex pipeline without execution"""
        pipeline = pylasr.Pipeline()

        # Add various stages
        pipeline += pylasr.info()
        pipeline += pylasr.classify_with_sor(k=8, m=6)
        pipeline += pylasr.delete_points(["Classification == 18"])
        pipeline += pylasr.classify_with_csf()

        # Add output stage
        output_file = os.path.join(self.temp_dir, "processed.las")
        pipeline += pylasr.write_las(output_file)

        # Test pipeline string representation
        pipeline_str = pipeline.to_string()
        self.assertIn("info", pipeline_str)
        self.assertIn("classify_with_sor", pipeline_str)
        self.assertIn(
            "filter", pipeline_str
        )  # delete_points becomes filter in pipeline string
        self.assertIn("classify_with_csf", pipeline_str)
        self.assertIn("write_las", pipeline_str)

        # Test JSON export
        json_file = os.path.join(self.temp_dir, "complex_pipeline.json")
        pipeline.write_json(json_file)
        self.assertTrue(os.path.exists(json_file))

    def test_dtm_dsm_workflow(self):
        """Test DTM/DSM workflow creation"""
        dtm_file = os.path.join(self.temp_dir, "dtm.tif")
        dsm_file = os.path.join(self.temp_dir, "dsm.tif")

        # Create DTM and DSM pipelines
        dtm_pipeline = pylasr.dtm(1.0, dtm_file)
        dsm_pipeline = pylasr.dsm(0.5, dsm_file)

        # Combine pipelines
        full_pipeline = dtm_pipeline + dsm_pipeline

        # Test pipeline structure
        pipeline_str = full_pipeline.to_string()
        self.assertIn("rasterize", pipeline_str)

        # Test JSON export
        json_file = os.path.join(self.temp_dir, "dtm_dsm_pipeline.json")
        full_pipeline.write_json(json_file)
        self.assertTrue(os.path.exists(json_file))

    def test_sampling_workflow(self):
        """Test point sampling workflow"""
        pipeline = pylasr.Pipeline()

        # Add different sampling methods
        pipeline += pylasr.sampling_voxel(res=2.0, method="random")
        pipeline += pylasr.sampling_pixel(res=1.0, method="max")
        pipeline += pylasr.sampling_poisson(distance=1.5)

        # Add output
        output_file = os.path.join(self.temp_dir, "sampled.las")
        pipeline += pylasr.write_las(output_file)

        # Test pipeline creation
        json_file = os.path.join(self.temp_dir, "sampling_pipeline.json")
        pipeline.write_json(json_file)
        self.assertTrue(os.path.exists(json_file))

    def test_attribute_management_workflow(self):
        """Test attribute management workflow"""
        pipeline = pylasr.Pipeline()

        # Add attribute operations
        pipeline += pylasr.add_attribute("double", "Roughness", "Surface roughness")
        pipeline += pylasr.add_rgb()
        pipeline += pylasr.geometry_features(k=10, r=1.0, features="eigen_values")
        pipeline += pylasr.edit_attribute(["Classification == 1"], "Classification", 6)
        pipeline += pylasr.remove_attribute("GPSTime")

        # Add output
        output_file = os.path.join(self.temp_dir, "with_attributes.las")
        pipeline += pylasr.write_las(output_file)

        # Test pipeline creation
        json_file = os.path.join(self.temp_dir, "attributes_pipeline.json")
        pipeline.write_json(json_file)
        self.assertTrue(os.path.exists(json_file))

    def test_format_conversion_workflow(self):
        """Test format conversion workflow"""
        pipeline = pylasr.Pipeline()

        # Add multiple output formats
        las_file = os.path.join(self.temp_dir, "output.laz")
        pcd_file = os.path.join(self.temp_dir, "output.pcd")
        copc_file = os.path.join(self.temp_dir, "output.copc.laz")
        vpc_file = os.path.join(self.temp_dir, "catalog.vpc")

        pipeline += pylasr.write_las(las_file)
        pipeline += pylasr.write_pcd(pcd_file, binary=True)
        # pipeline += pylasr.write_copc(copc_file, max_depth=10)  # Skip due to validation issue
        pipeline += pylasr.write_vpc(vpc_file)
        pipeline += pylasr.write_lax()

        # Test pipeline creation
        json_file = os.path.join(self.temp_dir, "conversion_pipeline.json")
        pipeline.write_json(json_file)
        self.assertTrue(os.path.exists(json_file))

    def test_pipeline_processing_strategies(self):
        """Test different processing strategies"""
        pipeline = pylasr.Pipeline()
        pipeline += pylasr.info()

        # Test different strategies
        pipeline.set_sequential_strategy()
        pipeline.set_concurrent_points_strategy(4)
        pipeline.set_concurrent_files_strategy(2)
        pipeline.set_nested_strategy(2, 4)

        # Test other options
        pipeline.set_verbose(True)
        pipeline.set_progress(True)
        pipeline.set_buffer(10.0)
        pipeline.set_chunk(1000.0)

        # Test JSON export with options
        json_file = os.path.join(self.temp_dir, "strategy_pipeline.json")
        pipeline.write_json(json_file)
        self.assertTrue(os.path.exists(json_file))

    def test_actual_data_processing(self):
        """Test processing with actual LAS data if available"""
        if not self.example_las:
            self.skipTest("Example LAS file not found")

        # Create simple pipeline
        pipeline = pylasr.Pipeline()
        pipeline += pylasr.info()

        output_file = os.path.join(self.temp_dir, "processed_example.las")
        pipeline += pylasr.write_las(output_file)

        # Set processing options
        pipeline.set_sequential_strategy()
        pipeline.set_verbose(False)  # Keep output clean for tests

        # Test execution (this will actually process data)
        try:
            result = pipeline.execute([self.example_las])
            
            # Validate new result structure
            self.assertIsInstance(result, dict, "Result must be a dictionary")
            self.assertIn('success', result, "Result must have 'success' field")  
            self.assertIn('data', result, "Result must have 'data' field")
            self.assertIn('json_config', result, "Result must have 'json_config' field")
            
            self.assertTrue(result['success'], "Pipeline execution failed")
            self.assertTrue(os.path.exists(output_file), "Output file was not created")
            
            # Validate data structure
            if result['data']:
                self.assertIsInstance(result['data'], list, "Data field must be a list")
        except Exception as e:
            self.fail(f"Pipeline execution raised an exception: {e}")


class TestErrorHandling(unittest.TestCase):
    """Test error handling and edge cases"""

    def setUp(self):
        if not PYLASR_AVAILABLE:
            self.skipTest("pylasr not available")

        self.temp_dir = tempfile.mkdtemp()

    def tearDown(self):
        if hasattr(self, "temp_dir") and os.path.exists(self.temp_dir):
            shutil.rmtree(self.temp_dir)

    def test_invalid_stage_parameters(self):
        """Test handling of invalid stage parameters"""
        # Test with invalid parameter types
        with self.assertRaises((TypeError, ValueError)):
            # Try to create a pipeline with invalid parameters
            pylasr.classify_with_sor(k="invalid", m="invalid")


def _cone_las(path, apex_x, apex_y, apex_h):
    """A single-apex conical canopy, wide enough to exercise a real max_cr and chunk size"""
    x, y, z = [], [], []
    for xi in range(int(apex_x) - 75, int(apex_x) + 76):
        for yi in range(int(apex_y) - 75, int(apex_y) + 76):
            d = ((xi - apex_x) ** 2 + (yi - apex_y) ** 2) ** 0.5
            x.append(float(xi))
            y.append(float(yi))
            z.append(max(0.5, apex_h - d))
    write_las(path, x, y, z)


class TestRandomWalker(unittest.TestCase):
    """Regression tests for random_walker's buffer size and crown radius bugs"""

    APEX = (135.5, 125.5, 30.0)

    def setUp(self):
        if not PYLASR_AVAILABLE:
            self.skipTest("pylasr not available")

        self.temp_dir = tempfile.mkdtemp()
        self.las = os.path.join(self.temp_dir, "cone.las")
        _cone_las(self.las, *self.APEX)

    def tearDown(self):
        if hasattr(self, "temp_dir") and os.path.exists(self.temp_dir):
            shutil.rmtree(self.temp_dir)

    def _run(self, max_cr, chunk=None):
        tif = os.path.join(self.temp_dir, f"out_{chunk}.tif")
        chm = pylasr.rasterize(1.0, 1.0, ["max"])
        seed = pylasr.local_maximum_raster(chm, 5, min_height=2.0)
        tree = pylasr.random_walker(chm, seed, max_cr=max_cr, ofile=tif)
        pipeline = chm + seed + tree
        if chunk:
            pipeline.set_chunk(chunk)
        result = pylasr.execute(pipeline, [self.las])
        self.assertTrue(result["success"], result.get("message"))
        return read_raster_cells(tif)

    def test_buffer_derives_from_max_cr(self):
        """A chunk boundary must not lose cells a seed just beyond it should still own"""
        unchunked = self._run(max_cr=40.0)
        chunked = self._run(max_cr=40.0, chunk=50.0)
        self.assertEqual(len(chunked), len(unchunked))

    def test_crown_radius_enforced(self):
        """No labeled cell may lie beyond max_cr/2 of the seed that claims it"""
        max_cr = 10.0
        apex_x, apex_y, _ = self.APEX
        cells = self._run(max_cr=max_cr)

        self.assertGreater(len(cells), 0)
        for x, y, _ in cells:
            dist = ((x - apex_x) ** 2 + (y - apex_y) ** 2) ** 0.5
            # +1 cell for the seed-to-cell-center snap, not the sqrt(2) a square window allowed
            self.assertLessEqual(dist, max_cr / 2 + 1.0)

    def test_max_cr_must_be_positive(self):
        """A non-positive max_cr must be rejected, not walk the solver off the raster"""
        chm = pylasr.rasterize(1.0, 1.0, ["max"])
        seed = pylasr.local_maximum_raster(chm, 5, min_height=2.0)
        tree = pylasr.random_walker(chm, seed, max_cr=-5.0)
        pipeline = chm + seed + tree

        with self.assertRaises(ValueError):
            pylasr.execute(pipeline, [self.las])


class TestMultichm(unittest.TestCase):
    """Regression tests for the multichm buffer size and tie-handling bugs"""

    def setUp(self):
        if not PYLASR_AVAILABLE:
            self.skipTest("pylasr not available")

        self.temp_dir = tempfile.mkdtemp()

    def tearDown(self):
        if hasattr(self, "temp_dir") and os.path.exists(self.temp_dir):
            shutil.rmtree(self.temp_dir)

    def test_buffer_covers_dist_2d(self):
        """A candidate near a chunk boundary must still see a taller tree suppressing it
        across dist_2d, even though dist_2d exceeds both ws and dist_3d"""
        las = os.path.join(self.temp_dir, "buffer.las")
        write_las(
            las,
            [106.5, 112.5, 100.0, 120.0],
            [105.5, 105.5, 100.0, 110.0],
            [20.0, 10.0, 0.0, 0.0],
        )

        def run(chunk=None):
            ofile = os.path.join(self.temp_dir, f"out_{chunk}.gpkg")
            pipeline = pylasr.multichm(
                res=1.0, ws=1.0, min_height=2.0, dist_2d=8.0, dist_3d=1.0, ofile=ofile
            )
            if chunk:
                pipeline.set_chunk(chunk)
            result = pylasr.execute(pipeline, [las])
            self.assertTrue(result["success"], result.get("message"))
            return read_points(ofile)

        self.assertEqual(len(run()), 1)
        self.assertEqual(len(run(chunk=10.0)), 1)

    def test_tie_handling_matches_reference(self):
        """On a flat run of equal-height cells, ties must resolve the way
        lidRplugins::multichm() does and not collapse to a single survivor"""
        las = os.path.join(self.temp_dir, "tie.las")
        x = [100.5 + i for i in range(13)] + [99.0, 115.0]
        y = [100.5] * 13 + [99.0, 102.0]
        z = [10.0] * 13 + [0.0, 0.0]
        write_las(las, x, y, z)

        ofile = os.path.join(self.temp_dir, "out_tie.gpkg")
        pipeline = pylasr.multichm(res=1.0, ws=3.0, ofile=ofile)
        result = pylasr.execute(pipeline, [las])
        self.assertTrue(result["success"], result.get("message"))

        points = sorted(read_points(ofile))
        self.assertEqual([round(p[0], 1) for p in points], [100.5, 106.5, 112.5])


if __name__ == "__main__":
    unittest.main(verbosity=2)
