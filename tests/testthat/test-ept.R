test_that("EPT local read matches LAS source",
{
  ept <- system.file("extdata", "ept-test-multi", "ept.json", package = "lasR")
  las <- system.file("extdata", "Topography.las", package = "lasR")

  ofile <- paste0(tempdir(), "/ept_full.las")
  exec(reader() + write_las(ofile), on = ept)
  full <- exec(reader() + summarise(), on = ofile)
  las_full <- exec(reader() + summarise(), on = las)
  expect_equal(full$npoints, las_full$npoints)
})

test_that("EPT spatial query fetches only needed tiles",
{
  ept <- system.file("extdata", "ept-test-multi", "ept.json", package = "lasR")

  ofile <- paste0(tempdir(), "/ept_bl.las")
  exec(reader_rectangles(273360, 5274360, 273490, 5274490) + write_las(ofile), on = ept)
  quad <- exec(reader() + summarise(), on = ofile)
  expect_equal(quad$npoints, 18806)

  ofile2 <- paste0(tempdir(), "/ept_tr.las")
  exec(reader_rectangles(273510, 5274510, 273640, 5274640) + write_las(ofile2), on = ept)
  quad2 <- exec(reader() + summarise(), on = ofile2)
  expect_equal(quad2$npoints, 23306)
})

test_that("EPT reader detects non-laszip dataType",
{
  ept_dir <- file.path(tempdir(), "ept-bad")
  dir.create(ept_dir, showWarnings = FALSE)

  writeLines('{"bounds":[0,0,0,10,10,10],"boundsConforming":[0,0,0,10,10,10],"dataType":"binary","hierarchyType":"json","schema":[{"name":"X","type":"signed","size":4}],"span":128}',
    file.path(ept_dir, "ept.json"))

  expect_error(
    exec(reader(), on = file.path(ept_dir, "ept.json")),
    "laszip"
  )
})

test_that("EPT pipeline integration works",
{
  ept <- system.file("extdata", "ept-test-multi", "ept.json", package = "lasR")

  ofile <- paste0(tempdir(), "/ept_raster.tif")
  pipeline <- reader() + rasterize(5, "zmax", ofile = ofile)
  ans <- exec(pipeline, on = ept)

  expect_true(file.exists(ofile))
})

test_that("EPT depth filtering works",
{
  ept <- system.file("extdata", "ept-test-multi", "ept.json", package = "lasR")

  # Test data has no 0-0-0-0 tile (hierarchy starts at depth 1), so depth=0
  # yields zero points and must not hang.
  d0 <- exec(reader(depth = 0) + summarise(), on = ept)
  full <- exec(reader() + summarise(), on = ept)

  expect_equal(d0$npoints, 0)
  expect_gt(full$npoints, 0)
})

test_that("Several EPT endpoints are read together",
{
  ept <- system.file("extdata", "ept-test-multi", "ept.json", package = "lasR")

  one <- exec(reader() + summarise(), on = ept)
  two <- exec(reader() + summarise(), on = c(ept, ept))

  # the same endpoint twice covers the same ground twice, as a duplicated file does
  expect_equal(two$npoints, 2 * one$npoints)
})

test_that("Several EPT endpoints are read together under a query",
{
  ept <- system.file("extdata", "ept-test-multi", "ept.json", package = "lasR")

  query <- reader_rectangles(273360, 5274360, 273490, 5274490)
  one <- exec(query + summarise(), on = ept)
  two <- exec(query + summarise(), on = c(ept, ept))

  expect_equal(two$npoints, 2 * one$npoints)
})

test_that("a LAS file and an EPT endpoint are read together",
{
  ept <- system.file("extdata", "ept-test-multi", "ept.json", package = "lasR")

  # a LAS written from the EPT inherits its scale and offset, so the two agree
  las <- tempfile(fileext = ".las")
  exec(reader() + write_las(las), on = ept, noread = TRUE)

  query <- reader_rectangles(273360, 5274360, 273490, 5274490)
  one <- exec(query + summarise(), on = ept)
  two <- exec(query + summarise(), on = c(ept, las))

  expect_equal(two$npoints, 2 * one$npoints)
})

test_that("sources that disagree on scale or offset are refused",
{
  ept <- system.file("extdata", "ept-test-multi", "ept.json", package = "lasR")
  las <- system.file("extdata", "Topography.las", package = "lasR")

  query <- reader_rectangles(273360, 5274360, 273490, 5274490)
  expect_error(exec(query + summarise(), on = c(ept, las)), "scale or offset")
})

test_that("an area of interest prunes the EPT octree traversal",
{
  ept <- system.file("extdata", "ept-test-multi", "ept.json", package = "lasR")
  las <- system.file("extdata", "Topography.las", package = "lasR")

  aoi <- paste0("POLYGON((273357 5274357, 273500 5274357, 273500 5274500, 273643 5274500, ",
                "273643 5274643, 273357 5274643, 273357 5274357))")

  # the area of interest clips the EPT read exactly like the LAS read of the same data
  ofile <- paste0(tempdir(), "/ept_aoi.las")
  exec(reader(aoi = aoi) + write_las(ofile), on = ept)
  from_ept <- exec(reader() + summarise(), on = ofile)

  ofile2 <- paste0(tempdir(), "/las_aoi.las")
  exec(reader(aoi = aoi) + write_las(ofile2), on = las)
  from_las <- exec(reader() + summarise(), on = ofile2)

  expect_equal(from_ept$npoints, from_las$npoints)
  expect_equal(from_ept$npoints, 53153)

  # The four depth 1 nodes split the cube at x = 273500 and y = 5274500. This area of interest
  # excludes the south east node with a margin, so its 20250 points are never downloaded. The count
  # differs from the LAS by two points sitting on the boundary: the EPT re-encoding quantizes the
  # coordinates differently
  aoi <- paste0("POLYGON((273357 5274357, 273480 5274357, 273480 5274520, 273643 5274520, ",
                "273643 5274643, 273357 5274643, 273357 5274357))")

  ofile3 <- paste0(tempdir(), "/ept_aoi_pruned.las")
  exec(reader(aoi = aoi) + write_las(ofile3), on = ept)
  pruned <- exec(reader() + summarise(), on = ofile3)
  expect_equal(pruned$npoints, 46946)

  # an area of interest inside the north east node alone
  aoi <- "POLYGON((273560 5274560, 273620 5274560, 273620 5274620, 273560 5274620, 273560 5274560))"

  ofile4 <- paste0(tempdir(), "/ept_aoi_one_node.las")
  exec(reader(aoi = aoi) + write_las(ofile4), on = ept)
  one_node <- exec(reader() + summarise(), on = ofile4)
  expect_equal(one_node$npoints, 4474)
})
