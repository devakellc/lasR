test_that("the depth gate of multichm keeps and drops the same peaks as the arm", {
  # a 20 m surface tree over a cell holding an understory peak at 4, 5 and 6 m, plus ground
  # returns: the peak is a candidate of the second layer, 1 m from the surface tree
  X <- c(100.5, 101.5, 101.5, 101.5, 100.5, 101.5)
  Y <- c(100.5, 100.5, 100.5, 100.5, 104.5, 104.5)
  Z <- c(20.0, 4.0, 5.0, 6.0, 0.0, 0.0)
  data <- data.frame(X = X, Y = Y, Z = Z, ReturnNumber = 1L)
  header <- rlas::header_create(data)
  f <- tempfile(fileext = ".las")
  rlas::write.las(f, header, data)

  tops <- function(...) {
    ofile <- tempfile(fileext = ".gpkg")
    ans <- exec(reader() + multichm(res = 1, ws = 3, dist_2d = 0.5, dist_3d = 0.5, ofile = ofile, ...), on = f)
    v <- sf::st_read(ofile, quiet = TRUE)
    sort(round(unname(sf::st_coordinates(v)[, 3]), 2))
  }

  expect_equal(tops(), c(4.95, 20.00))
  expect_equal(tops(cover_min = 0.6, depth_max = 4), 20.00)
  expect_equal(tops(cover_min = 0.6, depth_max = 20), c(4.95, 20.00))
  expect_equal(tops(zmin = 5), 20.00)
  expect_equal(tops(zmin_surface = 25), 4.95)
  expect_equal(tops(cover_min = 0.6, near_2d = 2), 20.00)
  expect_equal(tops(report = "zrep"), c(6.00, 20.00))
})
