#include "multichm.h"
#include "NA.h"
#include "openmp.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>

// Constants of the depth gate. They mirror multichm_depth_lib.R of the tree top benchmark
static const double COLUMN_RADIUS = 1.0;  // smallest horizontal radius of a candidate's column
static const double COLUMN_FIRST  = 8.0;  // first returns the column holds on average
static const double ZREP_GAP      = 1.5;  // largest vertical step of the contiguous column
static const double SHARE_RADIUS  = 1.0;  // horizontal radius of a candidate's first-return share
static const double BAND          = 1.0;  // half height of the band around a candidate's own top
static const double LIT_SHARE     = 0.75; // first-return share at or above which a top is lit
static const double LIT_DEPTH     = 5.0;  // largest depth of a lit top
static const double COVER_HEIGHT  = 2.0;  // first returns above this height are canopy
static const double MAX_COLUMN    = 3.0;  // column radius at the lowest density the gate is used on
static const double PI            = 3.14159265358979323846;

// 95th percentile of the n first values of a sorted run. Same convention as MetricManager::percentile
static double percentile95(const double* x, uint32_t n)
{
  double rank = 0.95 * (double)(n - 1) + 1.0;
  uint32_t i = (uint32_t)rank;
  double frac = rank - (double)i;
  double lo = x[i - 1];
  double hi = (frac > 0.0) ? x[i] : lo;
  return lo + (hi - lo) * frac;
}

// Offsets of the largest window, nearest first
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

  std::stable_sort(window.begin(), window.end(), [](const std::pair<int,int>& a, const std::pair<int,int>& b)
  {
    return a.first*a.first + a.second*a.second < b.first*b.first + b.second*b.second;
  });

  return window;
}

// Key of the bucket a coordinate falls in, for a grid of cell size 'cell'
static uint64_t bucket_key(double x, double y, double cell)
{
  int32_t bx = (int32_t)std::floor(x/cell);
  int32_t by = (int32_t)std::floor(y/cell);
  return (((uint64_t)(uint32_t)bx) << 32) | (uint32_t)by;
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

double LASRmultichm::need_buffer() const
{
  double b = MAX3(variable_ws ? max_ws : ws, dist_2d, dist_3d);

  // The column of a candidate reaches MAX_COLUMN further than its own top
  if (depth_gate) b = std::max(b, std::max(near_2d, MAX_COLUMN));

  return b;
}

double LASRmultichm::window_at(double v) const
{
  if (!variable_ws) return ws;

  double pos = (v - ws_lut_min) / ws_lut_step;
  if (pos <= 0) return ws_lut.front();
  if (pos >= (double)(ws_lut.size() - 1)) return ws_lut.back();

  size_t i = (size_t)pos;
  double frac = pos - (double)i;
  return ws_lut[i] * (1.0 - frac) + ws_lut[i + 1] * frac;
}

bool LASRmultichm::set_parameters(const nlohmann::json& stage)
{
  ws = stage.at("ws");
  variable_ws = stage.contains("ws_lut") && !stage.at("ws_lut").empty();
  if (variable_ws)
  {
    ws_lut = stage.at("ws_lut").get<std::vector<double>>();
    ws_lut_min = stage.value("ws_lut_min", 0.0);
    ws_lut_step = stage.value("ws_lut_step", 0.5);

    if (ws_lut_step <= 0)
    {
      last_error = "ws_lut_step must be > 0";
      return false;
    }
    for (double r : ws_lut)
    {
      if (!std::isfinite(r) || r <= 0)
      {
        last_error = "ws_lut must contain only finite positive values";
        return false;
      }
    }

    max_ws = *std::max_element(ws_lut.begin(), ws_lut.end());
  }

  res = stage.value("res", 1.0);
  min_height = stage.value("min_height", 2.0);
  layer_thickness = stage.value("layer_thickness", 0.5);
  dist_2d = stage.value("dist_2d", 3.0);
  dist_3d = stage.value("dist_3d", 5.0);
  use_max = stage.value("use_max", false);

  if (res <= 0 || (!variable_ws && ws <= 0) || layer_thickness <= 0 || dist_2d <= 0 || dist_3d <= 0)
  {
    last_error = "res, ws, layer_thickness, dist_2d and dist_3d must be positive";
    return false;
  }

  depth_max = stage.value("depth_max", 0.0);
  near_2d = stage.value("near_2d", 0.0);
  zmin = stage.value("zmin", 0.0);
  zmin_surface = stage.value("zmin_surface", 0.0);
  lit_union = stage.value("lit_union", false);
  cover_min = stage.value("cover_min", -1.0);
  std::string report = stage.value("report", std::string("z"));

  if (depth_max < 0 || near_2d < 0 || zmin < 0 || zmin_surface < 0 || cover_min < -1)
  {
    last_error = "depth_max, near_2d, zmin, zmin_surface and cover_min must be positive";
    return false;
  }

  if (report != "z" && report != "zrep")
  {
    last_error = "report must be 'z' or 'zrep'";
    return false;
  }

  report_zrep = report == "zrep";
  depth_gate = depth_max > 0 || near_2d > 0 || zmin > 0 || zmin_surface > 0 || lit_union
            || cover_min >= 0 || report_zrep;

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

  window = build_window(variable_ws ? max_ws : ws, res);
  if (!build_index(las, grid)) return false;

  std::vector<Maximum> maxima;
  size_t alive = 0;
  double zmax = update_chm(false, alive);
  int layer = 1;

  while (!active.empty() && zmax > min_height)
  {
    if (progress->interrupted()) break;

    find_maxima(grid, maxima, layer);
    zmax = update_chm(true, alive);
    layer++;

    progress->update(las->npoints - alive);
    progress->show();
  }

  if (depth_gate)
  {
    features(las, maxima);
    gate(maxima);
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

// Group the heights by cell and sort each group. This is the only pass over the points with the
// one that reads the columns of the candidates. It also measures the canopy cover and the first
// return density the depth gate reads, on the chunk's own extent
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

  // Decode and filter each point once; the scatter below reuses (cell, z) instead of
  // calling get_point() and cell_from_xy() again
  std::vector<int32_t> cell_of(las->npoints, -1);
  std::vector<double> zval(las->npoints);

  has_return_number = las->header->schema.find_attribute("ReturnNumber") != nullptr;

  size_t nfirst = 0;
  size_t ncanopy = 0;

  #pragma omp parallel num_threads(ncpu)
  {
    Point p;
    p.set_schema(&las->header->schema);
    AttributeAccessor return_number("ReturnNumber");
    size_t first = 0;
    size_t canopy = 0;

    #pragma omp for
    for (size_t i = 0 ; i < las->npoints ; i++)
    {
      if (!las->get_point(i, &p, &pointfilter)) continue;
      int cell = grid.cell_from_xy(p.get_x(), p.get_y());
      if (cell < 0) continue;
      cell_of[i] = cell;
      zval[i] = p.get_z();
      #pragma omp atomic
      offset[cell+1]++;

      if (!depth_gate || !has_return_number) continue;
      if (return_number(&p) != 1.0) continue;

      double x = p.get_x();
      double y = p.get_y();
      if (x < xmin || x > xmax || y < ymin || y > ymax) continue;

      first++;
      if (zval[i] > COVER_HEIGHT) canopy++;
    }

    #pragma omp atomic
    nfirst += first;
    #pragma omp atomic
    ncanopy += canopy;
  }

  if (depth_gate)
  {
    double area = std::max(0.0, xmax - xmin) * std::max(0.0, ymax - ymin);
    frdens = (area > 0) ? (double)nfirst/area : 0.0;
    cover = (nfirst > 0) ? (double)ncanopy/(double)nfirst : NAN;
    r_col = (frdens > 0) ? std::max(COLUMN_RADIUS, std::sqrt(COLUMN_FIRST/(PI*frdens))) : MAX_COLUMN;

    // A closed stand is decided on the cover: without return numbers there is none
    if (cover_min > 0 && cover_min <= 1 && !std::isfinite(cover))
    {
      last_error = "the depth gate needs the canopy cover, which requires first returns";
      return false;
    }
  }

  for (int c = 0 ; c < ncells ; c++) offset[c+1] += offset[c];

  std::vector<uint32_t> cursor(offset.begin(), offset.end()-1);
  z.resize(offset[ncells]);

  for (size_t i = 0 ; i < las->npoints ; i++)
  {
    int cell = cell_of[i];
    if (cell >= 0) z[cursor[cell]++] = zval[i];
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
double LASRmultichm::update_chm(bool peel, size_t& alive)
{
  double zmax = NA_F32_RASTER;
  size_t nalive = 0;
  size_t n = active.size();

  #pragma omp parallel for num_threads(ncpu)
  for (size_t a = 0 ; a < n ; a++)
  {
    int c = active[a];
    const double* run = z.data() + offset[c];

    if (peel)
    {
      double threshold = chm[c] - layer_thickness;
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
// 'top' is the highest point still alive in the cell: the height of the candidate's own top
void LASRmultichm::find_maxima(const Grid& grid, std::vector<Maximum>& maxima, int layer)
{
  int ncols = grid.get_ncols();
  int nrows = grid.get_nrows();
  size_t n = active.size();

  for (size_t a = 0 ; a < n ; a++)
  {
    int c = active[a];
    if (has_lm[c]) continue;

    double v = chm[c];
    if (v < min_height) continue;

    int row = c/ncols;
    int col = c%ncols;
    bool is_max = true;
    double hws = window_at(v)/2;

    for (size_t w = 0 ; w < window.size() && is_max ; w++)
    {
      int dr = window[w].first;
      int dc = window[w].second;
      if ((dc*dc + dr*dr)*res*res > hws*hws) break;

      int rr = row + dr;
      int cc = col + dc;
      if (rr < 0 || rr >= nrows || cc < 0 || cc >= ncols) continue;

      int j = rr*ncols + cc;
      double u = chm[j];

      if (u == NA_F32_RASTER) continue;
      if (u > v || (u == v && has_lm[j])) is_max = false;
    }

    if (is_max)
    {
      has_lm[c] = 1;
      maxima.push_back({grid.x_from_cell(c), grid.y_from_cell(c), v, layer,
                        z[offset[c] + k[c] - 1], 0.0, v, NAN, true});
    }
  }
}

// Depth, contiguous top and first return share of every candidate. A candidate's column is every
// return within r_col horizontally; its depth is the highest of them minus its own height, its
// zrep the top of the run that climbs from the candidate's own top with steps of at most ZREP_GAP,
// and its share the fraction of first returns within BAND of that top.
// Sequential: the candidates of a bucket are shared by the points around them
void LASRmultichm::features(PointCloud* las, std::vector<Maximum>& maxima)
{
  size_t n = maxima.size();
  if (n == 0) return;

  std::unordered_map<uint64_t, std::vector<uint32_t>> buckets;
  for (uint32_t i = 0 ; i < n ; i++)
    buckets[bucket_key(maxima[i].x, maxima[i].y, r_col)].push_back(i);

  std::vector<std::vector<double>> above(n);
  std::vector<uint32_t> nband(n, 0);
  std::vector<uint32_t> nfirst(n, 0);
  std::vector<double> top(n);
  for (size_t i = 0 ; i < n ; i++) top[i] = maxima[i].ztop;

  double r2 = r_col*r_col;
  double share2 = SHARE_RADIUS*SHARE_RADIUS;

  Point p;
  p.set_schema(&las->header->schema);
  AttributeAccessor return_number("ReturnNumber");

  for (size_t i = 0 ; i < las->npoints ; i++)
  {
    if (!las->get_point(i, &p, &pointfilter)) continue;

    double x = p.get_x();
    double y = p.get_y();
    double zz = p.get_z();
    bool first = has_return_number && return_number(&p) == 1.0;

    int32_t bx = (int32_t)std::floor(x/r_col);
    int32_t by = (int32_t)std::floor(y/r_col);

    for (int32_t dbx = -1 ; dbx <= 1 ; dbx++)
    {
      for (int32_t dby = -1 ; dby <= 1 ; dby++)
      {
        auto it = buckets.find((((uint64_t)(uint32_t)(bx+dbx)) << 32) | (uint32_t)(by+dby));
        if (it == buckets.end()) continue;

        for (uint32_t ci : it->second)
        {
          const Maximum& m = maxima[ci];
          double dx = x - m.x;
          double dy = y - m.y;
          double d2 = dx*dx + dy*dy;
          if (d2 > r2) continue;

          if (zz > top[ci]) top[ci] = zz;
          if (zz > m.ztop) above[ci].push_back(zz);
          if (d2 <= share2 && std::fabs(zz - m.ztop) <= BAND)
          {
            nband[ci]++;
            if (first) nfirst[ci]++;
          }
        }
      }
    }
  }

  for (size_t i = 0 ; i < n ; i++)
  {
    Maximum& m = maxima[i];

    m.depth = top[i] - m.z;

    std::sort(above[i].begin(), above[i].end());
    double zrep = m.ztop;
    for (double v : above[i])
    {
      if ((double)v - zrep > ZREP_GAP) break;
      zrep = v;
    }
    m.zrep = zrep;

    m.share = (nband[i] > 0) ? (double)nfirst[i]/(double)nband[i] : NAN;
  }
}

// Keep rules: height floors of the surface and of the deeper layers in every stand, and in a
// closed stand a deepest position under the canopy surface for the deeper layers, or a lit one.
// A closed stand is cover >= cover_min; a negative cover_min means no stand is closed
void LASRmultichm::gate(std::vector<Maximum>& maxima)
{
  closed = cover_min >= 0 && (cover_min == 0 || cover >= cover_min);

  for (auto& m : maxima)
  {
    bool surface = m.layer == 1;
    bool keep = surface ? (m.z >= zmin_surface) : (m.z >= zmin);

    if (!surface && closed)
    {
      bool enough = depth_max <= 0 || m.depth <= depth_max;
      if (!enough && lit_union) enough = m.share >= LIT_SHARE && m.depth <= LIT_DEPTH;
      keep = keep && enough;
    }

    m.keep = keep;
  }
}

// Sort the candidates by decreasing height and keep those far enough from the trees already
// retained. Ties are broken on the coordinates to get a reproducible order.
// Retained trees are bucketed on a MAX(dist_2d, dist_3d) grid so a candidate only scans its
// 3x3 neighbourhood instead of every tree kept so far.
// In a closed stand with near_2d, one horizontal spacing applies to every kept top and the
// surface peaks are no longer exempt; without it the surface peaks of a closed stand are kept
// unconditionally. An open stand keeps the package's filter.
void LASRmultichm::select_trees(std::vector<Maximum>& maxima) const
{
  double d2d = dist_2d*dist_2d;
  double d3d = dist_3d*dist_3d;
  double spacing = near_2d*near_2d;
  bool uniform = closed && near_2d > 0;
  double cell = MAX(MAX(dist_2d, dist_3d), near_2d);

  std::sort(maxima.begin(), maxima.end(), [](const Maximum& a, const Maximum& b)
  {
    if (a.z != b.z) return a.z > b.z;
    if (a.x != b.x) return a.x < b.x;
    return a.y < b.y;
  });

  std::vector<Maximum> trees;
  std::unordered_map<uint64_t, std::vector<uint32_t>> buckets;

  for (const auto& m : maxima)
  {
    if (!m.keep) continue;

    bool exempt = closed && !uniform && m.layer == 1;
    bool detected = exempt;
    int32_t bx = (int32_t)std::floor(m.x/cell);
    int32_t by = (int32_t)std::floor(m.y/cell);

    if (!exempt)
    {
      detected = true;

      for (int dbx = -1 ; dbx <= 1 && detected ; dbx++)
      {
        for (int dby = -1 ; dby <= 1 && detected ; dby++)
        {
          auto it = buckets.find((((uint64_t)(uint32_t)(bx+dbx)) << 32) | (uint32_t)(by+dby));
          if (it == buckets.end()) continue;

          for (uint32_t idx : it->second)
          {
            const Maximum& t = trees[idx];
            double dx = m.x - t.x;
            double dy = m.y - t.y;
            double dz = m.z - t.z;
            double dd = dx*dx + dy*dy;

            if (uniform) { if (dd < spacing) { detected = false; break; } }
            else if (dd < d2d || dd + dz*dz < d3d) { detected = false; break; }
          }
        }
      }
    }

    if (detected)
    {
      buckets[bucket_key(m.x, m.y, cell)].push_back((uint32_t)trees.size());
      trees.push_back(m);
    }
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
      p.z = report_zrep ? t.zrep : t.z;

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
  std::vector<double>().swap(z);
  std::vector<uint32_t>().swap(offset);
  std::vector<uint32_t>().swap(k);
  std::vector<double>().swap(chm);
  std::vector<char>().swap(has_lm);
  std::vector<int>().swap(active);
}
