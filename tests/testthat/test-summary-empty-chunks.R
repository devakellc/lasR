test_that("empty summary rows retain query order and missing-value semantics",
{
  previous_strategy = get_parallel_strategy()
  on.exit({
    unset_parallel_strategy()
    set_parallel_strategy(previous_strategy)
  }, add = TRUE)
  folder = tempfile("summary-empty-")
  dir.create(folder)
  on.exit(unlink(folder, recursive = TRUE), add = TRUE)
  file = file.path(folder, "points.las")
  points = data.frame(X = rep(c(5, 105, 205), each = 3) + c(-1, 0, 1),
                      Y = rep(c(4, 5, 6), 3),
                      Z = c(0, 1, 2, 10, 12, 14, 20, 22, 24),
                      Classification = rep(c(2L, 7L, 2L), each = 3))
  # Pad the file extent so every populated query lies wholly inside it.
  points = rbind(points, data.frame(X = c(0, 210), Y = c(0, 10),
                                    Z = 100, Classification = 2L))
  exec(write_las(file), on = points, progress = FALSE)

  # The interior gap loads no points; the final query is outside the catalog
  # and is skipped before loading. Deliberately query in reverse spatial order.
  read = reader_circles(c(205, 55, 105, 5, 1000), rep(5, 5), r = 4)
  metrics = c("z_mean", "z_min", "z_max", "z_sum", "z_p95", "count", "z_count")
  strategies = list(sequential())
  if (has_omp_support()) strategies = c(strategies, list(concurrent_files(2)))
  for (strategy in strategies)
  {
    set_parallel_strategy(strategy)
    pipeline = read + summarise(metrics = metrics) +
      delete_points("Classification == 7") + summarise(metrics = metrics)
    ans = exec(pipeline, on = file, buffer = 0, progress = FALSE)
    expect_equal(ans$summary$metrics$z_mean, c(22, NA_real_, 12, 1))
    expect_equal(ans$summary$metrics$count, c(3, 0, 3, 3))
    expect_equal(ans$summary.1$metrics$z_mean, c(22, NA_real_, NA_real_, 1))
    expect_equal(ans$summary.1$metrics$z_min, c(20, NA_real_, NA_real_, 0))
    expect_equal(ans$summary.1$metrics$count, c(3, 0, 0, 3))
    expect_equal(ans$summary.1$metrics$z_count, c(3, 0, 0, 3))
    expect_true(all(is.na(ans$summary.1$metrics[2:3, setdiff(metrics, c("count", "z_count"))])))
    expect_equal(ans$summary$npoints, 9)
    expect_equal(ans$summary.1$npoints, 6)

    selected = exec(read + summarise(metrics = metrics, filter = "Classification != 7"),
                    on = file, buffer = 0, progress = FALSE)
    expect_equal(selected$metrics, ans$summary.1$metrics)

    filtered_read = reader_circles(c(205, 55, 105, 5), rep(5, 4), r = 4,
                                   filter = "Classification != 7")
    filtered = exec(filtered_read + summarise(metrics = metrics), on = file,
                    buffer = 0, progress = FALSE)
    expect_equal(filtered$metrics, ans$summary.1$metrics)

    empty = exec(read + delete_points("Z >= 0") + summarise(metrics = metrics) +
                   summarise(metrics = metrics), on = file, buffer = 0,
                   progress = FALSE)
    expect_equal(empty$summary$metrics, empty$summary.1$metrics)
    expect_equal(empty$summary$metrics$count, rep(0, 4))
    expect_equal(empty$summary$npoints, 0)
    expect_true(all(is.na(empty$summary$metrics[setdiff(metrics, c("count", "z_count"))])))

    stopped = exec(read + summarise(metrics = "z_mean") +
                     delete_points("Classification == 7") +
                     lasR:::stop_if_outside(0, 0, 10, 10) +
                     summarise(metrics = "z_mean"), on = file, buffer = 0,
                     progress = FALSE)
    expect_equal(stopped$summary$metrics$z_mean, c(22, NA_real_, 12, 1))
    expect_equal(stopped$summary.1$metrics$z_mean, 1)

    skipped = exec(summarise(metrics = metrics), on = file, noprocess = TRUE,
                    progress = FALSE)
    expect_equal(nrow(skipped$metrics), 0L)
    expect_equal(skipped$npoints, 0)
  }
})
