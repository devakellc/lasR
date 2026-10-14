tmpdir = paste0(tempdir(), "/testvpc")
if (!dir.exists(tmpdir)) dir.create(tmpdir)

# create a datset without lax
f = paste0(system.file(package="lasR"), "/extdata/bcts/")
f = list.files(f, pattern = "(?i)\\.la(s|z)$", full.names = TRUE)
z = file.copy(f, tmpdir)
f = list.files(tmpdir, pattern = "(?i)\\.la(s|z)$", full.names = TRUE)

test_that("vpc reader works",
{
  f = system.file("extdata", "bcts/bcts.vpc", package="lasR")
  pipeline = reader_las() + hulls()
  ans = exec(pipeline, on = f)

  expect_s3_class(ans, "sf")
  expect_equal(dim(ans), c(4L,1L))
  expect_equal(length(ans$geom[[1]]), 1L)
})

test_that("vpc writer works",
{
  o = tempfile(fileext = ".vpc")
  pipeline = reader_las() + write_vpc(o)
  ans = exec(pipeline, on = f)

  expect_true(file.exists(ans))

  ans = sf::st_read(ans, quiet = TRUE)
  expect_s3_class(ans, "sf")
  expect_equal(nrow(ans), 4L)
  expect_true(all(sf::st_geometry_type(ans) == rep("POLYGON", 4)))
  expect_equal(as.numeric(sf::st_bbox(ans)), c(-127.627836098, 50.665469135, -127.624899322, 50.675051675))
})

test_that("VPC pointcloud v2 metadata preserves relative and absolute round trips",
{
  file = system.file("extdata", "MixedConifer.las", package = "lasR")
  expected = exec(summarise(), on = file, progress = FALSE)
  folder = tempfile("vpc-pointcloud-v2-")
  dir.create(folder)
  on.exit(unlink(folder, recursive = TRUE))

  for (absolute_path in c(FALSE, TRUE))
  {
    output = file.path(folder, paste0(absolute_path, ".vpc"))
    exec(write_vpc(output, absolute_path = absolute_path), on = file,
         progress = FALSE)
    metadata = readLines(output, warn = FALSE)
    expect_true(any(grepl("https://stac-extensions.github.io/pointcloud/v2.0.0/schema.json",
                          metadata, fixed = TRUE)))
    expect_false(any(grepl("https://stac-extensions.github.io/pointcloud/v1.0.0/schema.json",
                           metadata, fixed = TRUE)))
    properties = sf::st_read(output, quiet = TRUE)
    expect_equal(properties$pc.count, expected$npoints)
    expect_equal(properties$pc.type, "lidar")

    actual = exec(summarise(), on = output, progress = FALSE)
    expect_equal(actual, expected)
  }
})

file.remove(f)

test_that("vpc reader fails",
{
  f = tempfile(fileext = ".vpc")
  file.create(f)
  pipeline = lasR:::nothing()
  expect_error(exec(pipeline, on = f), "JSON parsing error")
})

test_that("vpc writer fails without CRS",
{
  f = system.file("extdata", "nocrs.las", package="lasR")
  o = tempfile(fileext = ".vpc")

  pipeline = reader_las() + write_vpc(o)
  expect_error(exec(pipeline, on = f), "Invalid CRS. Cannot write a VPC file")

  pipeline = reader_las() + set_crs(32617) + write_vpc(o)
  expect_error(exec(pipeline, on = f), NA)
})

test_that("vpc writer uses GPS time",
{
  f <- system.file("extdata", "Topography.las", package="lasR")
  o = tempfile(fileext = ".vpc")

  pipeline = reader_las() + write_vpc(o)

  ans = exec(pipeline, on = f)
  ans = sf::st_read(ans, quiet = TRUE, stringsAsFactors = TRUE)

  date = ans$datetime
  attr(date, "tzone") <- "UTC"

  expect_equal(as.character(date), "2018-01-01")

  pipeline = reader_las() + write_vpc(o, use_gpstime = T)

  ans = exec(pipeline, on = f)
  ans = sf::st_read(ans, quiet = TRUE, stringsAsFactors = TRUE)

  date = ans$datetime
  attr(date, "tzone") <- "UTC"

  expect_equal(as.character(date), "2018-09-07")
})

test_that("vpc writer does not uses GPS time",
{
  f <- system.file("extdata", "Megaplot.las", package="lasR")
  o = tempfile(fileext = ".vpc")

  pipeline = reader_las() + write_vpc(o, use_gpstime = T)

  expect_error(exec(pipeline, on = f), NA)
})

