#!/usr/bin/env python3
"""
Shared utility functions for tests
"""

import csv
import os
import struct
import subprocess
import time


# Windows file lock retry delay in seconds
FILE_LOCK_RETRY_DELAY = 0.1


def safe_unlink(filepath, warn=False):
    """
    Safely delete a file with Windows compatibility.
    
    On Windows, files can be temporarily locked after operations. This function
    retries once after a brief delay (FILE_LOCK_RETRY_DELAY seconds) if the 
    initial deletion fails.
    
    Parameters:
    -----------
    filepath : str
        Path to the file to delete
    warn : bool
        If True, print a warning when file cannot be deleted.
        If False, fail silently (default).
    """
    if not os.path.exists(filepath):
        return
    
    try:
        os.unlink(filepath)
    except (PermissionError, OSError):
        # On Windows, sometimes files are locked briefly
        time.sleep(FILE_LOCK_RETRY_DELAY)
        try:
            os.unlink(filepath)
        except (PermissionError, OSError) as e:
            if warn:
                # If still can't delete, warn but don't fail
                print(f"Warning: Could not delete file {filepath}: {e}")
            # Otherwise, continue silently


def write_las(path, x, y, z, scale=0.001):
    """Write a minimal uncompressed LAS 1.2 (point format 0) file from raw coordinates.

    There is no laspy here, so this hand-rolls the public header and point records to
    give tests full control over point placement without depending on a fixture file.
    """
    n = len(x)
    header = struct.pack(
        "<4sHHIHH8sBB32s32sHHHIIBHI5I12d",
        b"LASF", 0, 0, 0, 0, 0, b"\x00" * 8, 1, 2,
        b"lasR".ljust(32, b"\x00"), b"pytest".ljust(32, b"\x00"),
        1, 2024, 227, 227, 0, 0, 20, n,
        n, 0, 0, 0, 0,
        scale, scale, scale, 0.0, 0.0, 0.0,
        max(x), min(x), max(y), min(y), max(z), min(z),
    )

    with open(path, "wb") as f:
        f.write(header)
        for xi, yi, zi in zip(x, y, z):
            f.write(struct.pack(
                "<iiiHBBbBH",
                round(xi / scale), round(yi / scale), round(zi / scale),
                0, 0x09, 0, 0, 0, 0,
            ))


def read_points(ofile):
    """Read back the (x, y, z) of every feature a lasR vector stage wrote to ofile."""
    csv_path = ofile + ".csv"
    subprocess.run(
        ["ogr2ogr", "-f", "CSV", csv_path, ofile, "-lco", "GEOMETRY=AS_XYZ"],
        check=True, capture_output=True,
    )

    with open(csv_path, newline="") as f:
        return [(float(row["X"]), float(row["Y"]), float(row["Z"])) for row in csv.DictReader(f)]