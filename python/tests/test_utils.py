#!/usr/bin/env python3
"""
Shared utility functions for tests
"""

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


def read_raster_cells(path):
    """Read a lasR raster stage's output as a list of (x, y, value) for non-nodata cells.

    Goes through gdal_translate to an Arc/Info ASCII Grid since there is no osgeo/rasterio
    here either; ogrinfo (used for vector outputs) does not read rasters.
    """
    grid_path = path + ".asc"
    if os.path.exists(grid_path):
        os.remove(grid_path)
    subprocess.run(
        ["gdal_translate", "-of", "AAIGrid", path, grid_path],
        check=True, capture_output=True,
    )

    header = {}
    rows = []
    with open(grid_path) as f:
        for line in f:
            key, _, rest = line.strip().partition(" ")
            key = key.lower()
            if key in ("ncols", "nrows"):
                header[key] = int(rest)
            elif key in ("xllcorner", "yllcorner", "cellsize", "nodata_value"):
                header[key] = float(rest)
            else:
                rows.append([float(v) for v in line.split()])

    cellsize = header["cellsize"]
    nodata = header["nodata_value"]
    nrows = header["nrows"]

    cells = []
    for row_idx, row in enumerate(rows):
        y = header["yllcorner"] + (nrows - 1 - row_idx + 0.5) * cellsize
        for col_idx, value in enumerate(row):
            if value == nodata:
                continue
            x = header["xllcorner"] + (col_idx + 0.5) * cellsize
            cells.append((x, y, value))

    return cells