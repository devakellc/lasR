#include "multichm.h"
#include "NA.h"
#include "openmp.h"

#include <algorithm>
#include <chrono>
#include <cmath>

// 95th percentile of the n first values of a sorted run. Same convention as MetricManager::percentile
static float percentile95(const float* x, uint32_t n)
{
  float rank = 0.95f * (float)(n - 1) + 1.0f;
  uint32_t i = (uint32_t)rank;
  float frac = rank - (float)i;
  float lo = x[i - 1];
  float hi = (frac > 0.0f) ? x[i] : lo;
  return lo + (hi - lo) * frac;
}

// The window is circular and its size is fixed. Compute the offsets once
static std::vector<std::pair<int,int>> build_window(double ws, double res)
{
  double hws = ws/2;
  int radius = (int)std::floor(hws/res);
  std::vector<std::pair<int,int>> window;

  for (int dr = -radius ; dr <= radius ; dr++)
  {
    for (int dc = -radius ; dc <= radius ; dc++)
    {
      if (dr == 0 && dc == 0) continue;
      if ((dc*dc + dr*dr)*res*res <= hws*hws) window.emplace_back(dr, dc);
    }
  }

  return window;
}

LASRmultichm::LASRmultichm()
{
  res = 1.0;
  ws = 3.0;
  min_height = 2.0;
  layer_thickness = 0.5;
  dist_2d = 3.0;
  dist_3d = 5.0;
  use_max = false;

  counter = std::make_shared<unsigned int>(0);
  unicity_table = std::make_shared<std::unordered_map<uint64_t, unsigned int>>();
}

bool LASRmultichm::set_parameters(const nlohmann::json& stage)
{
  ws = stage.at("ws");
  res = stage.value("res", 1.0);
  min_height = stage.value("min_height", 2.0);
  layer_thickness = stage.value("layer_thickness", 0.5);
  dist_2d = stage.value("dist_2d", 3.0);
  dist_3d = stage.value("dist_3d", 5.0);
  use_max = stage.value("use_max", false);

  if (res <= 0 || ws <= 0 || layer_thickness <= 0 || dist_2d <= 0 || dist_3d <= 0)
  {
    last_error = "res, ws, layer_thickness, dist_2d and dist_3d must be positive";
    return false;
  }

  vector = Vector(xmin, ymin, xmax, ymax);
  vector.set_geometry_type(wkbPoint25D);

  return true;
}

bool LASRmultichm::process(PointCloud*& las)
{
  if (!las)
  {
    last_error = "Uninitialized pointer to LAS object"; // # nocov
    return false; // # nocov
  }

  auto start_time = std::chrono::high_resolution_clock::now();

  progress->reset();
  progress->set_total(las->npoints);
  progress->set_prefix("Multi CHM");
  progress->set_ncpu(ncpu);

  // Same grid as the rasters of the other stages (see Raster::set_chunk). It is enlarged by the
  // buffer to use the points read around the chunk
  int nbuffer = (int)std::ceil(buffer/res);

  Grid grid(ROUNDANY(xmin - 0.5*res, res) - nbuffer*res,
            ROUNDANY(ymin - 0.5*res, res) - nbuffer*res,
            ROUNDANY(xmax - 0.5*res, res) + res + nbuffer*res,
            ROUNDANY(ymax - 0.5*res, res) + res + nbuffer*res, res);

  window = build_window(ws, res);
  if (!build_index(las, grid)) return false;

  std::vector<Maximum> maxima;
  size_t alive = 0;
  float zmax = update_chm(false, alive);

  while (!active.empty() && zmax > min_height)
  {
    if (progress->interrupted()) break;

    find_maxima(grid, maxima);
    zmax = update_chm(true, alive);

    progress->update(las->npoints - alive);
    progress->show();
  }

  select_trees(maxima);
  record(maxima, las->header);

  progress->done();

  if (verbose)
  {
    // # nocov start
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    print("  Multi CHM took %.2f sec. %lu trees\n", (float)duration.count()/1000.0f, lm.size());
    // # nocov end
  }

  return true;
}

// Group the heights by cell and sort each group. This is the only pass over the points
bool LASRmultichm::build_index(PointCloud* las, const Grid& grid)
{
  int ncells = grid.get_ncells();

  if (las->npoints > (size_t)UINT32_MAX)
  {
    last_error = "too many points for a single chunk"; // # nocov
    return false; // # nocov
  }

  chm.assign(ncells, NA_F32_RASTER);
  has_lm.assign(ncells, 0);
  k.assign(ncells, 0);
  offset.assign(ncells+1, 0);

  Point p;
  p.set_schema(&las->header->schema);

  for (size_t i = 0 ; i < las->npoints ; i++)
  {
    if (!las->get_point(i, &p, &pointfilter)) continue;
    int cell = grid.cell_from_xy(p.get_x(), p.get_y());
    if (cell >= 0) offset[cell+1]++;
  }

  for (int c = 0 ; c < ncells ; c++) offset[c+1] += offset[c];

  std::vector<uint32_t> cursor(offset.begin(), offset.end()-1);
  z.resize(offset[ncells]);

  for (size_t i = 0 ; i < las->npoints ; i++)
  {
    if (!las->get_point(i, &p, &pointfilter)) continue;
    int cell = grid.cell_from_xy(p.get_x(), p.get_y());
    if (cell >= 0) z[cursor[cell]++] = (float)p.get_z();
  }

  #pragma omp parallel for num_threads(ncpu)
  for (int c = 0 ; c < ncells ; c++)
  {
    k[c] = offset[c+1] - offset[c];
    if (k[c] > 1) std::sort(z.begin()+offset[c], z.begin()+offset[c+1]);
  }

  active.clear();
  for (int c = 0 ; c < ncells ; c++) if (k[c] > 0) active.push_back(c);

  return true;
}

// Peel the band below the current CHM and rebuild the CHM of the cells that remain. A cell with
// nothing left above min_height cannot be a maximum nor suppress a neighbour. It is removed
float LASRmultichm::update_chm(bool peel, size_t& alive)
{
  float zmax = NA_F32_RASTER;
  size_t nalive = 0;
  size_t n = active.size();

  #pragma omp parallel for num_threads(ncpu)
  for (size_t a = 0 ; a < n ; a++)
  {
    int c = active[a];
    const float* run = z.data() + offset[c];

    if (peel)
    {
      float threshold = (float)(chm[c] - layer_thickness);
      k[c] = (uint32_t)(std::lower_bound(run, run + k[c], threshold) - run);
    }

    if (k[c] == 0 || run[k[c]-1] < min_height)
    {
      chm[c] = NA_F32_RASTER;
      continue;
    }

    chm[c] = use_max ? run[k[c]-1] : percentile95(run, k[c]);
  }

  size_t w = 0;
  for (size_t a = 0 ; a < n ; a++)
  {
    int c = active[a];
    if (chm[c] == NA_F32_RASTER) continue;
    if (chm[c] > zmax) zmax = chm[c];
    nalive += k[c];
    active[w++] = c;
  }
  active.resize(w);

  alive = nalive;

  return zmax;
}

// Local maximum filter on the CHM. A cell that already gave a maximum is skipped. Its CHM only
// decreases so the next ones would be duplicates.
// Sequential on purpose: a same-height neighbour only suppresses once it is itself confirmed
// (has_lm), matching lidRplugins::multichm(); a naive parallel pass lets a rejected neighbour
// suppress too and collapses a whole flat plateau to one point.
void LASRmultichm::find_maxima(const Grid& grid, std::vector<Maximum>& maxima)
{
  int ncols = grid.get_ncols();
  int nrows = grid.get_nrows();
  size_t n = active.size();

  for (size_t a = 0 ; a < n ; a++)
  {
    int c = active[a];
    if (has_lm[c]) continue;

    float v = chm[c];
    if (v < min_height) continue;

    int row = c/ncols;
    int col = c%ncols;
    bool is_max = true;

    for (size_t w = 0 ; w < window.size() && is_max ; w++)
    {
      int rr = row + window[w].first;
      int cc = col + window[w].second;
      if (rr < 0 || rr >= nrows || cc < 0 || cc >= ncols) continue;

      int j = rr*ncols + cc;
      float u = chm[j];

      if (u == NA_F32_RASTER) continue;
      if (u > v || (u == v && has_lm[j])) is_max = false;
    }

    if (is_max)
    {
      has_lm[c] = 1;
      maxima.push_back({grid.x_from_cell(c), grid.y_from_cell(c), (double)v});
    }
  }
}

// Sort the candidates by decreasing height and keep those far enough from the trees already
// retained. Ties are broken on the coordinates to get a reproducible order
void LASRmultichm::select_trees(std::vector<Maximum>& maxima) const
{
  double d2d = dist_2d*dist_2d;
  double d3d = dist_3d*dist_3d;

  std::sort(maxima.begin(), maxima.end(), [](const Maximum& a, const Maximum& b)
  {
    if (a.z != b.z) return a.z > b.z;
    if (a.x != b.x) return a.x < b.x;
    return a.y < b.y;
  });

  std::vector<Maximum> trees;

  for (const auto& m : maxima)
  {
    bool detected = true;

    for (const auto& t : trees)
    {
      double dx = m.x - t.x;
      double dy = m.y - t.y;
      double dz = m.z - t.z;
      double dd = dx*dx + dy*dy;

      if (dd < d2d || dd + dz*dz < d3d) { detected = false; break; }
    }

    if (detected) trees.push_back(m);
  }

  maxima.swap(trees);
}

void LASRmultichm::record(const std::vector<Maximum>& trees, const Header* header)
{
  lm.reserve(lm.size() + trees.size());

  #pragma omp critical(assign_multichm_ids)
  {
    for (const auto& t : trees)
    {
      PointLAS p;
      p.x = t.x;
      p.y = t.y;
      p.z = t.z;

      // A tree found twice in two overlapping chunks must get the same ID. The quantized
      // coordinates are a unique geographic key mapped to a 32 bit counter
      uint32_t X = (uint32_t)(int64_t)std::round((t.x - header->x_offset)/header->x_scale_factor);
      uint32_t Y = (uint32_t)(int64_t)std::round((t.y - header->y_offset)/header->y_scale_factor);
      uint64_t FID = ((uint64_t)X << 32) | (uint64_t)Y;

      if (unicity_table->count(FID) == 0)
      {
        (*unicity_table)[FID] = *counter;
        (*counter)++;
      }

      lm.push_back(p);
      lm.back().FID = (*unicity_table)[FID];
    }
  }
}

bool LASRmultichm::write()
{
  if (ofile.empty()) return true;
  if (lm.size() == 0) return true;

  bool success;
  #pragma omp critical (write_multichm)
  {
    success = vector.write(lm, false);
  }

  if (!success) return false;

  int dupfid = vector.get_dupfid();
  if (dupfid) print("%d points skipped with duplicated FID. This may be due to overlapping tiles or duplicated points.\n", dupfid);

  return true;
}

void LASRmultichm::clear(bool last)
{
  lm.clear();
}
