with_independent_file_fixture = function(code)
{
  previous <- as.list(LASROPTIONS)
  on.exit({
    rm(list = ls(LASROPTIONS), envir = LASROPTIONS)
    list2env(previous, envir = LASROPTIONS)
  }, add = TRUE)
  unset_exec_option()
  unset_parallel_strategy()

  root <- tempfile("independent-files-")
  inputs <- file.path(root, "inputs")
  dir.create(inputs, recursive = TRUE)
  on.exit(unlink(root, recursive = TRUE), add = TRUE)
  files <- file.path(inputs, c("survey_2023.las", "survey_2024.las"))
  points <- expand.grid(X = c(0, 1, 2), Y = c(0, 1, 2))
  points$Z <- 3
  points$Intensity <- seq_len(nrow(points))
  points$Classification <- 2L
  for (i in seq_along(files))
  {
    points$Z <- 3 + 100 * (i - 1)
    exec(set_crs(32617) + write_las(files[i]), points, ncores = sequential())
  }
  code(files, root)
}

test_that("independent summaries count each overlapping file once",
{
  with_independent_file_fixture(function(files, root)
  {
    pipeline <- summarise(metrics = c("count", "z_mean", "i_mean"))
    default <- exec(pipeline, files, ncores = sequential(), buffer = 0)
    expect_equal(default$npoints, 36)
    expect_equal(default$metrics$count, c(18, 18))

    strategies <- list(sequential())
    if (has_omp_support())
      strategies <- c(strategies, list(concurrent_files(2), concurrent_points(2), nested(2, 2)))
    for (strategy in strategies)
    {
      ans <- exec(pipeline, files,
                  with = list(independent_files = TRUE, ncores = strategy))
      expect_equal(ans$npoints, 18)
      expect_equal(ans$metrics$count, c(9, 9))
      expect_equal(ans$metrics$z_mean, c(3, 103))
      expect_equal(ans$metrics$i_mean, c(5, 5))
    }

    explicit_default <- exec(pipeline, files, independent_files = FALSE, ncores = sequential())
    expect_equal(explicit_default, default)
  })
})

test_that("independent LAS and LAZ writers preserve each acquisition",
{
  with_independent_file_fixture(function(files, root)
  {
    strategies <- list(sequential())
    if (has_omp_support()) strategies <- c(strategies, list(concurrent_files(2)))
    for (strategy in strategies)
    for (loaded in c(FALSE, TRUE))
    for (extension in c("las", "laz"))
    {
      output <- tempfile(tmpdir = root)
      dir.create(output)
      writer <- write_las(file.path(output, paste0("*.", extension)), keep_buffer = TRUE)
      pipeline <- if (loaded) triangulate() + writer else writer
      log <- tempfile(tmpdir = root)
      ans <- exec(pipeline, files, with = list(independent_files = TRUE,
                  ncores = strategy, log_file = log))
      expect_equal(sort(basename(ans)), sub("las$", extension, basename(files)))
      expect_true(all(file.exists(ans)))
      expect_match(paste(readLines(log), collapse = "\n"), "Buffer: 0.0", fixed = TRUE)
      if (identical(attr(strategy, "strategy"), "concurrent-files"))
        expect_match(paste(readLines(log), collapse = "\n"), "Concurrent files: 2", fixed = TRUE)
      for (i in seq_along(files))
      {
        original <- exec(callback(identity, expose = "xyzi", no_las_update = TRUE),
                         files[i], ncores = sequential())
        rewritten <- exec(callback(identity, expose = "xyzi", no_las_update = TRUE),
                          file.path(output, sub("las$", extension, basename(files[i]))),
                          ncores = sequential())
        expect_equal(rewritten, original)
        expect_equal(nrow(rewritten), 9)
      }
    }

    merged <- file.path(root, "merged.las")
    exec(write_las(merged), files, independent_files = TRUE, ncores = sequential())
    expect_equal(exec(summarise(), merged, ncores = sequential())$npoints, 18)
  })
})

test_that("directories and virtual point clouds expand into independent jobs",
{
  with_independent_file_fixture(function(files, root)
  {
    vpc <- file.path(root, "surveys.vpc")
    exec(write_vpc(vpc), files, ncores = sequential())
    for (input in c(dirname(files[1]), vpc))
    {
      ans <- exec(summarise(metrics = "z_mean"), input,
                  independent_files = TRUE, ncores = sequential())
      expect_equal(ans$npoints, 18)
      expect_equal(ans$metrics$z_mean, c(3, 103))
    }
  })
})

test_that("independent jobs respect filters and skipped files",
{
  with_independent_file_fixture(function(files, root)
  {
    pipeline <- reader_coverage(filter = "-keep_intensity 5 9") + summarise()
    ans <- exec(pipeline, files, independent_files = TRUE, ncores = sequential())
    expect_equal(ans$npoints, 10)

    ans <- exec(summarise(), files, noprocess = c(FALSE, TRUE),
                independent_files = TRUE, ncores = sequential())
    expect_equal(ans$npoints, 9)
    expect_equal(unname(ans$z_histogram), 9)

    ans <- exec(summarise(), files, noprocess = c(TRUE, TRUE),
                independent_files = TRUE, ncores = sequential())
    expect_equal(ans$npoints, 0)
  })
})

test_that("independent file processing also reads overlapping LAZ and PCD inputs",
{
  with_independent_file_fixture(function(files, root)
  {
    for (extension in c("laz", "pcd"))
    {
      output <- file.path(root, paste0("*.", extension))
      writer <- if (extension == "pcd") write_pcd(output) else write_las(output)
      converted <- exec(writer, files, independent_files = TRUE, ncores = sequential())
      ans <- exec(summarise(metrics = "z_mean"), converted,
                  independent_files = TRUE, ncores = sequential())
      expect_equal(ans$npoints, 18)
      expect_equal(ans$metrics$z_mean, c(3, 103))
    }
  })
})

test_that("LAScatalog spatial options are validated before independent processing",
{
  skip_if_not_installed("lidR")
  with_independent_file_fixture(function(files, root)
  {
    ctg <- eval(parse(text = "lidR::readLAScatalog(files)"))
    ctg@processing_options$progress <- FALSE
    ctg@chunk_options$buffer <- 0
    ctg@chunk_options$size <- 0
    expect_equal(exec(summarise(), ctg, independent_files = TRUE)$npoints, 18)

    ctg@chunk_options$buffer <- 5
    expect_error(exec(summarise(), ctg, independent_files = TRUE, buffer = 0),
                 "independent_files requires buffer = 0 and chunk = 0", fixed = TRUE)
    ctg@chunk_options$buffer <- 0
    ctg@chunk_options$size <- 5
    expect_error(exec(summarise(), ctg, independent_files = TRUE, chunk = 0),
                 "independent_files requires buffer = 0 and chunk = 0", fixed = TRUE)
  })
})

test_that("independent file options retain normal precedence and can be unset",
{
  with_independent_file_fixture(function(files, root)
  {
    ans <- exec(summarise(), files, independent_files = FALSE,
                with = list(independent_files = TRUE, ncores = sequential()))
    expect_equal(ans$npoints, 18)
    set_exec_options(independent_files = TRUE, ncores = sequential())
    ans <- exec(summarise(), files, with = list(independent_files = FALSE))
    expect_equal(ans$npoints, 18)
    set_exec_options(independent_files = FALSE, ncores = sequential())
    ans <- exec(summarise(), files, independent_files = TRUE)
    expect_equal(ans$npoints, 36)
    unset_exec_option()
    expect_null(LASROPTIONS$independent_files)
    expect_equal(exec(summarise(), files)$npoints, 36)
  })
})

test_that("independent file mode rejects incompatible inputs and options",
{
  with_independent_file_fixture(function(files, root)
  {
    for (value in c(-1, 0.5, 10))
    {
      expect_error(exec(summarise(), files, independent_files = TRUE, buffer = value),
                   "independent_files requires buffer = 0 and chunk = 0", fixed = TRUE)
      expect_error(exec(summarise(), files, independent_files = TRUE, chunk = value),
                   "independent_files requires buffer = 0 and chunk = 0", fixed = TRUE)
    }
    for (value in list(1, NA, logical(), c(TRUE, FALSE)))
    {
      expect_error(exec(summarise(), files, independent_files = value), "independent_files")
      expect_error(set_exec_options(independent_files = value), "independent_files")
    }
    queries <- list(reader_circles(1, 1, 1), reader_rectangles(0, 0, 2, 2))
    output <- file.path(root, "invalid.las")
    for (query in queries)
    {
      expect_error(exec(query + write_las(output), files, independent_files = TRUE),
                   "independent_files cannot be combined with spatial-query readers", fixed = TRUE)
      expect_false(file.exists(output))
    }
    points <- data.frame(X = 0:2, Y = 0:2, Z = 0:2)
    expect_error(exec(summarise(), points, independent_files = TRUE),
                 "independent_files requires file input", fixed = TRUE)
  })
})
