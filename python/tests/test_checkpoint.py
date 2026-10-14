"""Checkpoint reuse and recovery must survive a real process interruption."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import unittest

import pylasr


def checkpoint_pipeline(output, state, resume=False, changed=False, extension="laz"):
    pattern = str(Path(output) / f"*.{extension}")
    writer = pylasr.write_pcd(pattern) if extension == "pcd" else pylasr.write_las(pattern)
    pipeline = (pylasr.reader_coverage(filter=["Z > -99999"]) + writer
                if changed else writer)
    pipeline.set_sequential_strategy()
    pipeline.set_progress(False)
    pipeline.set_checkpoint(str(state), resume=resume)
    return pipeline


class TestCheckpoint(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="pylasr-checkpoint-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name).resolve()
        self.inputs = self.root / "input"
        self.outputs = self.root / "output"
        self.inputs.mkdir()
        self.outputs.mkdir()
        self.state = self.root / "checkpoint"
        self.data = Path(__file__).resolve().parents[2] / "inst" / "extdata"

    def make_inputs(self, count=3, name="Example.las"):
        files = [self.inputs / f"tile_{i:02d}.las" for i in range(count)]
        for path in files:
            shutil.copyfile(self.data / name, path)
        return [str(path) for path in files]

    def pipeline(self, resume=False, changed=False):
        return checkpoint_pipeline(self.outputs, self.state, resume, changed)

    def written_files(self, result):
        self.assertTrue(result["success"])
        writers = [stage["write_las"] for stage in result["data"]
                   if "write_las" in stage]
        self.assertEqual(len(writers), 1)
        return writers[0]

    @staticmethod
    def snapshot(paths, age=False):
        snapshots = {}
        for index, path in enumerate(paths):
            path = Path(path)
            if age:
                # A rewrite cannot accidentally retain an identical timestamp
                # on filesystems with coarse modification-time resolution.
                stamp = 1600000000000000000 + index * 1000000000
                os.utime(path, ns=(stamp, stamp))
            snapshots[str(path)] = (
                path.stat().st_mtime_ns,
                hashlib.sha256(path.read_bytes()).hexdigest(),
            )
        return snapshots

    @staticmethod
    def point_count(path):
        with open(path, "rb") as stream:
            header = stream.read(375)
        if header[:4] != b"LASF":
            raise AssertionError(f"Invalid LAS/LAZ header: {path}")
        legacy = struct.unpack_from("<I", header, 107)[0]
        if header[25] >= 4 and len(header) >= 255:
            extended = struct.unpack_from("<Q", header, 247)[0]
            return extended or legacy
        return legacy

    def test_complete_resume_preserves_results_and_outputs(self):
        files = self.make_inputs()
        first = self.written_files(self.pipeline().execute(files))
        self.assertEqual(first, [str(self.outputs / f"tile_{i:02d}.laz")
                                 for i in range(len(files))])
        original = self.snapshot(first, age=True)
        self.assertEqual(len(list((self.state / "completed").glob("*.json"))), 3)

        # Reconstructing the pipeline generates new stage UIDs but an identical
        # job must still reuse its completed outputs.
        resumed = self.written_files(self.pipeline(resume=True).execute(files))
        self.assertEqual(resumed, first)
        self.assertEqual(self.snapshot(resumed), original)

    def test_default_json_paths_are_unique(self):
        pipeline = self.pipeline()
        pipeline.set_files(self.make_inputs())
        first = Path(pipeline.write_json())
        self.addCleanup(first.unlink, missing_ok=True)
        first_contents = first.read_text()
        second = Path(pipeline.write_json())
        self.addCleanup(second.unlink, missing_ok=True)
        self.assertNotEqual(first, second)
        self.assertEqual(first.read_text(), first_contents)
        config = json.loads(first_contents)
        self.assertEqual(json.loads(second.read_text())["processing"], config["processing"])
        self.assertEqual(config["processing"]["checkpoint"], str(self.state))
        self.assertFalse(config["processing"]["resume"])

    def test_changed_pipeline_and_input_are_rejected_without_rewriting(self):
        files = self.make_inputs()
        first = self.written_files(self.pipeline().execute(files))
        original = self.snapshot(first, age=True)
        with self.assertRaisesRegex(ValueError, "inputs, pipeline.*changed"):
            self.pipeline(resume=True, changed=True).execute(files)
        self.assertEqual(self.snapshot(first), original)

        # Keep both size and modification time: the input fingerprint must
        # detect a same-size content change, not just a filesystem timestamp.
        path = Path(files[0])
        stat = path.stat()
        with path.open("r+b") as stream:
            stream.seek(-1, os.SEEK_END)
            previous = stream.read(1)
            stream.seek(-1, os.SEEK_END)
            stream.write(bytes([previous[0] ^ 1]))
        os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        self.assertEqual(path.stat().st_size, stat.st_size)
        with self.assertRaisesRegex(ValueError, "inputs, pipeline.*changed"):
            self.pipeline(resume=True).execute(files)
        self.assertEqual(self.snapshot(first), original)

    @unittest.skipUnless(os.name == "posix", "Requires POSIX file-size limits")
    def test_failed_writers_publish_no_receipt_and_can_resume(self):
        try:
            import resource
            import signal
        except ImportError:
            self.skipTest("resource and signal support is unavailable")
        if not hasattr(resource, "RLIMIT_FSIZE") or not hasattr(signal, "SIGXFSZ"):
            self.skipTest("File-size limits and SIGXFSZ are unavailable")

        files = self.make_inputs(1, "MixedConifer.las")
        expected_count = self.point_count(files[0])
        module = Path(pylasr.__file__).resolve()
        child_code = """
import json, pathlib, resource, signal, sys
sys.path.insert(0, sys.argv[1])
import pylasr
assert pathlib.Path(pylasr.__file__).resolve() == pathlib.Path(sys.argv[2])
sys.path.insert(0, sys.argv[3])
from test_checkpoint import checkpoint_pipeline
pipeline = checkpoint_pipeline(sys.argv[4], sys.argv[5], extension=sys.argv[6])
signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
resource.setrlimit(resource.RLIMIT_FSIZE, (65536, 65536))
try:
    pipeline.execute(json.loads(sys.argv[7]))
except Exception as error:
    print("EXPECTED_WRITE_FAILURE: " + str(error), flush=True)
    sys.exit(23)
print("UNEXPECTED_WRITE_SUCCESS", flush=True)
"""
        for extension in ("las", "laz", "copc.laz", "pcd"):
            with self.subTest(extension=extension):
                directory = self.outputs / extension
                directory.mkdir()
                state = self.root / f"state-{extension}"
                output = directory / f"tile_00.{extension}"
                # subprocess.run kills and waits for the child on timeout. The
                # parent's limits remain unchanged for the subsequent resume.
                child = subprocess.run(
                    [sys.executable, "-c", child_code, str(module.parent), str(module),
                     str(Path(__file__).resolve().parent), str(directory), str(state),
                     extension, json.dumps(files)],
                    capture_output=True, text=True, timeout=15,
                )
                diagnostic = child.stdout + child.stderr
                self.assertEqual(child.returncode, 23, diagnostic)
                self.assertIn("EXPECTED_WRITE_FAILURE:", diagnostic)
                self.assertTrue((state / "manifest.json").is_file())
                self.assertFalse(output.exists(), diagnostic)
                self.assertEqual(list((state / "completed").glob("*.json")), [])

                pipeline = checkpoint_pipeline(directory, state, resume=True,
                                               extension=extension)
                result = pipeline.execute(files)
                self.assertTrue(result["success"])
                writer = "write_pcd" if extension == "pcd" else "write_las"
                paths = [stage[writer] for stage in result["data"] if writer in stage]
                self.assertEqual(paths, [[str(output)]])
                self.assertGreater(output.stat().st_size, 65536)
                self.assertEqual(len(list((state / "completed").glob("*.json"))), 1)

                # Count decoded points, rather than trusting the output header.
                # This read is outside checkpoint mode, where summaries are valid.
                read = pylasr.summarise(metrics=["count"])
                read.set_sequential_strategy()
                read.set_progress(False)
                result = read.execute(str(output))
                self.assertTrue(result["success"])
                summary = next(stage["summary"] for stage in result["data"]
                               if "summary" in stage)
                self.assertEqual(summary["npoints"], expected_count)
                self.assertEqual(summary["metrics"]["count"], [expected_count])

    def test_killed_run_releases_lock_and_resumes_completed_chunks(self):
        files = self.make_inputs(12, "MixedConifer.las")
        module = Path(pylasr.__file__).resolve()
        child_code = (
            "import json, pathlib, sys; "
            "sys.path.insert(0, sys.argv[1]); import pylasr; "
            "assert pathlib.Path(pylasr.__file__).resolve() == pathlib.Path(sys.argv[2]); "
            "sys.path.insert(0, sys.argv[3]); "
            "from test_checkpoint import checkpoint_pipeline; "
            "pipeline = checkpoint_pipeline(sys.argv[4], sys.argv[5]); "
            "result = pipeline.execute(json.loads(sys.argv[6])); "
            "pathlib.Path(sys.argv[7]).write_text(json.dumps(result))"
        )
        log_path = self.root / "child.log"
        completed_dir = self.state / "completed"
        process = None
        with log_path.open("w") as log:
            try:
                process = subprocess.Popen(
                    [sys.executable, "-c", child_code, str(module.parent),
                     str(module), str(Path(__file__).resolve().parent),
                     str(self.outputs), str(self.state), json.dumps(files),
                     str(self.root / "child-result.json")],
                    stdout=log, stderr=subprocess.STDOUT,
                )
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    receipts = list(completed_dir.glob("*.json"))
                    if receipts or process.poll() is not None:
                        break
                    time.sleep(0.002)
                self.assertIsNone(process.poll(), log_path.read_text())
                self.assertGreater(len(receipts), 0, "No chunk completed before the timeout")
                self.assertLess(len(receipts), len(files), "Child completed before interruption")

                # Another process must not resume a checkpoint that is active.
                with self.assertRaisesRegex(ValueError, "locked by another process"):
                    self.pipeline(resume=True).execute(files)
                process.kill()
                process.wait(timeout=10)
                self.assertNotEqual(process.returncode, 0)
            finally:
                if process is not None and process.poll() is None:
                    process.kill()
                    process.wait(timeout=10)

        # Publication is atomic, so inspect the final durable receipts only
        # after the killed child has exited. Unreceipted outputs may be retried.
        receipts = list(completed_dir.glob("*.json"))
        self.assertGreater(len(receipts), 0)
        self.assertLess(len(receipts), len(files))
        completed_outputs = []
        for path in receipts:
            receipt = json.loads(path.read_text())
            completed_outputs.extend(record["path"]
                                     for record in receipt["outputs"].values()
                                     if record["present"])
        self.assertEqual(len(completed_outputs), len(receipts))
        original = self.snapshot(completed_outputs, age=True)

        # Rebuild the same pipeline with the same engine, changing only resume.
        # Success also proves that killing the child released its lock.
        resumed = self.written_files(self.pipeline(resume=True).execute(files))
        expected = [str(self.outputs / f"tile_{i:02d}.laz") for i in range(len(files))]
        self.assertEqual(resumed, expected)
        self.assertEqual(self.snapshot(completed_outputs), original)
        self.assertEqual(len(list(completed_dir.glob("*.json"))), len(files))
        # These copies overlap completely: normal spatial-collection semantics
        # include all copies in every chunk, including chunks processed on resume.
        expected_count = self.point_count(files[0]) * len(files)
        self.assertEqual([self.point_count(path) for path in resumed],
                         [expected_count] * len(files))


if __name__ == "__main__":
    unittest.main()
