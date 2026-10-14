test_that("queries outside the data extent ignore overlapping buffers",
{
  skip_if_not_installed("terra")
  points <- expand.grid(X = as.numeric(0:20), Y = as.numeric(0:16))
  points$Z <- 1
  directory <- tempfile()
  dir.create(directory)
  on.exit(unlink(directory, recursive = TRUE))
  f <- exec(write_las(file.path(directory, "input.las")), on = points,
            progress = FALSE)
  centers <- list(c(25, 8), c(-5, 8), c(10, 21), c(10, -5))

  for (center in centers)
  {
    queries <- list(reader_circles(center[1], center[2], 3),
                    reader_rectangles(center[1] - 3, center[2] - 3,
                                      center[1] + 3, center[2] + 3))
    for (query in queries)
    {
      for (input in list(points, f))
      {
        for (loaded in c(FALSE, TRUE))
        {
          pipeline <- query
          if (loaded) pipeline <- pipeline + sort_points()
          # A single raster retains the established all-nodata output for an
          # entirely unmatched query; per-query writers produce no file.
          actual <- exec(pipeline + rasterize(1, "max"), on = input,
                         buffer = 3, progress = FALSE)
          expect_s4_class(actual, "SpatRaster")
          expect_true(all(is.na(terra::values(actual))))
          unlink(terra::sources(actual))

          output <- file.path(directory, "empty_*.tif")
          expect_null(exec(pipeline + rasterize(1, "max", ofile = output),
                           on = input, buffer = 3, progress = FALSE))
          expect_length(list.files(directory, pattern = "^empty_"), 0L)
        }
      }
    }
  }
})

test_that("buffer-only queries do not affect neighboring valid queries",
{
  skip_if_not_installed("terra")
  points <- expand.grid(X = as.numeric(0:20), Y = as.numeric(0:16))
  points$Z <- points$X + points$Y
  f <- exec(write_las(tempfile(fileext = ".las")), on = points,
            progress = FALSE)
  on.exit(unlink(c(f, sub("\\.las$", ".lax", f))))
  for (strategy in list(sequential(), concurrent_files(2)))
  {
    for (loaded in c(FALSE, TRUE))
    {
      reference <- reader_circles(10, 8, 3)
      queries <- reader_circles(c(25, 10, -5), c(8, 8, 8), 3)
      if (loaded)
      {
        reference <- reference + sort_points()
        queries <- queries + sort_points()
      }
      expected <- exec(reference + rasterize(1, "max"), on = f,
                       buffer = 3, ncores = strategy, progress = FALSE)
      actual <- exec(queries + rasterize(1, "max"), on = f,
                     buffer = 3, ncores = strategy, progress = FALSE)
      expect_equal(as.vector(terra::ext(actual)),
                   as.vector(terra::ext(expected)))
      expect_equal(terra::values(actual), terra::values(expected))
      unlink(c(terra::sources(actual), terra::sources(expected)))
    }
  }
})

test_that("queries inside collection gaps can still read buffer points",
{
  points <- expand.grid(X = as.numeric(c(0:2, 8:10)), Y = as.numeric(0:2))
  points$Z <- 1
  directory <- tempfile()
  dir.create(directory)
  on.exit(unlink(directory, recursive = TRUE))
  files <- c(exec(write_las(file.path(directory, "left.las")),
                  on = points[points$X < 3, ], progress = FALSE),
             exec(write_las(file.path(directory, "right.las")),
                  on = points[points$X > 7, ], progress = FALSE))
  query <- reader_rectangles(4, 0, 6, 2)
  capture <- callback(function(data) data, expose = "xyzb",
                      no_las_update = TRUE)
  actual <- exec(query + capture, on = files, buffer = 5, progress = FALSE)
  expect_equal(nrow(actual), nrow(points))
  expect_true(all(actual$Buffer))
})
