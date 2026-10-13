test_that("summary metrics survive a later stage emptying the cloud",
{
  f = system.file("extdata", "extra_byte.las", package = "lasR")
  expected = exec(summarise(metrics = "z_max"), on = f, progress = FALSE)
  output = tempfile(fileext = ".las")
  on.exit(unlink(output))

  ans = exec(summarise(metrics = "z_max") + hag() + write_las(output),
             on = f, progress = FALSE)

  expect_equal(ans$npoints, expected$npoints)
  expect_equal(ans$metrics, expected$metrics)
  expect_false(file.exists(output))
})

test_that("summary orders its own rows when chunks stop at different stages",
{
  previous_strategy = get_parallel_strategy()
  on.exit({
    unset_parallel_strategy()
    set_parallel_strategy(previous_strategy)
  }, add = TRUE)
  folder = tempfile("summary-order-")
  dir.create(folder)
  on.exit(unlink(folder, recursive = TRUE), add = TRUE)
  files = file.path(folder, paste0(1:3, ".las"))
  for (i in seq_along(files))
  {
    points = data.frame(X = i * 100 + c(0, 1, 0, 0.2),
                        Y = c(0, 0, 1, 0.2),
                        Z = c(i, i, i, i + 4),
                        Classification = if (i == 2L) 1L else c(2L, 2L, 2L, 1L))
    exec(write_las(files[i]), on = points, progress = FALSE)
  }

  pipeline = summarise(metrics = "z_max") + hag() +
    summarise(metrics = "z_max")
  strategies = list(sequential())
  if (has_omp_support()) strategies = c(strategies, list(concurrent_files(2)))
  for (strategy in strategies)
  {
    set_parallel_strategy(strategy)
    ans = exec(pipeline, on = files, ncores = strategy, buffer = 0,
               progress = FALSE)
    expect_equal(ans$summary$metrics$z_max, c(5, 6, 7))
    expect_equal(ans$summary.1$metrics$z_max, c(5, 7))
    expect_equal(ans$summary$npoints, 12)
    expect_equal(ans$summary.1$npoints, 8)
  }
})

test_that("a skipped summary returns empty metric columns safely",
{
  f = system.file("extdata", "extra_byte.las", package = "lasR")
  ans = exec(hag() + summarise(metrics = c("z_max", "count")),
             on = f, progress = FALSE)

  expect_equal(ans$npoints, 0)
  expect_equal(nrow(ans$metrics), 0L)
  expect_named(ans$metrics, c("count", "z_max"))
  expect_equal(unname(ans$z_histogram), 0)
  expect_equal(unname(ans$i_histogram), 0)
})
