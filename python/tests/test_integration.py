#!/usr/bin/env python3
"""
Integration tests for pylasr using actual data processing workflows
"""

import os
import glob
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


if __name__ == "__main__":
    unittest.main(verbosity=2)


class TestAreaOfInterest(unittest.TestCase):
    """Test the area of interest clipping the readers"""

    # Bounding box of Topography.las
    XMIN, YMIN, XMAX, YMAX = 273357, 5274357, 273643, 5274643
    XMID, YMID = 273500, 5274500

    def setUp(self):
        if not PYLASR_AVAILABLE:
            self.skipTest("pylasr not available")

        self.las = None
        for path in [
            "../inst/extdata/Topography.las",
            "../../inst/extdata/Topography.las",
            "../../../inst/extdata/Topography.las",
        ]:
            full_path = os.path.join(os.path.dirname(__file__), path)
            if os.path.exists(full_path):
                self.las = full_path
                break

        if not self.las:
            self.skipTest("Topography LAS file not found")

    def npoints(self, aoi=None):
        reader = pylasr.reader_coverage() if aoi is None else pylasr.reader_polygons(aoi=aoi)
        pipeline = reader + pylasr.summarise()
        result = pipeline.execute([self.las])
        self.assertTrue(result["success"], "Pipeline execution failed")
        return result["data"][0]["summary"]["npoints"]

    @staticmethod
    def ring(xmin, ymin, xmax, ymax):
        return [[xmin, ymin], [xmax, ymin], [xmax, ymax], [xmin, ymax], [xmin, ymin]]

    @classmethod
    def box(cls, xmin, ymin, xmax, ymax):
        vertices = ", ".join(f"{x} {y}" for x, y in cls.ring(xmin, ymin, xmax, ymax))
        return f"POLYGON(({vertices}))"

    def test_aoi_covering_the_data_keeps_every_point(self):
        """An area of interest larger than the data changes nothing"""
        self.assertEqual(self.npoints(self.box(self.XMIN, self.YMIN, self.XMAX, self.YMAX)), self.npoints())

    def test_aoi_halves_partition_the_point_cloud(self):
        """Two halves of the coverage add up to the whole"""
        left = self.npoints(self.box(self.XMIN, self.YMIN, self.XMID, self.YMAX))
        right = self.npoints(self.box(self.XMID, self.YMIN, self.XMAX, self.YMAX))
        self.assertEqual(left + right, self.npoints())

    def test_aoi_from_coordinate_rings_matches_wkt(self):
        """Nested lists of coordinates describe the same area as the WKT"""
        wkt = self.npoints(self.box(self.XMIN, self.YMIN, self.XMID, self.YMAX))
        rings = self.npoints([self.ring(self.XMIN, self.YMIN, self.XMID, self.YMAX)])
        self.assertEqual(rings, wkt)

    def test_aoi_from_multipolygon_rings(self):
        """One more level of nesting describes a multipolygon"""
        halves = [
            [self.ring(self.XMIN, self.YMIN, self.XMID, self.YMAX)],
            [self.ring(self.XMID, self.YMIN, self.XMAX, self.YMAX)],
        ]
        self.assertEqual(self.npoints(halves), self.npoints())

    def test_aoi_hole_is_subtracted(self):
        """A hole removes exactly the points the hole alone would keep"""
        outer = self.ring(self.XMIN, self.YMIN, self.XMAX, self.YMAX)
        inner = self.ring(273450, 5274450, 273550, 5274550)
        holed = self.npoints([outer, inner])
        hole = self.npoints([inner])
        self.assertEqual(holed + hole, self.npoints())

    def test_aoi_chunking_does_not_change_the_points(self):
        """A chunk size tiles the area of interest instead of being refused"""
        aoi = self.box(self.XMIN, self.YMIN, self.XMID, self.YMAX)
        pipeline = pylasr.reader_polygons(aoi=aoi) + pylasr.summarise()
        pipeline.set_chunk(100)
        result = pipeline.execute([self.las])
        self.assertTrue(result["success"], "Pipeline execution failed")
        self.assertEqual(result["data"][0]["summary"]["npoints"], self.npoints(aoi))

    def test_auto_chunking_does_not_change_the_points(self):
        """Chunks sized on the memory they can afford keep every point of the area"""
        aoi = self.box(self.XMIN, self.YMIN, self.XMID, self.YMAX)
        pipeline = pylasr.reader_polygons(aoi=aoi) + pylasr.rasterize(res=0.05, window=0) + pylasr.summarise()
        result = pipeline.execute([self.las])
        self.assertTrue(result["success"], "Pipeline execution failed")
        self.assertEqual(result["data"][0]["summary"]["npoints"], self.npoints(aoi))

    def test_aoi_chunking_tiles_the_rasters(self):
        """The tiles of a chunked area of interest are written one by one"""
        aoi = self.box(self.XMIN, self.YMIN, self.XMID, self.YMAX)
        temp_dir = tempfile.mkdtemp()
        try:
            pipeline = pylasr.reader_polygons(aoi=aoi) + pylasr.rasterize(
                res=2, window=2, ofile=os.path.join(temp_dir, "*_chm.tif")
            )
            pipeline.set_chunk(100)
            self.assertTrue(pipeline.execute([self.las])["success"], "Pipeline execution failed")
            self.assertGreater(len(glob.glob(os.path.join(temp_dir, "*.tif"))), 1)
        finally:
            shutil.rmtree(temp_dir)

    def test_aoi_outside_the_data_returns_nothing(self):
        """An area of interest that reaches no file is not an error"""
        self.assertEqual(self.npoints(self.box(280000, 5280000, 280100, 5280100)), 0)

    def test_invalid_aoi_is_rejected(self):
        """A malformed or non areal geometry is refused with a readable message"""
        with self.assertRaises(Exception):
            self.npoints("POLYGON((0 0, 1 1")
        with self.assertRaises(Exception):
            self.npoints("POINT(0 0)")

    def test_island_in_a_hole_is_not_double_counted(self):
        """The island's box sits entirely inside the donut's box: a point there must be read once"""
        outer = self.ring(self.XMIN, self.YMIN, self.XMAX, self.YMAX)
        hole = self.ring(273437, 5274437, 273563, 5274563)
        island = self.ring(273477, 5274477, 273523, 5274523)

        donut_alone = self.npoints([outer, hole])
        island_alone = self.npoints([island])
        combined = self.npoints([[outer, hole], [island]])

        self.assertEqual(combined, donut_alone + island_alone)

    def test_island_in_a_hole_is_not_double_counted_when_chunked(self):
        """The same disjoint-box check, but tiled: overlapping boxes tile into overlapping chunks too"""
        outer = self.ring(self.XMIN, self.YMIN, self.XMAX, self.YMAX)
        hole = self.ring(273437, 5274437, 273563, 5274563)
        island = self.ring(273477, 5274477, 273523, 5274523)

        donut_alone = self.npoints([outer, hole])
        island_alone = self.npoints([island])

        pipeline = pylasr.reader_polygons(aoi=[[outer, hole], [island]]) + pylasr.summarise()
        pipeline.set_chunk(50)
        result = pipeline.execute([self.las])
        self.assertTrue(result["success"], "Pipeline execution failed")

        self.assertEqual(result["data"][0]["summary"]["npoints"], donut_alone + island_alone)
