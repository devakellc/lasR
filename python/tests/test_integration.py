#!/usr/bin/env python3
"""
Integration tests for pylasr using actual data processing workflows
"""

import os
import glob
import shutil
import struct
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


EPT = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))), "inst", "extdata", "ept-test-multi", "ept.json")


def _npoints(result):
    return sum(e["summary"]["npoints"] for e in result["data"] if "summary" in e)


class TestMultipleEptEndpoints(unittest.TestCase):
    def setUp(self):
        if not PYLASR_AVAILABLE:
            self.skipTest("pylasr not available")

        self.temp_dir = tempfile.mkdtemp()

    def tearDown(self):
        if hasattr(self, "temp_dir") and os.path.exists(self.temp_dir):
            shutil.rmtree(self.temp_dir)

    def test_two_endpoints_read_both(self):
        one = _npoints(pylasr.execute(
            pylasr.reader_coverage() + pylasr.summarise(), EPT))
        two = _npoints(pylasr.execute(
            pylasr.reader_coverage() + pylasr.summarise(), [EPT, EPT]))
        # 7 of the tiles' 73403 points fall outside boundsConforming and are skipped as buffer points
        self.assertEqual(one, 73396)
        self.assertEqual(two, 2 * one)

    def test_two_endpoints_under_a_query(self):
        one = _npoints(pylasr.execute(
            pylasr.reader_rectangles([273360.0], [5274360.0], [273490.0], [5274490.0])
            + pylasr.summarise(), EPT))
        two = _npoints(pylasr.execute(
            pylasr.reader_rectangles([273360.0], [5274360.0], [273490.0], [5274490.0])
            + pylasr.summarise(), [EPT, EPT]))
        self.assertEqual(one, 16294)
        self.assertEqual(two, 2 * one)

    def test_intersection_keeps_attributes_common_to_all_sources(self):
        query = pylasr.reader_rectangles([273360.0], [5274360.0], [273490.0], [5274490.0])
        one = os.path.join(self.temp_dir, "one.las")
        two = os.path.join(self.temp_dir, "two.las")
        pylasr.execute(query + pylasr.write_las(one), EPT)
        pylasr.execute(query + pylasr.write_las(two), [EPT, EPT])

        with open(one, "rb") as f:
            raw_one = f.read()
        with open(two, "rb") as f:
            raw_two = f.read()

        pdrf = raw_two[104]
        # gpstime is in the fixture's schema, so the intersection of two identical
        # sources must keep it: write_las picks a PDRF that carries gpstime
        self.assertIn(pdrf, (6, 7, 8, 10))

        n_one = struct.unpack_from("<Q", raw_one, 247)[0]
        n_two = struct.unpack_from("<Q", raw_two, 247)[0]
        self.assertEqual(n_two, 2 * n_one)


    def _las_from_ept(self):
        # a LAS written from the EPT inherits its scale and offset, so the two agree
        las = os.path.join(self.temp_dir, "from_ept.las")
        pylasr.execute(pylasr.reader_coverage() + pylasr.write_las(las), EPT)
        return las

    def test_las_and_ept_in_one_collection(self):
        las = self._las_from_ept()
        # read file by file the two are one chunk each, so the counts add up. They are not
        # equal: 7 points sit outside boundsConforming and only the EPT skips them as buffer
        from_ept = _npoints(pylasr.execute(pylasr.reader_coverage() + pylasr.summarise(), EPT))
        from_las = _npoints(pylasr.execute(pylasr.reader_coverage() + pylasr.summarise(), las))
        both = _npoints(pylasr.execute(pylasr.reader_coverage() + pylasr.summarise(), [EPT, las]))
        self.assertEqual(both, from_ept + from_las)

    def test_las_and_ept_merge_in_one_chunk(self):
        las = self._las_from_ept()
        query = pylasr.reader_rectangles([273360.0], [5274360.0], [273490.0], [5274490.0])
        one = _npoints(pylasr.execute(query + pylasr.summarise(), EPT))
        two = _npoints(pylasr.execute(query + pylasr.summarise(), [EPT, las]))
        self.assertEqual(two, 2 * one)

    def test_source_order_does_not_matter(self):
        las = self._las_from_ept()
        query = pylasr.reader_rectangles([273360.0], [5274360.0], [273490.0], [5274490.0])
        a = _npoints(pylasr.execute(query + pylasr.summarise(), [EPT, las]))
        b = _npoints(pylasr.execute(query + pylasr.summarise(), [las, EPT]))
        self.assertEqual(a, b)

    def test_mismatched_scale_is_refused(self):
        topography = os.path.normpath(os.path.join(os.path.dirname(EPT), "..", "Topography.las"))
        query = pylasr.reader_rectangles([273360.0], [5274360.0], [273490.0], [5274490.0])
        with self.assertRaises(Exception) as ctx:
            pylasr.execute(query + pylasr.summarise(), [EPT, topography])
        self.assertIn("scale or offset", str(ctx.exception))


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

    def test_aoi_clips_ept_like_las(self):
        """An EPT endpoint is clipped exactly like the LAS holding the same points"""
        ept = None
        for path in [
            "../inst/extdata/ept-test-multi/ept.json",
            "../../inst/extdata/ept-test-multi/ept.json",
            "../../../inst/extdata/ept-test-multi/ept.json",
        ]:
            full_path = os.path.join(os.path.dirname(__file__), path)
            if os.path.exists(full_path):
                ept = full_path
                break

        if not ept:
            self.skipTest("EPT test endpoint not found")

        concave = (
            "POLYGON((273357 5274357, 273500 5274357, 273500 5274500, 273643 5274500, "
            "273643 5274643, 273357 5274643, 273357 5274357))"
        )

        temp_dir = tempfile.mkdtemp()
        try:
            counts = []
            for name, source in [("ept", ept), ("las", self.las)]:
                written = os.path.join(temp_dir, f"aoi_{name}.las")
                pipeline = pylasr.reader_polygons(aoi=concave) + pylasr.write_las(ofile=written)
                self.assertTrue(pipeline.execute([source])["success"], "Pipeline execution failed")

                pipeline = pylasr.reader_coverage() + pylasr.summarise()
                counts.append(pipeline.execute([written])["data"][0]["summary"]["npoints"])

            self.assertEqual(counts[0], counts[1])
        finally:
            shutil.rmtree(temp_dir)
