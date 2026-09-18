#include "randomwalker.h"
#include "localmaximum.h"
#include "openmp.h"

#include <chrono>
#include <cmath>
#include <limits>

bool LASRrandomwalker::set_parameters(const nlohmann::json& stage)
{
  th_tree = stage.value("th_tree", 2.0);
  th_cr = stage.value("th_cr", 0.55);
  beta = stage.value("beta", 1.0);
  radius = stage.value("max_cr", 20.0)/2;

  if (beta < 0)
  {
    last_error = "beta cannot be negative";
    return false;
  }

  for (auto elem : connections)
  {
    StageRaster* rst = dynamic_cast<StageRaster*>(elem.second);
    if (rst)
    {
      raster = Raster(rst->get_raster());
      break;
    }
  }

  return true;
}

bool LASRrandomwalker::process(PointCloud*& las)
{
  auto start_time = std::chrono::high_resolution_clock::now();

  StageMaxima* lmf = nullptr;
  StageRaster* rst = nullptr;
  for (auto elem : connections)
  {
    StageMaxima* p = dynamic_cast<StageMaxima*>(elem.second);
    if (p)
      lmf = p;
    else
      rst = dynamic_cast<StageRaster*>(elem.second);
  }

  if (lmf == nullptr || rst == nullptr)
  {
    last_error = "invalid pointers: must be 'StageMaxima' and 'StageRaster'. Please report this error."; // # nocov
    return false; // # nocov
  }

  const std::vector<PointLAS>& lm = lmf->get_maxima();
  const Raster& image = rst->get_raster();
  int ncells = raster.get_ncells();

  // The weights are computed on a canopy normalized between 0 and 1 so that beta does not depend
  // on the range of the heights. Everything below th_tree is not part of the graph
  double zmin = std::numeric_limits<double>::max();
  double zmax = std::numeric_limits<double>::lowest();
  for (int c = 0 ; c < ncells ; c++)
  {
    float v = image.get_value(c);
    if (image.is_na(v) || v < th_tree) continue;
    zmin = MIN(zmin, (double)v);
    zmax = MAX(zmax, (double)v);
  }

  if (zmax < zmin) return true;
  double span = (zmax > zmin) ? zmax-zmin : 1;

  progress->reset();
  progress->set_prefix("random walker");
  progress->set_total(lm.size());

  std::vector<float> z(ncells, std::numeric_limits<float>::quiet_NaN());
  for (int c = 0 ; c < ncells ; c++)
  {
    float v = image.get_value(c);
    if (!image.is_na(v) && v >= th_tree) z[c] = (float)((v-zmin)/span);
  }

  // One label per distinct seed cell. The first seed wins a shared cell
  std::vector<int> seed_cell;
  std::vector<int> seed_top;
  std::vector<int> seed_of(ncells, -1);
  for (size_t i = 0 ; i < lm.size() ; i++)
  {
    int cell = raster.cell_from_xy(lm[i].x, lm[i].y);
    if (cell < 0 || std::isnan(z[cell]) || seed_of[cell] >= 0) continue;
    seed_of[cell] = (int)seed_cell.size();
    seed_cell.push_back(cell);
    seed_top.push_back((int)i);
  }

  std::vector<float> prob(ncells, 0.0f);
  std::vector<int> owner(ncells, -1);
  int nseeds = (int)seed_cell.size();

  // The next for loop may be at the level 2 of a nested parallel region. Printing the progress bar
  // is not thread safe. We first check that we are in outer thread 0
  bool main_thread = omp_get_thread_num() == 0;

  #pragma omp parallel for num_threads(ncpu)
  for (int s = 0 ; s < nseeds ; s++)
  {
    if (progress->interrupted()) continue;

    walk(s, seed_cell[s], z, seed_of, prob, owner);

    #pragma omp critical (random_walker_progress)
    {
      (*progress)++;
      if (main_thread) progress->show();
    }
  }

  progress->done();

  // Without a stop criterion the walker labels every pixel of the canopy and the crowns creep down
  // into the gaps between the trees. A crown is truncated at th_cr of the height of its own apex
  for (int c = 0 ; c < ncells ; c++)
  {
    int s = owner[c];
    if (s < 0) continue;

    const PointLAS& top = lm[seed_top[s]];
    if (image.get_value(c) < th_cr*top.z) continue;

    raster.set_value(c, (float)top.FID);
  }

  if (verbose)
  {
    // # nocov start
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    print("  Random walker took %.2f sec. %d seeds\n", (float)duration.count()/1000.0f, nseeds);
    // # nocov end
  }

  return true;
}

// Probability for a walker released from a pixel to reach the seed 's' before any other seed
// (Grady 2006). The problem is solved with a successive over-relaxation restricted to a window of
// radius max_cr/2 around the seed. The other seeds and the border of the window are zeros. The
// pixels won by 's' are recorded in 'owner'
void LASRrandomwalker::walk(int s, int seed_cell, const std::vector<float>& z, const std::vector<int>& seed_of,
                            std::vector<float>& prob, std::vector<int>& owner)
{
  static const int drow[4] = {-1, 1, 0, 0};
  static const int dcol[4] = {0, 0, -1, 1};

  int ncols = raster.get_ncols();
  int nrows = raster.get_nrows();
  int rad = (int)std::ceil(radius/raster.get_xres());

  int row = raster.row_from_cell(seed_cell);
  int col = raster.col_from_cell(seed_cell);
  int row1 = MAX(row-rad, 0);
  int row2 = MIN(row+rad, nrows-1);
  int col1 = MAX(col-rad, 0);
  int col2 = MIN(col+rad, ncols-1);
  int nr = row2-row1+1;
  int nc = col2-col1+1;
  int n = nr*nc;

  // Only the pixels that are neither a seed nor a hole of the canopy are unknowns. They are packed
  // so that the relaxation runs on a contiguous range
  std::vector<int> index(n, -1);
  int nfree = 0;
  for (int l = 0 ; l < n ; l++)
  {
    int c = raster.cell_from_row_col(row1 + l/nc, col1 + l%nc);
    if (std::isnan(z[c]) || seed_of[c] >= 0) continue;
    index[l] = nfree++;
  }

  // The two extra values of 'x' are the constant boundaries: the zero of the other seeds and of the
  // border of the window, and the one of this seed. Every neighbour then points at a valid entry
  std::vector<int> cell(nfree);
  std::vector<float> inv(nfree, 0.0f);
  std::vector<float> x(nfree+2, 0.0f);
  std::vector<float> w(4*nfree, 0.0f);
  std::vector<int> neighbour(4*nfree, nfree);
  x[nfree+1] = 1.0f;

  for (int l = 0 ; l < n ; l++)
  {
    int f = index[l];
    if (f < 0) continue;

    int r = row1 + l/nc;
    int k = col1 + l%nc;
    cell[f] = raster.cell_from_row_col(r, k);

    double sum_w = 0;
    for (int i = 0 ; i < 4 ; i++)
    {
      int rr = r + drow[i];
      int kk = k + dcol[i];
      if (rr < 0 || rr >= nrows || kk < 0 || kk >= ncols) continue;

      int adj = raster.cell_from_row_col(rr, kk);
      if (std::isnan(z[adj])) continue;

      double dz = z[cell[f]]-z[adj];
      w[4*f+i] = (float)(std::exp(-beta*dz*dz) + 1e-6);
      sum_w += w[4*f+i];

      if (rr < row1 || rr > row2 || kk < col1 || kk > col2) continue;

      int m = index[(rr-row1)*nc + (kk-col1)];
      if (m >= 0) neighbour[4*f+i] = m;
      else if (adj == seed_cell) neighbour[4*f+i] = nfree+1;
    }

    // The sum of the weights does not change from a sweep to the next one
    if (sum_w > 0) inv[f] = (float)(1/sum_w);
  }

  // Optimal relaxation factor of the SOR for a nr x nc grid. Plain Gauss-Seidel (omega = 1) needs
  // an order of magnitude more sweeps
  double omega = 2/(1 + std::sin(std::acos(-1.0)/MAX(nr, nc)));
  double delta = 1;

  for (int iter = 0 ; iter < 200 && delta > 1e-5 ; iter++)
  {
    delta = 0;

    for (int f = 0 ; f < nfree ; f++)
    {
      double sum_wx = 0;
      for (int i = 0 ; i < 4 ; i++) sum_wx += w[4*f+i]*x[neighbour[4*f+i]];

      double d = omega*(sum_wx*inv[f] - x[f]);
      x[f] += (float)d;
      delta = MAX(delta, std::fabs(d));
    }
  }

  // A pixel that no walker reached is left to the background. Ties are broken with the rank of the
  // seed so that the result does not depend on the order of the threads
  #pragma omp critical(random_walker_merge)
  {
    prob[seed_cell] = 1.0f;
    owner[seed_cell] = s;

    for (int f = 0 ; f < nfree ; f++)
    {
      if (x[f] <= 1e-8f) continue;

      if (x[f] > prob[cell[f]] || (x[f] == prob[cell[f]] && s < owner[cell[f]]))
      {
        prob[cell[f]] = x[f];
        owner[cell[f]] = s;
      }
    }
  }
}

bool LASRrandomwalker::connect(const std::list<std::unique_ptr<Stage>>& pipeline, const std::string& uid)
{
  Stage* s = search_connection(pipeline, uid);

  if (s == nullptr) return false;

  StageMaxima* p = dynamic_cast<StageMaxima*>(s);
  StageRaster* q = dynamic_cast<StageRaster*>(s);

  if (p || q)
    set_connection(s);
  else
  {
    last_error = "Incompatible stage combination for 'random_walker'"; // # nocov
    return false; // # nocov
  }

  return true;
}
