triangulation_reuse_cloud = function()
{
  # A sloping ground triangle, one canopy point and an outside point. The
  # interspersed order makes sorting and compaction move the ground vertices.
  cloud = data.frame(
    X = c(150, 100, 20, 0, 0),
    Y = c(150, 0, 20, 100, 0),
    Z = c(100, 20, 21, 30, 10),
    Classification = c(7L, 2L, 1L, 2L, 2L),
    PointSourceID = 1:5,
    ReturnNumber = 1L,
    NumberOfReturns = 1L,
    Terrain = c(100, 20, 16, 30, 10))
  attr(cloud, "crs") = sf::st_crs(32631)$wkt
  cloud
}

test_that("a triangulation can be reused after normalization compacts points",
{
  f = system.file("extdata", "Topography.las", package = "lasR")
  mesh = triangulate(20, filter = keep_ground())
  pipeline = mesh + hulls(mesh) + rasterize(10, mesh) +
    transform_with(mesh) + hulls(mesh) + rasterize(10, mesh)
  ans = exec(pipeline, on = f, progress = FALSE)

  expect_true(sf::st_equals(ans$hulls, ans$hulls.1, sparse = FALSE)[1, 1])
  expect_equal(as.numeric(ans$rasterize[]), as.numeric(ans$rasterize.1[]))
})

test_that("triangulations retain their source geometry and interpolation values",
{
  cloud = triangulation_reuse_cloud()
  translate = diag(4)
  translate[3, 4] = 100
  mutations = list(
    sort = sort_points(),
    delete = delete_points(keep_ground()),
    edit = edit_attribute(attribute = "Terrain", value = 999),
    remove = remove_attributes("Terrain"),
    translate = transform_with(translate))

  for (name in names(mutations))
  {
    attribute = if (name %in% c("edit", "remove")) "Terrain" else "Z"
    mesh = triangulate(filter = keep_ground(), use_attribute = attribute)
    pipeline = mesh + hulls(mesh) + rasterize(10, mesh) + mutations[[name]] +
      hulls(mesh) + rasterize(10, mesh) + transform_with(mesh) + write_las()
    ans = exec(pipeline, on = cloud, progress = FALSE)

    expect_true(sf::st_equals(ans$hulls, ans$hulls.1, sparse = FALSE)[1, 1],
                info = name)
    expect_equal(as.numeric(ans$rasterize[]), as.numeric(ans$rasterize.1[]),
                 info = name)

    points = read_las(ans$write_las)
    points = points[order(points$PointSourceID), ]
    ids = if (name == "delete") 3L else 2:5
    expected_z = ifelse(ids == 3L, 5, 0)
    if (name == "translate") expected_z = expected_z + 100
    expect_equal(points$PointSourceID, ids, info = name)
    expect_equal(points$Z, expected_z, info = name)
    expect_equal(points$X, cloud$X[ids], info = name)
    expect_equal(points$Y, cloud$Y[ids], info = name)
  }
})

test_that("a reused triangulation is isolated between chunks and workers",
{
  previous_strategy = get_parallel_strategy()
  on.exit({
    unset_parallel_strategy()
    set_parallel_strategy(previous_strategy)
  }, add = TRUE)
  unset_parallel_strategy()

  directory = tempfile()
  dir.create(directory)
  on.exit(unlink(directory, recursive = TRUE), add = TRUE)
  sources = vector("list", 3)
  files = file.path(directory, paste0("input", 1:3, ".las"))
  for (i in seq_along(files))
  {
    cloud = triangulation_reuse_cloud()
    cloud$X = cloud$X + 1000 * i
    cloud$Y = cloud$Y + 1000 * i
    cloud$Z = cloud$Z + 10 * i
    sources[[i]] = cloud
    exec(write_las(files[i]), on = cloud, progress = FALSE, ncores = sequential())
  }

  strategies = list(sequential())
  if (has_omp_support())
    strategies = c(strategies, list(concurrent_points(2), concurrent_files(2), nested(2, 2)))

  for (strategy in strategies)
  {
    mesh = triangulate(filter = keep_ground())
    pipeline = mesh + transform_with(mesh) + sort_points() +
      transform_with(mesh, "+") + write_las(file.path(directory, "restored_*.las"))
    ans = exec(pipeline, on = files, progress = FALSE, ncores = strategy)
    expect_length(ans, length(files))

    for (i in seq_along(files))
    {
      points = read_las(ans[i])
      points = points[order(points$PointSourceID), ]
      # The outside point is still removed; the supported points recover
      # their original elevations even after compaction and sorting.
      expect_equal(points$PointSourceID, 2:5)
      expect_equal(points$X, sources[[i]]$X[2:5])
      expect_equal(points$Y, sources[[i]]$Y[2:5])
      expect_equal(points$Z, sources[[i]]$Z[2:5])
    }
  }
})
