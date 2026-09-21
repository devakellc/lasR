# Ground-truth reprojections used in the assertions below were verified independently
# with the command-line tool `gdaltransform` (PROJ/GDAL). Topography.las is in
# NAD83(CSRS) / MTM zone 7 (EPSG:2949), located in Quebec, Canada.
#
# These tests deliberately rely on lasR-native readers (summarise/callback with
# noread = TRUE) so they do not depend on terra/sf being loadable. The optional
# rasterization test uses terra and is skipped if terra is unavailable.

read_crs   = function(f) { exec(summarise(), on = f, noread = TRUE)$crs }
read_epsg  = function(f) { exec(summarise(), on = f, noread = TRUE)$epsg }
read_range = function(f)
{
  exec(callback(function(d) c(xmin = min(d$X), xmax = max(d$X),
                              ymin = min(d$Y), ymax = max(d$Y),
                              zmin = min(d$Z), zmax = max(d$Z)), expose = "xyz"),
       on = f, noread = TRUE)
}

test_that("transform_crs reprojects to a geographic CRS (EPSG:4326)",
{
  f <- system.file("extdata", "Topography.las", package = "lasR")
  out <- tempfile(fileext = ".las")

  exec(reader_las() + transform_crs(4326) + write_las(out), on = f, noread = TRUE)

  expect_equal(read_epsg(out), 4326L)
  expect_match(read_crs(out), "WGS 84")

  r <- read_range(out)
  # Expected lon/lat (gdaltransform): lon ~ -70.918..-70.914, lat ~ 47.6076..47.6102
  expect_gt(unname(r["xmin"]), -70.93)
  expect_lt(unname(r["xmax"]), -70.90)
  expect_gt(unname(r["ymin"]), 47.60)
  expect_lt(unname(r["ymax"]), 47.62)
})

test_that("transform_crs reprojects to a projected CRS (EPSG:32619) and preserves Z",
{
  f <- system.file("extdata", "Topography.las", package = "lasR")
  src <- read_range(f)
  out <- tempfile(fileext = ".las")

  exec(reader_las() + transform_crs(32619) + write_las(out), on = f, noread = TRUE)

  expect_equal(read_epsg(out), 32619L)
  expect_match(read_crs(out), "UTM zone 19N")

  r <- read_range(out)
  # Expected UTM 19N (gdaltransform): center ~ 355975, 5274614
  expect_gt(unname(r["xmin"]), 355800)
  expect_lt(unname(r["xmax"]), 356150)
  expect_gt(unname(r["ymin"]), 5274400)
  expect_lt(unname(r["ymax"]), 5274800)

  # Horizontal-only reprojection: Z is preserved exactly
  expect_equal(unname(r["zmin"]), unname(src["zmin"]))
  expect_equal(unname(r["zmax"]), unname(src["zmax"]))
})

test_that("transform_crs moves coordinates whereas set_crs only relabels",
{
  f <- system.file("extdata", "Topography.las", package = "lasR")
  src <- read_range(f)

  # set_crs: coordinates are unchanged
  o1 <- tempfile(fileext = ".las")
  exec(reader_las() + set_crs(32619) + write_las(o1), on = f, noread = TRUE)
  r1 <- read_range(o1)
  expect_equal(unname(r1["xmin"]), unname(src["xmin"]))
  expect_equal(unname(r1["ymin"]), unname(src["ymin"]))

  # transform_crs: coordinates are reprojected (changed)
  o2 <- tempfile(fileext = ".las")
  exec(reader_las() + transform_crs(32619) + write_las(o2), on = f, noread = TRUE)
  r2 <- read_range(o2)
  expect_false(isTRUE(all.equal(unname(r2["xmin"]), unname(src["xmin"]))))
  expect_false(isTRUE(all.equal(unname(r2["ymin"]), unname(src["ymin"]))))
})

test_that("transform_crs is a near-identity when target equals source",
{
  f <- system.file("extdata", "Topography.las", package = "lasR")
  src <- read_range(f)
  out <- tempfile(fileext = ".las")

  exec(reader_las() + transform_crs(2949) + write_las(out), on = f, noread = TRUE)

  expect_equal(read_epsg(out), 2949L)
  r <- read_range(out)
  expect_equal(unname(r["xmin"]), unname(src["xmin"]), tolerance = 0.01)
  expect_equal(unname(r["ymax"]), unname(src["ymax"]), tolerance = 0.01)
})

test_that("transform_crs accepts a WKT string",
{
  f <- system.file("extdata", "Topography.las", package = "lasR")
  wkt <- 'GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563,AUTHORITY["EPSG","7030"]],AUTHORITY["EPSG","6326"]],PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]],AXIS["Latitude",NORTH],AXIS["Longitude",EAST],AUTHORITY["EPSG","4326"]]'
  out <- tempfile(fileext = ".las")

  exec(reader_las() + transform_crs(wkt) + write_las(out), on = f, noread = TRUE)

  expect_equal(read_epsg(out), 4326L)
  r <- read_range(out)
  expect_gt(unname(r["xmin"]), -70.93)
  expect_lt(unname(r["xmax"]), -70.90)
})

test_that("transform_crs fails with an invalid target CRS",
{
  f <- system.file("extdata", "Example.las", package = "lasR")
  expect_error(exec(reader_las() + transform_crs(12) + write_las(), on = f, noread = TRUE))
})

test_that("transform_crs reprojects the coverage extent for downstream rasterization",
{
  # Exercises the parser-level extent propagation: if the coverage extent were not
  # reprojected, the (master) raster would be allocated in the source CRS and the
  # reprojected points would fall outside it, producing an empty raster.
  skip_if_not_installed("terra")

  f <- system.file("extdata", "Topography.las", package = "lasR")
  tif <- tempfile(fileext = ".tif")

  exec(reader_las() + transform_crs(32619) + rasterize(10, "count", ofile = tif), on = f)

  r <- terra::rast(tif)
  e <- as.vector(terra::ext(r))
  expect_gt(e[["xmin"]], 355000)
  expect_lt(e[["xmax"]], 357000)
  expect_gt(e[["ymin"]], 5274000)
  expect_lt(e[["ymax"]], 5275000)
  # The raster is correctly placed, so it actually contains the points
  expect_gt(sum(terra::values(r), na.rm = TRUE), 0)
})

read_range_xyz = function(f, pipeline = NULL)
{
  cb = callback(function(d) c(xmin = min(d$X), xmax = max(d$X),
                              ymin = min(d$Y), ymax = max(d$Y),
                              zmin = min(d$Z), zmax = max(d$Z),
                              n = length(d$X),
                              nfinite = sum(is.finite(d$X) & is.finite(d$Y) & is.finite(d$Z))),
                expose = "xyz")
  if (is.null(pipeline)) exec(cb, on = f, noread = TRUE)
  else exec(pipeline + cb, on = f, noread = TRUE)
}

test_that("transform_crs reprojects PCD float coordinates without corrupting them",
{
  # PCD stores X/Y/Z as raw float/double (not scaled int32) and carries no CRS. Writing
  # reprojected coordinates with the int32 setter used to reinterpret the float bytes and
  # produce NaN. Assign a geographic CRS then reproject to Web Mercator.
  f <- system.file("extdata", "pcd_ascii.pcd", package = "lasR")

  src <- read_range_xyz(f)
  out <- read_range_xyz(f, set_crs(4326) + transform_crs(3857))

  # No corruption (the bug produced NaN) and no points spuriously dropped.
  expect_equal(unname(out["nfinite"]), unname(out["n"]))
  expect_equal(unname(out["n"]), unname(src["n"]))
  expect_true(all(is.finite(out)))

  # Coordinates were really reprojected from degrees to metres (magnitudes blow up).
  expect_gt(abs(unname(out["xmin"])), 1e6)
  expect_gt(abs(unname(out["ymin"])), 1e5)

  # Z (elevation) is preserved unchanged.
  expect_equal(unname(out["zmin"]), unname(src["zmin"]), tolerance = 1e-4)
  expect_equal(unname(out["zmax"]), unname(src["zmax"]), tolerance = 1e-4)
})

test_that("transform_crs writes reprojected PCD float coordinates correctly to LAS",
{
  # Regression for the PCD-float -> LAS path. The in-memory float schema keeps identity
  # scale/offset (so callbacks/get_x read the coordinate directly), but write_las() quantizes
  # to LAS int32. If it used the float schema's 1.0 scale, sub-degree lon/lat would all collapse
  # to 0. transform_crs records a target-appropriate scale/offset on the header and the LAS
  # writer uses it for float/double axes. Here the PCD coords are treated as Web Mercator metres
  # then reprojected to lon/lat, giving tiny sub-degree values that expose the bug.
  f <- system.file("extdata", "pcd_ascii.pcd", package = "lasR")
  inmem <- read_range_xyz(f, set_crs(3857) + transform_crs(4326))

  o <- tempfile(fileext = ".las")
  on.exit(unlink(o), add = TRUE)
  exec(reader_las() + set_crs(3857) + transform_crs(4326) + write_las(o), on = f, noread = TRUE)
  onlas <- read_range_xyz(o)

  expect_equal(unname(onlas["n"]), unname(inmem["n"]))
  # Reprojected lon/lat are sub-degree but non-zero; the bug collapsed every coordinate to 0.
  expect_gt(abs(unname(onlas["xmin"])), 1e-5)
  expect_gt(abs(unname(onlas["ymin"])), 1e-6)
  expect_false(isTRUE(all.equal(unname(onlas["xmin"]), unname(onlas["xmax"]))))
  # And they match the in-memory reprojected coordinates within LAS quantization (~1e-7). The
  # values are tiny, so compare with an absolute (not relative) tolerance.
  expect_lt(abs(unname(onlas["xmin"]) - unname(inmem["xmin"])), 1e-6)
  expect_lt(abs(unname(onlas["xmax"]) - unname(inmem["xmax"])), 1e-6)
  expect_lt(abs(unname(onlas["ymin"]) - unname(inmem["ymin"])), 1e-6)
  expect_lt(abs(unname(onlas["ymax"]) - unname(inmem["ymax"])), 1e-6)
})

test_that("transform_crs keeps projected precision for projected PCD writes to LAS",
{
  # Projected -> projected from a float source. The PCD schema scale is a placeholder (1.0), so
  # transform_crs must pick a fine projected scale (1 cm) for the LAS quantization rather than
  # reuse it. Otherwise the LAS output is quantized to whole units (~0.3 m error here). (For an
  # INT32/LAS source the real schema scale is reused, exercised by the other tests above.)
  f <- system.file("extdata", "pcd_ascii.pcd", package = "lasR")
  inmem <- read_range_xyz(f, set_crs(3857) + transform_crs(32619))

  o <- tempfile(fileext = ".las")
  on.exit(unlink(o), add = TRUE)
  exec(reader_las() + set_crs(3857) + transform_crs(32619) + write_las(o), on = f, noread = TRUE)
  onlas <- read_range_xyz(o)

  expect_equal(unname(onlas["n"]), unname(inmem["n"]))
  # Round-trip preserves the reprojected coordinates to centimetre level, not whole units.
  expect_lt(abs(unname(onlas["xmin"]) - unname(inmem["xmin"])), 0.02)
  expect_lt(abs(unname(onlas["xmax"]) - unname(inmem["xmax"])), 0.02)
  expect_lt(abs(unname(onlas["ymin"]) - unname(inmem["ymin"])), 0.02)
  expect_lt(abs(unname(onlas["ymax"]) - unname(inmem["ymax"])), 0.02)
})

test_that("transform_crs scales the tile buffer to the target CRS units",
{
  # triangulate() requires a 20 (source-metre) buffer. After reprojecting metres -> degrees
  # that buffer must be expressed in degrees (~1.8e-4) for the downstream rasterize halo; if
  # it stayed at 20 it would be read as 20 degrees and the master raster would balloon by
  # ceil(20 / 1e-4) ~ 1e5 pixels per side (out of memory). The fix keeps the raster small.
  skip_if_not_installed("terra")

  f <- system.file("extdata", "Topography.las", package = "lasR")
  tif <- tempfile(fileext = ".tif")

  exec(reader_las() + transform_crs(4326) + triangulate() + rasterize(0.0002, "max", ofile = tif), on = f)

  r <- terra::rast(tif)
  # ~0.004 deg coverage at 0.0002 deg => ~20 px plus a small (correctly scaled) halo.
  expect_lt(terra::ncol(r), 1000)
  expect_lt(terra::nrow(r), 1000)
  expect_gt(terra::ncell(r), 0)
})

test_that("transform_crs does not inflate projected buffers from geographic sources",
{
  # The inverse direction must not multiply a projected downstream buffer by the
  # degrees-to-metres ratio. That used to turn triangulate()'s 20 m halo into a
  # multi-million-metre raster buffer and fail with std::bad_alloc.
  skip_if_not_installed("terra")

  f <- system.file("extdata", "Topography.las", package = "lasR")
  geo <- tempfile(fileext = ".las")
  tif <- tempfile(fileext = ".tif")
  on.exit(unlink(c(geo, tif)), add = TRUE)

  exec(reader_las() + transform_crs(4326) + write_las(geo), on = f, noread = TRUE)
  exec(reader_las() + transform_crs(32619) + triangulate() + rasterize(10, "max", ofile = tif), on = geo)

  r <- terra::rast(tif)
  expect_lt(terra::ncol(r), 1000)
  expect_lt(terra::nrow(r), 1000)
  expect_gt(terra::ncell(r), 0)
})

test_that("transform_crs reprojects each file from its own CRS",
{
  f <- system.file("extdata", "Megaplot.las", package = "lasR")
  src <- read_range(f)

  # The same plot written in EPSG:4326, to be read next to the native NAD83 / UTM 17N one
  geo <- tempfile(fileext = ".las")
  exec(reader_las() + transform_crs(4326) + write_las(geo), on = f, noread = TRUE)

  utm <- tempfile(fileext = ".las")
  exec(reader_las() + write_las(utm), on = f, noread = TRUE)

  # The mixed CRS notice reaches stderr through REprintf, not as an R condition, so it is
  # not something expect_warning() can see
  out <- tempfile(fileext = ".las")
  exec(reader_las() + transform_crs(26917) + write_las(out), on = c(utm, geo), noread = TRUE)

  # Both files land back on the plot instead of the geographic file being relabelled. The
  # tolerance is absolute and in metres: the round trip through degrees quantizes at 1e-7 deg,
  # about 1.1 cm. expect_equal() would compare relative to coordinates of 5e6 and pass on a
  # file that landed kilometres away
  r <- read_range(out)
  expect_equal(read_epsg(out), 26917L)
  for (corner in c("xmin", "xmax", "ymin", "ymax"))
    expect_lt(abs(unname(r[corner]) - unname(src[corner])), 0.05)
})

test_that("a query spanning files of different CRS is refused",
{
  f <- system.file("extdata", "Megaplot.las", package = "lasR")

  geo <- tempfile(fileext = ".las")
  exec(reader_las() + transform_crs(4326) + write_las(geo), on = f, noread = TRUE)

  utm <- tempfile(fileext = ".las")
  exec(reader_las() + write_las(utm), on = f, noread = TRUE)

  pipeline <- reader_rectangles(684700, 5017700, 685100, 5018100) + transform_crs(26917) + summarise()
  expect_error(exec(pipeline, on = c(utm, geo), noread = TRUE), "different CRS")
})

# The vertical tests below use EGM96 (EPSG:5773), whose grid ships with PROJ, and NAVD88
# (EPSG:5703), whose grid does not. Ground truth from cs2cs: a point at 100 m EGM96 height in
# the Megaplot area is at 64.501 m ellipsoidal height, a -35.5 m geoid undulation.

test_that("transform_crs keeps the vertical CRS of the source when the target has none",
{
  f <- system.file("extdata", "Megaplot.las", package = "lasR")
  src <- tempfile(fileext = ".las")
  exec(reader_las() + set_crs("EPSG:26917+5703") + write_las(src), on = f, noread = TRUE)

  out <- tempfile(fileext = ".las")
  exec(reader_las() + transform_crs(32617) + write_las(out), on = src, noread = TRUE)

  # The heights did not move, so the vertical CRS that describes them must not move either
  expect_match(read_crs(out), "NAVD88")
  expect_equal(read_range(out)[c("zmin", "zmax")], read_range(src)[c("zmin", "zmax")])
})

test_that("transform_crs reprojects Z through the geoid when the target describes the heights",
{
  f <- system.file("extdata", "Megaplot.las", package = "lasR")
  src <- tempfile(fileext = ".las")
  exec(reader_las() + set_crs("EPSG:26917+5773") + write_las(src), on = f, noread = TRUE)

  # The EGM96 grid ships with PROJ but the machine running this may not have it, and the
  # stage refuses a transformation it cannot do properly rather than leaving Z untouched
  out <- tempfile(fileext = ".las")
  res <- try(exec(reader_las() + transform_crs(4979) + write_las(out), on = src, noread = TRUE), silent = TRUE)
  skip_if(inherits(res, "try-error"), "no vertical transformation available: the EGM96 grid is missing")

  # cs2cs puts the undulation here at -35.5 m; the tolerance is absolute and in metres
  before <- read_range(src)
  after <- read_range(out)
  expect_lt(abs(unname(after["zmin"] - before["zmin"]) + 35.5), 0.2)
  expect_lt(abs(unname(after["zmax"] - before["zmax"]) + 35.5), 0.2)
})

test_that("transform_crs leaves Z alone when the vertical CRS does not change",
{
  f <- system.file("extdata", "Megaplot.las", package = "lasR")
  src <- tempfile(fileext = ".las")
  exec(reader_las() + set_crs("EPSG:26917+5703") + write_las(src), on = f, noread = TRUE)

  out <- tempfile(fileext = ".las")
  exec(reader_las() + transform_crs("EPSG:32617+5703") + write_las(out), on = src, noread = TRUE)

  expect_equal(read_range(out)[c("zmin", "zmax")], read_range(src)[c("zmin", "zmax")])
  expect_match(read_crs(out), "NAVD88")
})

test_that("transform_crs refuses to shift heights the source does not describe",
{
  f <- system.file("extdata", "Megaplot.las", package = "lasR")
  out <- tempfile(fileext = ".las")

  expect_error(exec(reader_las() + transform_crs("EPSG:32617+5703") + write_las(out), on = f, noread = TRUE),
               "the source does not")
})

test_that("transform_crs refuses a ballpark vertical transformation",
{
  f <- system.file("extdata", "Megaplot.las", package = "lasR")
  src <- tempfile(fileext = ".las")
  exec(reader_las() + set_crs("EPSG:26917+5703") + write_las(src), on = f, noread = TRUE)

  # Where the NAVD88 geoid grid is installed the transformation is not ballpark and rightly
  # succeeds, so what the machine has decides which half of this is exercised
  out <- tempfile(fileext = ".las")
  res <- try(exec(reader_las() + transform_crs(4979) + write_las(out), on = src, noread = TRUE), silent = TRUE)
  skip_if(!inherits(res, "try-error"), "the NAVD88 geoid grid is installed, the transformation is not ballpark")

  expect_match(conditionMessage(attr(res, "condition")), "no vertical transformation")
})
