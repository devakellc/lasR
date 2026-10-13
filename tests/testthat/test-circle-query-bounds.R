test_that("a circle enclosing a tile preserves its full point population",
{
  f <- system.file("extdata", "Megaplot.las", package = "lasR")
  reference <- exec(summarise(), on = f, progress = FALSE)$npoints
  query <- reader_circles(684876.6, 5017902, 8000)

  for (buffer in c(0, 20))
  {
    streamed <- exec(query + summarise(), on = f, buffer = buffer,
                     progress = FALSE)
    loaded <- exec(query + sort_points() + summarise(), on = f,
                   buffer = buffer, progress = FALSE)
    expect_equal(streamed$npoints, reference)
    expect_equal(loaded$npoints, reference)

    output <- exec(query + write_las(tempfile(fileext = ".las")), on = f,
                   buffer = buffer, progress = FALSE)
    expect_equal(exec(summarise(), on = output, progress = FALSE)$npoints,
                 reference)
    unlink(output)
  }
})

test_that("partially overlapping circles keep their requested geometry",
{
  points <- expand.grid(X = as.numeric(0:20), Y = as.numeric(0:16))
  points$Z <- points$X + points$Y + 1
  f <- exec(write_las(tempfile(fileext = ".las")), on = points,
            progress = FALSE)
  on.exit(unlink(f))
  cases <- list(c(0, 4, 9), c(-3, 8, 7), c(18, 15, 6))

  for (circle in cases)
  {
    expected <- points[(points$X - circle[1])^2 +
                       (points$Y - circle[2])^2 <= circle[3]^2, ]
    query <- reader_circles(circle[1], circle[2], circle[3])
    for (input in list(points, f))
    {
      for (buffer in c(0, 3))
      {
        output <- exec(query + write_las(tempfile(fileext = ".las")),
                       on = input, buffer = buffer, progress = FALSE)
        actual <- exec(callback(function(data) data, expose = "xyz",
                                no_las_update = TRUE), on = output,
                       progress = FALSE)
        actual <- actual[order(actual$X, actual$Y), ]
        reference <- expected[order(expected$X, expected$Y), ]
        rownames(actual) <- rownames(reference) <- NULL
        expect_equal(unname(as.matrix(actual)), unname(as.matrix(reference)))
        expect_equal(exec(query + summarise(), on = input,
                          buffer = buffer, progress = FALSE)$npoints,
                     nrow(expected))
        unlink(output)
      }
    }
  }
})

test_that("large circular raster queries stay bounded by the data extent",
{
  skip_if_not_installed("terra")
  f <- system.file("extdata", "Megaplot.las", package = "lasR")
  reference <- exec(rasterize(1, "max"), on = f, progress = FALSE)

  for (buffer in c(0, 20))
  {
    # This query is far too large to allocate its entire raster bounding box.
    query <- reader_circles(684876.6, 5017902, 1e7)
    for (ofile in c(tempfile(fileext = ".tif"),
                    paste0(tempfile(), "_*.tif")))
    {
      actual <- exec(query + rasterize(1, "max", ofile = ofile), on = f,
                     buffer = buffer, progress = FALSE)
      expect_equal(as.vector(terra::ext(actual)),
                   as.vector(terra::ext(reference)))
      expect_equal(terra::values(actual), terra::values(reference))
      unlink(terra::sources(actual))
    }
  }
})

test_that("cropping a circular raster leaves its buffer mask in place",
{
  skip_if_not_installed("terra")
  points <- expand.grid(X = as.numeric(0:20), Y = as.numeric(0:16))
  points$Z <- points$X + points$Y + 1
  # Far-away points expand the reference coverage without entering the query.
  extended <- rbind(points, data.frame(X = c(-100, 100),
                                      Y = c(-100, 100), Z = 1))
  input <- exec(write_las(tempfile(fileext = ".las")), on = points,
                progress = FALSE)
  expanded <- exec(write_las(tempfile(fileext = ".las")), on = extended,
                   progress = FALSE)
  on.exit(unlink(c(input, expanded)))
  query <- reader_circles(0, 4, 9)
  for (ofile in c(tempfile(fileext = ".tif"),
                  paste0(tempfile(), "_*.tif")))
  {
    actual <- exec(query + rasterize(1, "z_mean", ofile = ofile), on = input,
                   buffer = 3, progress = FALSE)
    reference <- exec(query + rasterize(1, "z_mean"), on = expanded,
                      buffer = 3, progress = FALSE)
    reference <- terra::crop(reference, actual)
    expect_equal(as.vector(terra::ext(actual)),
                 as.vector(terra::ext(reference)))
    expect_equal(terra::values(actual), terra::values(reference))
    unlink(terra::sources(actual))
  }
})


test_that("circular raster crop bounds follow coordinate transformations",
{
  skip_if_not_installed("terra")
  points <- expand.grid(X = as.numeric(0:20), Y = as.numeric(0:20))
  points$Z <- points$X + 2 * points$Y
  translation <- diag(4)
  translation[1:2, 4] <- c(100, 200)
  rotation <- translation
  rotation[1:2, 1:2] <- matrix(c(0, 1, -1, 0), nrow = 2)

  for (mat in list(translation, rotation))
  {
    transformed <- points
    transformed[, c("X", "Y")] <- t(mat[1:2, 1:2] %*%
                                      t(points[, c("X", "Y")]) + mat[1:2, 4])
    for (center in list(c(10, 10), c(0, 10)))
    {
      new_center <- as.vector(mat[1:2, 1:2] %*% center + mat[1:2, 4])
      query <- reader_circles(center[1], center[2], 5)
      reference <- exec(reader_circles(new_center[1], new_center[2], 5) +
                        rasterize(1, "max"), on = transformed,
                        progress = FALSE)
      for (ofile in c(tempfile(fileext = ".tif"),
                      paste0(tempfile(), "_*.tif")))
      {
        actual <- exec(query + transform_with(mat) +
                       rasterize(1, "max", ofile = ofile), on = points,
                       progress = FALSE)$rasterize
        expect_equal(as.vector(terra::ext(actual)),
                     as.vector(terra::ext(reference)))
        expect_equal(terra::values(actual), terra::values(reference))
        unlink(terra::sources(actual))
      }
    }
  }
})
