# Minimal LAS 1.2 / point-format 0 fixtures from issue-320-repro-data.
# Each contains ten points, an unsupported four-byte attribute followed by a
# float Amplitude attribute whose value is 42.5 for every point.
extra_byte_fixtures = c("extra_byte_deprecated.las", "extra_byte_datatype0.las")

test_that("unsupported extra bytes do not shift supported attribute accessors",
{
  for (fixture in extra_byte_fixtures)
  {
    f = system.file("extdata", fixture, package = "lasR")
    data = read_las(f)

    expect_equal(nrow(data), 10L)
    expect_false(any(c("DeprecatedTuple", "RawBytes") %in% names(data)))
    expect_type(data$Amplitude, "double")
    expect_equal(data$Amplitude, rep(42.5, 10))
    expect_equal(data$Intensity, 100:109)
    expect_equal(data$Classification, rep(2L, 10))
    expect_equal(as.integer(data$Withheld), rep(0L, 10))
    expect_equal(as.integer(data$Synthetic), rep(0L, 10))
    expect_equal(as.integer(data$Keypoint), rep(0L, 10))
  }
})

test_that("supported extra bytes survive streaming LAS and LAZ round trips",
{
  for (fixture in extra_byte_fixtures)
  {
    f = system.file("extdata", fixture, package = "lasR")
    expected = read_las(f)

    for (extension in c(".las", ".laz"))
    {
      output = tempfile(fileext = extension)
      pipeline = reader_las() + write_las(output)
      expect_true(lasR:::get_pipeline_info(pipeline)$streamable)
      expect_equal(exec(pipeline, on = f), output)

      actual = read_las(output)
      expect_equal(names(actual), names(expected))
      expect_equal(actual$Amplitude, rep(42.5, 10))
      expect_equal(actual, expected)
      unlink(output)
    }
  }
})
