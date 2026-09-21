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


class TestTransformCrsVertical(unittest.TestCase):
    """Regression tests for transform_crs Z handling"""

    def setUp(self):
        if not PYLASR_AVAILABLE:
            self.skipTest("pylasr not available")

        self.megaplot = None
        megaplot_paths = [
            "../inst/extdata/Megaplot.las",
            "../../inst/extdata/Megaplot.las",
            "../../../inst/extdata/Megaplot.las",
        ]
        for path in megaplot_paths:
            full_path = os.path.join(os.path.dirname(__file__), path)
            if os.path.exists(full_path):
                self.megaplot = full_path
                break
        if self.megaplot is None:
            self.skipTest("Megaplot.las not found")

        self.temp_dir = tempfile.mkdtemp()

    def tearDown(self):
        if hasattr(self, "temp_dir") and os.path.exists(self.temp_dir):
            shutil.rmtree(self.temp_dir)

    def test_wkt_shorthand_target_is_stored_as_canonical_wkt(self):
        # set_crs()/transform_crs() accept GDAL shorthand such as "EPSG:26917+5703" via
        # SetFromUserInput(). Storing that literal string as the CRS's own WKT (instead of
        # exporting the parsed spatial reference back to WKT) corrupts write_las(): the literal
        # shorthand ends up in the WKT VLR, which strict WKT readers reject.
        baked = os.path.join(self.temp_dir, "baked.las")
        p0 = pylasr.Pipeline() + pylasr.set_crs("EPSG:26917+5703") + pylasr.write_las(baked)
        pylasr.execute(p0, self.megaplot)

        pipeline = pylasr.Pipeline() + pylasr.transform_crs("EPSG:32617+5703") + pylasr.summarise()
        result = pylasr.execute(pipeline, baked)
        wkt = result["data"][0]["summary"]["crs"]["wkt"]

        self.assertNotEqual(wkt.strip(), "EPSG:32617+5703")
        self.assertIn("COMPOUNDCRS", wkt)

    def test_two_ellipsoidal_crs_reproject_without_being_treated_as_equal(self):
        # vertical_id() used to give every uncoded ellipsoidal (3-axis) CRS the same id, -1, so
        # two different ones (e.g. WGS84 and NAD83(2011)) compared equal and the stage kept the
        # source Z untouched while still relabelling the output with the target's vertical CRS.
        baked = os.path.join(self.temp_dir, "baked_4979.las")
        p0 = pylasr.Pipeline() + pylasr.set_crs(4979) + pylasr.write_las(baked)
        pylasr.execute(p0, self.megaplot)

        out = os.path.join(self.temp_dir, "out_6319.las")
        pipeline = pylasr.Pipeline() + pylasr.transform_crs(6319) + pylasr.write_las(out)
        pylasr.execute(pipeline, baked)

        summary_pipeline = pylasr.Pipeline() + pylasr.summarise()
        result = pylasr.execute(summary_pipeline, out)
        self.assertEqual(result["data"][0]["summary"]["crs"]["epsg"], 6319)


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


TOPOGRAPHY = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))), "inst", "extdata", "Topography.las")


def _summary(result):
    return next(e["summary"] for e in result["data"] if "summary" in e)


class TestEquivalentCrs(unittest.TestCase):
    def setUp(self):
        if not PYLASR_AVAILABLE:
            self.skipTest("pylasr not available")

        self.temp_dir = tempfile.mkdtemp()

    def tearDown(self):
        if hasattr(self, "temp_dir") and os.path.exists(self.temp_dir):
            shutil.rmtree(self.temp_dir)

    def test_same_crs_written_in_another_wkt(self):
        probe = pylasr.reader_rectangles([0.0], [0.0], [1.0], [1.0]) + pylasr.summarise()
        wkt = _summary(pylasr.execute(probe, TOPOGRAPHY))["crs"]["wkt"]
        # renaming the axes changes the text, not the CRS
        other = wkt.replace('"easting (E(X))"', '"X"').replace('"northing (N(Y))"', '"Y"')
        self.assertNotEqual(other, wkt)

        copy = os.path.join(self.temp_dir, "copy.las")
        pylasr.execute(pylasr.reader_coverage() + pylasr.set_crs(other) + pylasr.write_las(copy), TOPOGRAPHY)

        query = pylasr.reader_rectangles([273360.0], [5274360.0], [273490.0], [5274490.0])
        one = _summary(pylasr.execute(query + pylasr.summarise(), TOPOGRAPHY))["npoints"]
        both = _summary(pylasr.execute(query + pylasr.summarise(), [TOPOGRAPHY, copy]))["npoints"]
        self.assertEqual(both, 2 * one)


class TestChunkCrsPropagation(unittest.TestCase):
    def setUp(self):
        if not PYLASR_AVAILABLE:
            self.skipTest("pylasr not available")

        self.temp_dir = tempfile.mkdtemp()

    def tearDown(self):
        if hasattr(self, "temp_dir") and os.path.exists(self.temp_dir):
            shutil.rmtree(self.temp_dir)

    def _npoints_near_native_extent(self, path):
        # A hair inside Topography.las's native (EPSG:2949) extent, so a source CRS lost to
        # a stale chunk.crs (points left in degrees, or shifted to another projection) misses it
        query = pylasr.reader_rectangles([273357.2], [5274357.2], [273642.8], [5274642.8]) + pylasr.summarise()
        return _summary(pylasr.execute(query, path))["npoints"]

    def test_chained_transform_crs_returns_to_the_source_crs(self):
        out = os.path.join(self.temp_dir, "roundtrip.las")
        pipeline = pylasr.transform_crs(4326) + pylasr.transform_crs(2949) + pylasr.write_las(out)
        pylasr.execute(pipeline, TOPOGRAPHY)

        baseline = self._npoints_near_native_extent(TOPOGRAPHY)
        roundtrip = self._npoints_near_native_extent(out)
        self.assertGreater(roundtrip, 0.9 * baseline)

    def test_set_crs_then_transform_crs_to_the_same_crs_is_identity(self):
        # Declares a CRS other than the file's own, so chunk.crs (the file's native CRS) and
        # the declared one disagree; source_crs must follow the declaration, not the file
        other_epsg = 32619
        out = os.path.join(self.temp_dir, "identity.las")
        pipeline = pylasr.set_crs(other_epsg) + pylasr.transform_crs(other_epsg) + pylasr.write_las(out)
        pylasr.execute(pipeline, TOPOGRAPHY)

        baseline = self._npoints_near_native_extent(TOPOGRAPHY)
        after = self._npoints_near_native_extent(out)
        self.assertEqual(after, baseline)


class TestQueryAcrossCollectionCrs(unittest.TestCase):
    def setUp(self):
        if not PYLASR_AVAILABLE:
            self.skipTest("pylasr not available")

        self.temp_dir = tempfile.mkdtemp()

        xmid = (273357.14 + 273642.86) / 2
        self.left = os.path.join(self.temp_dir, "left.las")
        self.right = os.path.join(self.temp_dir, "right_4326.las")

        pylasr.execute(
            pylasr.reader_rectangles([273357.14], [5274357.14], [xmid], [5274642.85]) + pylasr.write_las(self.left),
            TOPOGRAPHY,
        )
        pylasr.execute(
            pylasr.reader_rectangles([xmid], [5274357.14], [273642.86], [5274642.85])
            + pylasr.transform_crs(4326)
            + pylasr.write_las(self.right),
            TOPOGRAPHY,
        )

    def tearDown(self):
        if hasattr(self, "temp_dir") and os.path.exists(self.temp_dir):
            shutil.rmtree(self.temp_dir)

    def test_query_matching_only_a_differently_projected_file_is_refused(self):
        # The spatial index is in the collection's (left file's) CRS. A query landing only in
        # the right, EPSG:4326 tile must not be read with catalog-CRS bounds against it
        query = pylasr.reader_rectangles([273550.0], [5274400.0], [273642.86], [5274600.0]) + pylasr.summarise()
        with self.assertRaises(Exception):
            pylasr.execute(query, [self.left, self.right])

    def test_query_matching_only_the_collection_crs_file_still_works(self):
        query = pylasr.reader_rectangles([273357.14], [5274357.14], [273450.0], [5274500.0]) + pylasr.summarise()
        result = _summary(pylasr.execute(query, [self.left, self.right]))
        self.assertGreater(result["npoints"], 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
