test_that("keep_latest keeps the most recent acquisition where two overlap",
{
  f <- system.file("extdata", "Megaplot.las", package = "lasR")
  ext <- c(684766, 5017773, 684994, 5018008)
  third <- (ext[3] - ext[1]) / 3

  # an older acquisition over the left two thirds and a newer one over the right two thirds,
  # so they overlap in the middle third. Both carry a single timestamp, so a short window is
  # enough to tell them apart
  old <- tempfile(fileext = ".las")
  exec(reader_rectangles(ext[1], ext[2], ext[1] + 2 * third, ext[4]) +
       edit_attribute(attribute = "gpstime", value = 100) + write_las(old), on = f, noread = TRUE)

  new <- tempfile(fileext = ".las")
  exec(reader_rectangles(ext[1] + third, ext[2], ext[3], ext[4]) +
       edit_attribute(attribute = "gpstime", value = 200) + write_las(new), on = f, noread = TRUE)

  query <- reader_rectangles(ext[1], ext[2], ext[3], ext[4])
  merged <- exec(query + summarise(), on = c(old, new))
  # cells are aligned on multiples of res, so with third = 76 the seam falls on a cell edge and
  # no cell outside the overlap is shared by the two acquisitions
  kept <- exec(query + keep_latest(res = 2, window = 10) + summarise(), on = c(old, new))
  whole <- exec(reader_las() + summarise(), on = f)

  # merging the two counts the overlap twice; keeping the latest brings it back to one cover
  expect_gt(merged$npoints, whole$npoints)
  expect_equal(kept$npoints, whole$npoints)
})

test_that("keep_latest trims the older acquisition along a seam crossing a cell",
{
  f <- system.file("extdata", "Megaplot.las", package = "lasR")
  ext <- c(684766, 5017773, 684994, 5018008)

  old <- tempfile(fileext = ".las")
  exec(reader_rectangles(ext[1], ext[2], ext[3], ext[4]) +
       edit_attribute(attribute = "gpstime", value = 100) + write_las(old), on = f, noread = TRUE)

  # a seam one metre off the cell lattice. The cells it crosses hold both acquisitions and are
  # won entirely by the newer one, so a strip of the older cover goes with it
  new <- tempfile(fileext = ".las")
  exec(reader_rectangles(ext[1] + 77, ext[2], ext[3], ext[4]) +
       edit_attribute(attribute = "gpstime", value = 200) + write_las(new), on = f, noread = TRUE)

  query <- reader_rectangles(ext[1], ext[2], ext[3], ext[4])
  whole <- exec(reader_las() + summarise(), on = f)
  kept <- exec(query + keep_latest(res = 2, window = 10) + summarise(), on = c(old, new))

  expect_lt(kept$npoints, whole$npoints)
  expect_gt(kept$npoints, 0.99 * whole$npoints)
})

test_that("keep_latest leaves an acquisition that is alone on its ground",
{
  f <- system.file("extdata", "Megaplot.las", package = "lasR")

  before <- exec(reader_las() + summarise(), on = f)
  # the default window is longer than the 551 s this survey took, so its own passes are not
  # read as two acquisitions
  after <- exec(reader_las() + keep_latest(res = 2) + summarise(), on = f)

  expect_equal(after$npoints, before$npoints)
})

test_that("keep_latest rejects a window that keeps nothing and an unknown attribute",
{
  f <- system.file("extdata", "Megaplot.las", package = "lasR")

  expect_error(exec(reader_las() + keep_latest(res = 2, window = 0), on = f), "window must be strictly positive")
  expect_error(exec(reader_las() + keep_latest(res = 2, use_attribute = "nope"), on = f), "No attribute 'nope' found")
})
