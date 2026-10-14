checkpoint_fixture <- function(n = 3L)
{
  root <- tempfile("checkpoint-")
  dir.create(root)
  dir.create(file.path(root, "input"))
  dir.create(file.path(root, "output"))
  files <- file.path(root, "input", paste0("tile", seq_len(n), ".las"))
  source <- system.file("extdata", "Example.las", package = "lasR")
  for (file in files) file.copy(source, file)
  list(root = root, files = files, state = file.path(root, "state"),
       output = file.path(root, "output", "*.las"))
}

checkpoint_exec <- function(pipeline, f, resume = FALSE, ...)
{
  exec(pipeline, on = f$files, checkpoint = f$state, resume = resume,
       ncores = sequential(), noread = TRUE, ...)
}

test_that("a rebuilt pipeline resumes with complete ordered file results", {
  f <- checkpoint_fixture()
  on.exit(unlink(f$root, recursive = TRUE))
  first <- checkpoint_exec(write_las(f$output), f)
  expect_equal(basename(first), paste0("tile", 1:3, ".las"))
  stamps <- file.info(first)$mtime
  expect_length(list.files(file.path(f$state, "completed")), 3L)
  # Constructors generate new UIDs: these must not invalidate a resume.
  resumed <- checkpoint_exec(write_las(f$output), f, TRUE)
  expect_identical(resumed, first)
  expect_identical(file.info(first)$mtime, stamps)
  expect_error(checkpoint_exec(write_las(f$output), f), "already exists")
})

test_that("missing and modified outputs are recomputed, intact ones are reused", {
  f <- checkpoint_fixture()
  on.exit(unlink(f$root, recursive = TRUE))
  first <- checkpoint_exec(write_las(f$output), f)
  expected <- unname(tools::md5sum(first))
  untouched <- file.info(first[1])$mtime
  unlink(first[2])
  writeBin(as.raw(1:20), first[3])
  resumed <- checkpoint_exec(write_las(f$output), f, TRUE)
  expect_identical(resumed, first)
  expect_equal(unname(tools::md5sum(resumed)), expected)
  expect_identical(file.info(first[1])$mtime, untouched)
})

test_that("changed input contents and pipeline settings cannot reuse a job", {
  f <- checkpoint_fixture()
  on.exit(unlink(f$root, recursive = TRUE))
  first <- checkpoint_exec(write_las(f$output), f)
  original <- tools::md5sum(first)
  expect_error(checkpoint_exec(reader("Z > 1") + write_las(f$output), f, TRUE),
               "inputs, pipeline, chunk plan or processing settings changed", fixed = TRUE)
  stamp <- file.info(f$files[1])$mtime
  size <- file.info(f$files[1])$size
  con <- file(f$files[1], "r+b")
  seek(con, size - 1, rw = "write")
  writeBin(as.raw(17), con)
  close(con)
  Sys.setFileTime(f$files[1], stamp)
  expect_error(checkpoint_exec(write_las(f$output), f, TRUE), "inputs, pipeline")
  expect_identical(tools::md5sum(first), original)
})

test_that("unsupported pipelines fail before creating any output", {
  f <- checkpoint_fixture()
  on.exit(unlink(f$root, recursive = TRUE))
  merged <- file.path(f$root, "merged.tif")
  expect_error(checkpoint_exec(rasterize(1, "max", ofile = merged), f), "every output needs")
  expect_false(file.exists(merged))
  expect_error(checkpoint_exec(summarise() + write_las(f$output), f), "summarise.*not supported")
  expect_error(checkpoint_exec(callback(function(x) x) + write_las(f$output), f), "callback.*not supported")
  expect_error(checkpoint_exec(local_maximum(3) + write_las(f$output), f), "local_maximum.*not supported")
  expect_error(checkpoint_exec(sampling_voxel(1) + write_las(f$output), f), "sampling_voxel.*not supported")
  expect_error(checkpoint_exec(write_las(f$output) + write_las(f$output), f), "output collision")
  expect_false(dir.exists(f$state))
  expect_error(exec(write_las(f$output), on = f$files, resume = TRUE), "requires a checkpoint")
})

test_that("input files and pre-existing outputs are never overwritten on a fresh run", {
  f <- checkpoint_fixture()
  on.exit(unlink(f$root, recursive = TRUE))
  original <- tools::md5sum(f$files)
  expect_error(checkpoint_exec(write_las(file.path(f$root, "input", "*.las")), f), "collides with an input")
  expect_identical(tools::md5sum(f$files), original)
  occupied <- file.path(f$root, "output", "tile2.las")
  writeLines("user data", occupied)
  expect_error(checkpoint_exec(write_las(f$output), f), "output already exists")
  expect_equal(readLines(occupied), "user data")
  expect_false(file.exists(file.path(f$root, "output", "tile1.las")))
})

test_that("empty point chunks have durable receipts without invented files", {
  f <- checkpoint_fixture()
  on.exit(unlink(f$root, recursive = TRUE))
  pipeline <- reader("Z > 100000") + write_las(f$output)
  expect_null(checkpoint_exec(pipeline, f))
  expect_length(list.files(file.path(f$state, "completed")), 3L)
  expect_null(checkpoint_exec(pipeline, f, TRUE))
  expect_length(list.files(file.path(f$root, "output")), 0L)
})

test_that("GeoTIFFs and multiple writers are closed and restored across resumes", {
  skip_if_not_installed("terra")
  f <- checkpoint_fixture()
  on.exit(unlink(f$root, recursive = TRUE))
  raster <- file.path(f$root, "output", "*_height.tif")
  make_pipeline <- function() rasterize(2, "max", ofile = raster) + write_las(f$output)
  first <- checkpoint_exec(make_pipeline(), f)
  expect_length(first$rasterize, 3L)
  expect_length(first$write_las, 3L)
  values <- lapply(first$rasterize, function(path) terra::values(terra::rast(path)))
  resumed <- checkpoint_exec(make_pipeline(), f, TRUE)
  expect_identical(resumed, first)
  expect_equal(lapply(resumed$rasterize, function(path) terra::values(terra::rast(path))), values)
})

test_that("concurrent chunks return the same paths and point contents on resume", {
  skip_if_not(has_omp_support())
  f <- checkpoint_fixture(5L)
  on.exit(unlink(f$root, recursive = TRUE))
  run <- function(resume) exec(write_las(f$output), on = f$files,
    checkpoint = f$state, resume = resume, ncores = concurrent_files(2), noread = TRUE)
  first <- run(FALSE)
  expect_equal(basename(first), paste0("tile", 1:5, ".las"))
  checksums <- tools::md5sum(first)
  unlink(first[3])
  expect_identical(run(TRUE), first)
  expect_identical(tools::md5sum(first), checksums)
})

test_that("malformed completion metadata fails without overwriting outputs", {
  f <- checkpoint_fixture()
  on.exit(unlink(f$root, recursive = TRUE))
  first <- checkpoint_exec(write_las(f$output), f)
  original <- tools::md5sum(first)
  writeLines("broken", file.path(f$state, "completed", "0.json"))
  expect_error(checkpoint_exec(write_las(f$output), f, TRUE), "invalid or unreadable metadata")
  expect_identical(tools::md5sum(first), original)
})

test_that("logs cannot truncate an input through a hard-link alias", {
  f <- checkpoint_fixture(1L)
  on.exit(unlink(f$root, recursive = TRUE))
  alias <- file.path(f$root, "progress.txt")
  skip_if_not(suppressWarnings(file.link(f$files[1], alias)))
  original <- tools::md5sum(f$files)
  expect_error(checkpoint_exec(write_las(f$output), f, progress_file = alias),
               "log path collides")
  expect_identical(tools::md5sum(f$files), original)
  expect_false(dir.exists(f$state))
})

test_that("folder inputs cannot grow from the job's own outputs", {
  f <- checkpoint_fixture()
  on.exit(unlink(f$root, recursive = TRUE))
  folder <- dirname(f$files[1])
  expect_error(exec(write_las(file.path(folder, "*_processed.las")), on = folder,
                    checkpoint = f$state), "outside input directories")
  expect_false(dir.exists(f$state))
})

test_that("missing checkpoint metadata fails before rewriting outputs", {
  f <- checkpoint_fixture(1L)
  on.exit(unlink(f$root, recursive = TRUE))
  first <- checkpoint_exec(write_las(f$output), f)
  stamp <- file.info(first)$mtime
  unlink(file.path(f$state, "completed"), recursive = TRUE)
  expect_error(checkpoint_exec(write_las(f$output), f, TRUE), "missing completion directory")
  expect_identical(file.info(first)$mtime, stamp)
})

test_that("buffered queries and connected stages match uninterrupted execution", {
  skip_if_not_installed("terra")
  f <- checkpoint_fixture(1L)
  on.exit(unlink(f$root, recursive = TRUE))
  ground <- expand.grid(X = seq(-2, 10, 0.5), Y = seq(-2, 10, 0.5))
  ground$Z <- 10
  ground$Classification <- 2L
  canopy <- ground
  canopy$Z <- canopy$Z + 10
  canopy$Classification <- 5L
  exec(write_las(f$files[1]), on = rbind(ground, canopy))
  reference <- file.path(f$root, "reference")
  dir.create(reference)
  make_pipeline <- function(folder) {
    terrain <- rasterize(1, "min", ofile = "")
    reader_rectangles(c(0, 4), c(0, 0), c(4, 8), c(8, 8)) + terrain +
      transform_with(terrain, bilinear = FALSE) +
      rasterize(1, "max", ofile = file.path(folder, "*.tif")) +
      write_las(file.path(folder, "*.las"))
  }
  expected <- exec(make_pipeline(reference), on = f$files, buffer = 1, noread = TRUE)
  run <- function(resume = FALSE) exec(make_pipeline(dirname(f$output)), on = f$files,
    buffer = 1, checkpoint = f$state, resume = resume, noread = TRUE)
  actual <- run()
  expect_length(actual$write_las, 2L)
  raster_values <- function(paths) lapply(paths, function(p) terra::values(terra::rast(p)))
  expect_equal(raster_values(actual$rasterize), raster_values(expected$rasterize))
  get_points <- function(paths) lapply(paths, function(p)
    exec(callback(function(data) data, no_las_update = TRUE), on = p))
  expect_equal(get_points(actual$write_las), get_points(expected$write_las))
  # Losing one writer invalidates the whole chunk, including its other output.
  unlink(actual$rasterize[2])
  expect_identical(run(TRUE), actual)
  expect_equal(raster_values(actual$rasterize), raster_values(expected$rasterize))
  expect_equal(get_points(actual$write_las), get_points(expected$write_las))
})
