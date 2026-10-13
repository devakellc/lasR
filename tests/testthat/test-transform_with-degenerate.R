test_that("normalization discards points without non-degenerate TIN support",
{
  # Jitter lets Delaunator triangulate these points, but their original XY
  # coordinates are collinear and cannot define an interpolation surface.
  points = data.frame(
    X = c(0, 1, 2, 1, 3),
    Y = c(0, 1, 2, 1, 2),
    Z = c(1469.25, 1470.25, 1471.25, 1475.25, 1475.25),
    Classification = c(2L, 2L, 2L, 1L, 1L),
    ReturnNumber = 1L,
    NumberOfReturns = 1L
  )
  source = exec(write_las(tempfile(fileext = ".las")), on = points)

  for (input in list(points, source))
  {
    mesh = triangulate(filter = keep_ground())
    output = tempfile(fileext = ".laz")
    pipeline = mesh + transform_with(mesh) + write_las(output)

    expect_null(exec(pipeline, on = input))
    expect_false(file.exists(output))
  }
})

test_that("degenerate triangles do not overwrite supported normalized heights",
{
  # The flat surface has a collinear edge and a duplicate ground location.
  # The final canopy point is outside the convex hull and must be discarded.
  points = data.frame(
    X = c(0, 1, 2, 0, 2, 0, 1, 3),
    Y = c(0, 0, 0, 2, 2, 0, 1, 3),
    Z = c(rep(1469.25, 6), 1474.25, 1474.25),
    Classification = c(rep(2L, 6), 1L, 1L),
    ReturnNumber = 1L,
    NumberOfReturns = 1L
  )
  source = exec(write_las(tempfile(fileext = ".las")), on = points)

  for (input in list(points, source))
  {
    for (attribute in c("", "HAG"))
    {
      mesh = triangulate(filter = keep_ground())
      pipeline = mesh
      if (nzchar(attribute))
        pipeline = pipeline + add_extrabytes("int", "HAG", "Height Above Ground")
      pipeline = pipeline + transform_with(mesh, store_in_attribute = attribute)
      output = exec(pipeline + write_las(tempfile(fileext = ".laz")), on = input)
      result = read_las(output)

      expect_equal(result$X, points$X[1:7])
      expect_equal(result$Y, points$Y[1:7])
      if (nzchar(attribute))
      {
        expect_equal(result$HAG, c(rep(0, 6), 5))
        expect_equal(result$Z, points$Z[1:7])
      }
      else
      {
        expect_equal(result$Z, c(rep(0, 6), 5))
      }
    }
  }
})

test_that("normalization of collinear ground points also handles lidR LAS input",
{
  skip_if_not_installed("lidR")

  points = data.frame(
    X = c(0, 1, 2, 1),
    Y = c(0, 1, 2, 1),
    Z = c(1469.25, 1470.25, 1471.25, 1475.25),
    Classification = c(2L, 2L, 2L, 1L),
    ReturnNumber = 1L,
    NumberOfReturns = 1L
  )
  source = exec(write_las(tempfile(fileext = ".las")), on = points)
  # lidR is optional, as in test-lidR-compatibility.R.
  input = eval(parse(text = "lidR::readLAS(source)"))
  mesh = triangulate(filter = keep_ground())
  output = tempfile(fileext = ".laz")
  pipeline = mesh + transform_with(mesh) + write_las(output)

  expect_null(exec(pipeline, on = input))
  expect_false(file.exists(output))
})
