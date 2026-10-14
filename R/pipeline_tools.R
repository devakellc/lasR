#' Tools inherited from base R
#'
#' Tools inherited from base R
#'
#' @param x,e1,e2 lasR objects
#' @param i index
#' @param ... lasR objects. Is equivalent to +
#' @examples
#' algo1 <- rasterize(1, "max")
#' algo2 <- rasterize(4, "min")
#' print(algo1)
#' pipeline <- algo1 + algo2
#' print(pipeline)
#' @name tools
#' @rdname tools
NULL

#' @rdname tools
#' @export
print.PipelinePtr <- function(x, ...)
{
  .APIOPERATIONS$print_pipeline(x)
}

#' @export
#' @method print PipelinePtr
print.PipelinePtr <- function(x, ...)
{
  .APIOPERATIONS$print_pipeline(x)
}

#' @rdname tools
#' @export
`+.PipelinePtr` <- function(e1, e2)
{
  if (!methods::is(e1, "PipelinePtr") || !methods::is(e2, "PipelinePtr"))
    stop("Both operands must be of class PipelinePtr") # nocov

  ans = .APIOPERATIONS$merge_pipeline(e1, e2);

  # Carry over SEXPs that callback/aggregate stages reference by raw address
  # (func/args/call/env). The C++ pipeline stores their addresses in the JSON
  # config, so the underlying R objects must outlive every '+' to avoid GC
  # reusing the slot.
  preserved <- c(attr(e1, "preserved"), attr(e2, "preserved"))
  for (key in c("func", "args", "call", "env"))
  {
    v1 <- attr(e1, key); if (!is.null(v1)) preserved <- c(preserved, list(v1))
    v2 <- attr(e2, key); if (!is.null(v2)) preserved <- c(preserved, list(v2))
  }
  if (length(preserved) > 0) attr(ans, "preserved") <- preserved

  return(ans)
}

#' @rdname tools
#' @export
`[[.PipelinePtr` <- function(x, i, ...) {
  # Example: Access an element by name or index
  if (is.character(i)) {
    stop("Extraction by name not supported")
  } else if (is.numeric(i)) {
    return(.APIOPERATIONS$get_stage_by_index(x, i-1))
  } else {
    stop("Index must be character or numeric")
  }
}

get_pipeline_info = function(pipeline)
{
  .APIOPERATIONS$get_pipeline_info(pipeline)
}

is_indexed = function(files)
{
  ans = logical(length(files))
  for (i in seq_along(files))
  {
    file = files[i]
    indexed = .APIUTILS$is_indexed(file)
    ans[i] = indexed
  }

  return (ans)
}

address = function(x)
{
  .APIOPERATIONS$get_address(x)
}

#' Inspect and validate a pipeline
#'
#' Describe a pipeline and check its configuration without processing points or
#' creating output, index, log, or temporary JSON files. Both functions return
#' the same structured report. `validate_pipeline()` can optionally stop when
#' the report contains an error.
#'
#' @param pipeline A `PipelinePtr` created by composing lasR stages.
#' @param on Optional input accepted by [exec], including file paths, a
#'   `data.frame`, or a `LAS`, `LAScatalog`, or `lasrcloud` object. `NULL` checks
#'   the pipeline without input metadata.
#' @param with A list of processing options, as in [exec].
#' @param ... Additional processing options, as in [exec].
#' @param stop_on_error A single logical value. If `TRUE`, stop when the report
#'   has status `"invalid"`. An incomplete report does not raise an error.
#' @param x A report returned by `pipeline_plan()` or `validate_pipeline()`.
#'
#' @return A list of class `lasr_pipeline_plan`, with schema version 1. `status`
#'   is `"valid"`, `"invalid"`, or `"incomplete"`; `valid` is respectively
#'   `TRUE`, `FALSE`, or `NULL`. `scope` identifies pipeline-only or input-aware
#'   inspection. The report contains `diagnostics`, `stages`, `capabilities`,
#'   `processing`, `inputs`, `chunks`, and `outputs`. Diagnostics have a
#'   severity, code, and message, with stage or input details where available.
#'   JSON arrays are represented as lists. Fields that cannot yet be determined
#'   are `NULL`, including chunks when input bounds are unavailable.
#'
#' @details
#' Options use the same precedence as [exec]: global settings, `LAScatalog`
#' settings, `with`, then `...`. The report distinguishes requested settings
#' from effective settings, including required buffers and parallelism limits.
#'
#' Inspection reads available input metadata but does not process point payloads
#' or invoke callbacks. Checks that require unavailable metadata or execution
#' are reported as deferred. A valid report confirms only the checks performed;
#' it does not guarantee that processing will succeed. Planning neither changes
#' the supplied pipeline nor creates its outputs.
#'
#' Positive chunk sizes are unsupported for in-memory inputs (`data.frame`,
#' `LAS`, and `lasrcloud`). Use file inputs for chunked execution.
#'
#' @seealso [exec], [set_exec_options]
#' @examples
#' pipeline <- triangulate() + rasterize(1)
#' pipeline_plan(pipeline)
#'
#' input <- system.file("extdata", "Example.las", package = "lasR")
#' report <- validate_pipeline(pipeline, on = input)
#' report$status
#' @name pipeline_plan
#' @md
#' @export
pipeline_plan = function(pipeline, on = NULL, with = list(), ...)
{
  if (!methods::is(pipeline, "PipelinePtr"))
    stop("'pipeline' must be a PipelinePtr object")

  report <- tryCatch(
    {
      prepared <- prepare_pipeline(pipeline, on, with, ..., allow_no_input = TRUE)
      .APIOPERATIONS$pipeline_plan(prepared$pipeline, prepared$on, prepared$options)
    },
    error = function(error)
    {
      list(
        schema_version = 1L,
        status = "invalid",
        valid = FALSE,
        scope = if (is.null(on)) "pipeline" else "inputs",
        diagnostics = list(list(
          severity = "error",
          code = "invalid_configuration",
          message = conditionMessage(error)
        )),
        stages = list(),
        capabilities = setNames(list(), character()),
        processing = setNames(list(), character()),
        inputs = list(),
        chunks = NULL,
        outputs = list()
      )
    }
  )
  class(report) <- c("lasr_pipeline_plan", "list")
  report
}

#' @rdname pipeline_plan
#' @export
validate_pipeline = function(pipeline, on = NULL, with = list(), ..., stop_on_error = FALSE)
{
  if (!is.logical(stop_on_error) || length(stop_on_error) != 1L || is.na(stop_on_error))
    stop("'stop_on_error' must be TRUE or FALSE")

  report <- pipeline_plan(pipeline, on, with, ...)
  if (stop_on_error && identical(report$status, "invalid"))
  {
    errors <- Filter(function(x) identical(x$severity, "error"), report$diagnostics)
    messages <- vapply(errors, function(x) x$message, character(1))
    if (length(messages) == 0L) messages <- "Invalid pipeline"
    stop(paste(messages, collapse = "\n"), call. = FALSE)
  }
  report
}

#' @rdname pipeline_plan
#' @export
print.lasr_pipeline_plan = function(x, ...)
{
  value <- function(x, fallback = "unknown")
  {
    if (is.null(x) || length(x) == 0L) return(fallback)
    paste(unlist(x, use.names = FALSE), collapse = ", ")
  }

  cat("lasR pipeline plan: ", value(x$status), "\n", sep = "")
  cat("Scope: ", value(x$scope), "\n", sep = "")
  if (!is.null(x$stages)) cat("Stages: ", length(x$stages), "\n", sep = "")
  cat("Chunks: ", if (is.null(x$chunks)) "unknown" else length(x$chunks), "\n", sep = "")

  effective <- x$processing$effective
  if (!is.null(effective))
  {
    cat("Effective buffer: ", value(effective$buffer), "\n", sep = "")
    cat("Concurrent files: ", value(effective$concurrent_files),
        "; concurrent points: ", value(effective$concurrent_points), "\n", sep = "")
  }

  for (diagnostic in x$diagnostics)
  {
    location <- if (is.null(diagnostic$stage_index)) "" else
      paste0(" stage ", value(diagnostic$stage_index))
    cat("[", value(diagnostic$severity), "]", location, ": ",
        value(diagnostic$message, ""), "\n", sep = "")
  }
  invisible(x)
}

