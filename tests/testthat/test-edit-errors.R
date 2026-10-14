test_that("missing edit targets fail in streaming and loaded pipelines",
{
  f <- system.file("extdata", "Example.las", package = "lasR")
  for (loaded in c(FALSE, TRUE))
  {
    pipeline <- edit_attribute("Z > 0", "nonexistent", 18) + summarise()
    if (loaded) pipeline <- classify_with_ivf() + pipeline
    expect_error(exec(pipeline, on = f, progress = FALSE),
                 "No attribute 'nonexistent' found", fixed = TRUE)
  }
})

test_that("edit target validation restarts for every chunk",
{
  directory <- tempfile()
  dir.create(directory)
  on.exit(unlink(directory, recursive = TRUE))
  points <- expand.grid(X = as.numeric(0:2), Y = as.numeric(0:2))
  points$Z <- 1
  first <- exec(add_extrabytes("uchar", "target", "Edit target") +
                  write_las(file.path(directory, "first.las")),
                on = points, progress = FALSE)
  points$X <- points$X + 100
  second <- exec(write_las(file.path(directory, "second.las")),
                 on = points, progress = FALSE)

  for (loaded in c(FALSE, TRUE))
  {
    pipeline <- edit_attribute("Z > 0", "target", 18) + summarise()
    if (loaded) pipeline <- sort_points() + pipeline
    expect_no_error(exec(pipeline, on = first, progress = FALSE))
    # The same worker sees a valid schema followed by a missing target. A
    # successful validation in the first chunk must not mask the second error.
    expect_error(exec(pipeline, on = c(first, second), ncores = sequential(),
                      progress = FALSE), "No attribute 'target' found",
                 fixed = TRUE)
    expect_error(exec(pipeline, on = c(first, second),
                      ncores = concurrent_files(2), progress = FALSE),
                 "No attribute 'target' found", fixed = TRUE)
  }
})

test_that("optional filter attributes retain their existing behavior",
{
  f <- system.file("extdata", "Example.las", package = "lasR")
  for (loaded in c(FALSE, TRUE))
  {
    pipeline <- edit_attribute("nonexistent > 0", "UserData", 18) +
      summarise()
    if (loaded) pipeline <- sort_points() + pipeline
    expect_no_error(exec(pipeline, on = f, progress = FALSE))
  }
})

test_that("valid edits still select and update the intended points",
{
  f <- system.file("extdata", "Example.las", package = "lasR")
  output <- tempfile(fileext = ".las")
  on.exit(unlink(output))
  for (loaded in c(FALSE, TRUE))
  {
    pipeline <- edit_attribute("Z < 975", "UserData", 18) + write_las(output)
    if (loaded) pipeline <- sort_points() + pipeline
    exec(pipeline, on = f, progress = FALSE)
    actual <- exec(callback(function(data) data, expose = "zu",
                            no_las_update = TRUE), on = output,
                   progress = FALSE)
    expect_true(all(actual$UserData[actual$Z < 975] == 18))
    expect_true(all(actual$UserData[actual$Z >= 975] == 32))
  }
})
