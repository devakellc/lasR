plan_fixture <- function(name = "Example.las")
{
  system.file("extdata", name, package = "lasR", mustWork = TRUE)
}

plan_directory <- function()
{
  path <- tempfile("lasr-plan-")
  dir.create(path)
  path
}

plan_diagnostics <- function(report, code = NULL, severity = NULL)
{
  Filter(function(x) {
    (is.null(code) || identical(x$code, code)) &&
      (is.null(severity) || identical(x$severity, severity))
  }, report$diagnostics)
}

expect_plan_error <- function(report, code)
{
  expect_identical(report$status, "invalid")
  expect_false(report$valid)
  expect_gt(length(plan_diagnostics(report, code, "error")), 0L)
}

test_that("plans expose checked stages, inputs, chunks and processing settings",
{
  pipeline <- summarise(metrics = c("z_max", "i_mean", "count"))
  report <- pipeline_plan(pipeline, on = plan_fixture(), ncores = sequential())

  expect_s3_class(report, "lasr_pipeline_plan")
  expect_equal(report$schema_version, 1)
  expect_identical(report$status, "valid")
  expect_true(report$valid)
  expect_identical(report$scope, "inputs")
  expect_type(report$inputs, "list")
  expect_length(report$inputs, 1L)
  expect_type(report$stages, "list")
  expect_type(report$outputs, "list")
  expect_length(report$chunks, 1L)
  expect_equal(report$chunks[[1]]$index, 1)
  expect_equal(report$chunks[[1]]$id, 0)
  expect_true(report$chunks[[1]]$process)
  expect_false(report$chunks[[1]]$empty)
  expect_length(report$chunks[[1]]$bounds, 4L)
  expect_true(all(c("requested", "effective") %in% names(report$processing)))
  for (settings in report$processing[c("requested", "effective")])
  {
    expect_true(all(c("buffer", "chunk", "strategy", "ncores",
                      "concurrent_files", "concurrent_points") %in% names(settings)))
  }
  expect_equal(report$processing$effective$concurrent_files, 1)
  expect_equal(report$processing$effective$concurrent_points, 1)
  expect_equal(validate_pipeline(pipeline, on = plan_fixture(),
                                 ncores = sequential()), report)
})

test_that("planning without inputs is incomplete and leaves the pipeline unchanged",
{
  pipeline <- summarise(metrics = "z_max")
  before <- capture.output(print(pipeline))
  first <- pipeline_plan(pipeline)
  second <- pipeline_plan(pipeline)

  expect_identical(first$status, "incomplete")
  expect_null(first$valid)
  expect_identical(first$scope, "pipeline")
  expect_null(first$chunks)
  expect_gt(length(plan_diagnostics(first, "missing_inputs", "deferred")), 0L)
  expect_equal(first, second)
  expect_identical(capture.output(print(pipeline)), before)
  expect_identical(validate_pipeline(pipeline, stop_on_error = TRUE)$status,
                   "incomplete")
})

test_that("missing metric, filter and edit attributes are diagnosed",
{
  pipelines <- list(
    summarise(metrics = "HAG_mean"),
    reader(filter = "HAG > 0") + summarise(),
    edit_attribute(attribute = "HAG", value = 1),
    rasterize(1, "HAG_mean")
  )
  for (pipeline in pipelines)
  {
    report <- pipeline_plan(pipeline, on = plan_fixture())
    expect_plan_error(report, "missing_attribute")
    diagnostics <- plan_diagnostics(report, "missing_attribute", "error")
    expect_true(any(vapply(diagnostics, function(x) identical(x$attribute, "HAG"),
                           logical(1))))
  }
  expect_error(validate_pipeline(pipelines[[1]], on = plan_fixture(),
                                 stop_on_error = TRUE), "HAG")
})

test_that("attribute availability follows add, use, remove and keep order",
{
  add <- add_extrabytes("double", "HAG", "Height above ground")
  use <- edit_attribute(attribute = "HAG", value = 7) +
    summarise(metrics = "HAG_mean")

  expect_true(pipeline_plan(add + use, on = plan_fixture())$valid)
  expect_plan_error(pipeline_plan(use + add, on = plan_fixture()),
                    "missing_attribute")
  expect_plan_error(pipeline_plan(add + remove_attribute("HAG") + use,
                                  on = plan_fixture()), "missing_attribute")
  expect_plan_error(pipeline_plan(keep_attributes("Intensity") +
                                  summarise(metrics = "Classification_sum"),
                                  on = plan_fixture()), "missing_attribute")
  expect_true(pipeline_plan(keep_attributes("Intensity") +
                            summarise(metrics = "z_max"), on = plan_fixture())$valid)
  expect_plan_error(pipeline_plan(remove_attribute("Withheld") +
                                  summarise(metrics = "Synthetic_sum"),
                                  on = plan_fixture()), "missing_attribute")
  expect_true(pipeline_plan(geometry_features(k = 3, features = "n") +
                            summarise(metrics = "normalZ_mean"),
                            on = plan_fixture())$valid)
  expect_plan_error(pipeline_plan(geometry_features(k = 3, features = "a") +
                                  summarise(metrics = "normalZ_mean"),
                                  on = plan_fixture()), "missing_attribute")
})

test_that("buffer adjustments are reported and incompatible PCD output is rejected",
{
  directory <- plan_directory()
  on.exit(unlink(directory, recursive = TRUE), add = TRUE)
  report <- pipeline_plan(triangulate() + write_las(file.path(directory, "*.las")),
                          on = plan_fixture(), buffer = 0, ncores = sequential())
  expect_true(report$valid)
  expect_equal(report$processing$requested$buffer, 0)
  expect_equal(report$processing$effective$buffer, 20)
  expect_equal(report$chunks[[1]]$buffer, 20)

  report <- pipeline_plan(triangulate() + write_pcd(file.path(directory, "*.pcd")),
                          on = plan_fixture(), buffer = 0)
  expect_plan_error(report, "pcd_buffer")
  expect_length(list.files(directory, all.files = TRUE, no.. = TRUE), 0L)
})

test_that("planning never creates or truncates writer, index or profile files",
{
  directory <- plan_directory()
  on.exit(unlink(directory, recursive = TRUE), add = TRUE)
  input <- file.path(directory, "input.las")
  expect_true(file.copy(plan_fixture(), input))
  existing <- file.path(directory, "existing.tif")
  writeBin(charToRaw("preserve this file"), existing)
  before <- tools::md5sum(c(input, existing))
  contents <- list.files(directory, all.files = TRUE, recursive = TRUE)
  pipeline <- rasterize(1, "max", ofile = existing) +
    local_maximum(3, ofile = file.path(directory, "tops.gpkg")) +
    write_las(file.path(directory, "result_*.las")) +
    write_lax() + write_vpc(file.path(directory, "catalog.vpc"))

  report <- pipeline_plan(pipeline, on = input,
                          profile_file = file.path(directory, "profile.json"),
                          log_file = file.path(directory, "log.json"),
                          progress_file = file.path(directory, "progress.txt"))
  expect_true(report$valid)
  expect_gt(length(plan_diagnostics(report, "output_exists", "warning")), 0L)
  expect_identical(tools::md5sum(c(input, existing)), before)
  expect_identical(list.files(directory, all.files = TRUE, recursive = TRUE), contents)
})

test_that("planning invokes neither callbacks nor custom raster expressions",
{
  called <- 0L
  fun <- function(data) { called <<- called + 1L; stop("callback executed") }
  pipeline <- callback(fun, no_las_update = TRUE) + summarise(metrics = "z_max")
  report <- pipeline_plan(pipeline, on = plan_fixture())
  expect_equal(called, 0L)
  expect_identical(report$status, "incomplete")
  expect_null(report$valid)
  expect_gt(length(plan_diagnostics(report, severity = "deferred")), 0L)

  # Construction evaluates an expression to infer its output width. Planning
  # must not evaluate it again after the stage has been constructed.
  metric <- function(x) { called <<- called + 1L; mean(x) }
  pipeline <- rasterize(1, metric(Z))
  called <- 0L
  report <- pipeline_plan(pipeline, on = plan_fixture())
  expect_equal(called, 0L)
  expect_identical(report$status, "incomplete")
})

test_that("PCD planning leaves the input directory free of bbox sidecars",
{
  directory <- plan_directory()
  on.exit(unlink(directory, recursive = TRUE), add = TRUE)
  input <- file.path(directory, "points.pcd")
  writeLines(c("VERSION .7", "FIELDS x y z", "SIZE 4 4 4", "TYPE F F F",
               "COUNT 1 1 1", "WIDTH 3", "HEIGHT 1", "VIEWPOINT 0 0 0 1 0 0 0",
               "POINTS 3", "DATA ascii", "0 0 1", "1 0 2", "0 1 3"), input)
  before <- tools::md5sum(input)
  report <- pipeline_plan(summarise(metrics = "z_max"), on = input)
  expect_true(report$status %in% c("valid", "incomplete"))
  expect_identical(list.files(directory), "points.pcd")
  expect_identical(tools::md5sum(input), before)
})

test_that("output collisions use normalized paths and protect input files",
{
  directory <- plan_directory()
  on.exit(unlink(directory, recursive = TRUE), add = TRUE)
  output <- file.path(directory, "shared.las")
  pipeline <- write_las(output) + write_las(file.path(directory, ".", "shared.las"))
  expect_plan_error(pipeline_plan(pipeline, on = plan_fixture()), "output_collision")
  before <- tools::md5sum(plan_fixture())
  expect_plan_error(pipeline_plan(write_las(plan_fixture()), on = plan_fixture()),
                    "output_overwrites_input")
  expect_identical(tools::md5sum(plan_fixture()), before)
  expect_false(file.exists(output))
  report <- pipeline_plan(hulls(ofile = file.path(directory, "tops.shp")),
                          on = plan_fixture(),
                          log_file = file.path(directory, "tops.dbf"))
  expect_plan_error(report, "output_collision")
  expect_length(list.files(directory, all.files = TRUE, no.. = TRUE), 0L)
})

test_that("raster local maxima use the raster schema without changing the cloud schema",
{
  raster <- rasterize(1, "max", ofile = "")
  invalid <- raster + local_maximum_raster(raster, 3,
                                          filter = "Classification == 2", ofile = "")
  expect_plan_error(pipeline_plan(invalid, on = plan_fixture()), "missing_attribute")
  valid <- raster + local_maximum_raster(raster, 3, filter = "Z > 0", ofile = "") +
    summarise(metrics = "Classification_sum")
  expect_true(pipeline_plan(valid, on = plan_fixture())$valid)
})

test_that("duplicate tile stems collide while one merged writer is allowed",
{
  directory <- plan_directory()
  on.exit(unlink(directory, recursive = TRUE), add = TRUE)
  paths <- file.path(directory, c("a", "b"))
  for (path in paths) dir.create(path)
  inputs <- file.path(paths, "tile.las")
  for (input in inputs) expect_true(file.copy(plan_fixture(), input))
  expect_plan_error(pipeline_plan(write_las(file.path(directory, "*.las")), on = inputs),
                    "output_collision")
  merged <- pipeline_plan(write_las(file.path(directory, "merged.las")), on = inputs)
  expect_true(merged$valid)
  expect_length(merged$chunks, 2L)
  expect_length(plan_diagnostics(merged, "output_collision"), 0L)
})

test_that("known bad processing options and missing output parents return reports",
{
  for (value in c(-1, NA_real_, NaN, Inf))
  {
    for (name in c("buffer", "chunk"))
    {
      options <- setNames(list(value), name)
      report <- pipeline_plan(summarise(), on = plan_fixture(), with = options)
      expect_identical(report$status, "invalid")
      expect_false(report$valid)
      expect_gt(length(plan_diagnostics(report, severity = "error")), 0L)
      expect_identical(validate_pipeline(summarise(), on = plan_fixture(),
                                         with = options)$status, "invalid")
    }
  }
  expect_identical(pipeline_plan(rasterize(0, "max"), on = plan_fixture())$status,
                   "invalid")
  for (pipeline in list(summarise(zwbin = 0), summarise(iwbin = 0),
                        classify_with_csf(cloth_resolution = 0)))
  {
    expect_identical(pipeline_plan(pipeline, on = plan_fixture())$status, "invalid")
  }
  directory <- plan_directory()
  on.exit(unlink(directory, recursive = TRUE), add = TRUE)
  output <- file.path(directory, "missing", "output.las")
  report <- pipeline_plan(write_las(output), on = plan_fixture())
  expect_identical(report$status, "invalid")
  expect_false(dir.exists(dirname(output)))
  report <- pipeline_plan(rasterize(1, "max", ofile = file.path(directory, "output.png")),
                          on = plan_fixture())
  expect_identical(report$status, "invalid")
  expect_false(file.exists(file.path(directory, "output.png")))
})

test_that("excessively fine chunk grids are deferred without allocating them",
{
  report <- pipeline_plan(summarise(), on = plan_fixture(), chunk = 0.0001)
  expect_identical(report$status, "incomplete")
  expect_null(report$valid)
  expect_null(report$chunks)
  expect_gt(length(plan_diagnostics(report, "plan_limit", "deferred")), 0L)
})

test_that("finite parameters that overflow buffers or query geometry are invalid",
{
  pipeline <- classify_with_csf(cloth_resolution = 1e308)
  expect_identical(pipeline_plan(pipeline, on = plan_fixture())$status, "invalid")
  expect_identical(pipeline_plan(pipeline)$status, "invalid")
  circle <- reader_circles(1e308, 1e308, 1e308) + summarise()
  expect_identical(pipeline_plan(circle, on = plan_fixture())$status, "invalid")
  rectangle <- reader_rectangles(1e308, 1e308, 1.1e308, 1.1e308) + summarise()
  expect_identical(pipeline_plan(rectangle, on = plan_fixture())$status, "invalid")
})

test_that("dataframe chunking is rejected and regular plans preserve the input",
{
  points <- expand.grid(X = seq(0.5, 7.5, by = 1), Y = seq(0.5, 7.5, by = 1))
  points$Z <- rep(1, nrow(points))
  before <- points
  pipeline <- summarise(metrics = "count")
  report <- pipeline_plan(pipeline, on = points, buffer = 25, chunk = 2,
                          ncores = sequential())
  expect_identical(report$status, "invalid")
  expect_false(report$valid)
  expect_error(exec(pipeline, on = points, buffer = 25, chunk = 2),
               "Chunking an in-memory input is not supported")
  report <- pipeline_plan(pipeline, on = points, buffer = 25,
                          ncores = sequential())
  expect_false(identical(report$status, "invalid"))
  expect_equal(report$processing$requested$buffer, 25)
  expect_equal(report$processing$effective$buffer, 25)
  expect_equal(report$processing$effective$chunk, 0)
  expect_length(report$chunks, 1L)
  expect_equal(report$chunks[[1]]$buffer, 0)
  expect_identical(points, before)
  result <- exec(pipeline, on = points, buffer = 25,
                 ncores = sequential())
  expect_equal(sum(result$metrics$count), nrow(points))
  expect_identical(points, before)
})

test_that("unsupported in-memory cloud chunking is rejected without changing the cloud",
{
  cloud <- read_cloud(plan_fixture(), progress = FALSE)
  pipeline <- summarise(metrics = c("count", "z_max"))
  before <- exec(pipeline, on = cloud, ncores = sequential())
  report <- pipeline_plan(pipeline, on = cloud, chunk = 2)
  expect_identical(report$status, "invalid")
  expect_false(report$valid)
  expect_error(exec(pipeline, on = cloud, chunk = 2),
               "Chunking an in-memory input is not supported")
  buffered <- pipeline_plan(pipeline, on = cloud, buffer = 25)
  expect_true(buffered$valid)
  expect_equal(buffered$processing$effective$buffer, 25)
  expect_equal(exec(pipeline, on = cloud, ncores = sequential()), before)
})

test_that("CRS diagnostics distinguish missing, incompatible and geographic inputs",
{
  missing <- pipeline_plan(summarise(metrics = "count"), on = plan_fixture("nocrs.las"))
  expect_gt(length(plan_diagnostics(missing, "missing_crs", "warning")), 0L)
  expect_true(missing$valid)
  mixed <- pipeline_plan(summarise(), on = c(plan_fixture(), plan_fixture("Topography.las")))
  expect_plan_error(mixed, "mixed_crs")

  directory <- plan_directory()
  on.exit(unlink(directory, recursive = TRUE), add = TRUE)
  geographic <- file.path(directory, "geographic.las")
  exec(set_crs(4326) + write_las(geographic), on = plan_fixture(), ncores = sequential())
  report <- pipeline_plan(rasterize(1, "max"), on = geographic)
  expect_gt(length(plan_diagnostics(report, "geographic_crs", "warning")), 0L)

  catalog <- file.path(directory, "catalog.vpc")
  missing <- pipeline_plan(write_vpc(catalog), on = plan_fixture("nocrs.las"))
  expect_identical(missing$status, "invalid")
  assigned <- pipeline_plan(set_crs(26917) + write_vpc(catalog),
                            on = plan_fixture("nocrs.las"))
  expect_true(assigned$valid)
  expect_false(file.exists(catalog))
})

test_that("each input schema is checked instead of trusting the first file",
{
  directory <- plan_directory()
  on.exit(unlink(directory, recursive = TRUE), add = TRUE)
  enriched <- file.path(directory, "enriched.las")
  exec(add_extrabytes("double", "HAG", "Height above ground") + write_las(enriched),
       on = plan_fixture(), ncores = sequential())
  for (inputs in list(c(enriched, plan_fixture()), c(plan_fixture(), enriched)))
  {
    report <- pipeline_plan(summarise(metrics = "HAG_mean"), on = inputs)
    expect_plan_error(report, "missing_attribute")
    diagnostics <- plan_diagnostics(report, "missing_attribute", "error")
    expect_true(any(vapply(diagnostics, function(x) {
      !is.null(x$input) && grepl("Example.las", x$input, fixed = TRUE)
    }, logical(1))))
    expect_true(pipeline_plan(summarise(metrics = "z_max"), on = inputs)$valid)
  }
  skipped <- pipeline_plan(summarise(metrics = "HAG_mean"),
                            on = c(enriched, plan_fixture("Topography.las")),
                            noprocess = c(FALSE, TRUE), buffer = 0)
  expect_true(skipped$valid)
  expect_true(skipped$chunks[[1]]$process)
  expect_false(skipped$chunks[[2]]$process)
  expect_false(skipped$inputs[[2]]$participates)
})

test_that("uninspected inputs and LASlib filters remain explicitly incomplete",
{
  remote <- pipeline_plan(summarise(), on = "https://example.invalid/points.copc.laz")
  expect_identical(remote$status, "incomplete")
  expect_null(remote$valid)
  expect_gt(length(plan_diagnostics(remote, severity = "deferred")), 0L)
  vpc <- pipeline_plan(summarise(metrics = "HAG_mean"), on = plan_fixture("bcts/bcts.vpc"))
  expect_identical(vpc$status, "incomplete")
  expect_null(vpc$valid)
  report <- pipeline_plan(reader(filter = "-keep_z_above 2") + summarise(),
                          on = plan_fixture())
  expect_identical(report$status, "incomplete")
  expect_null(report$valid)
})
