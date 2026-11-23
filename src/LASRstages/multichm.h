#ifndef LASRMULTICHM_H
#define LASRMULTICHM_H

#include "Stage.h"
#include "Grid.h"
#include "Vector.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

class LASRmultichm : public StageVector, public StageMaxima
{
public:
  LASRmultichm();
  bool process(PointCloud*& las) override;
  bool write() override;
  void clear(bool last) override;
  double need_buffer() const override;
  bool set_parameters(const nlohmann::json&) override;
  std::string get_name() const override { return "multichm"; };
  std::vector<PointLAS>& get_maxima() override { return lm; };
  bool is_parallelized() const override { return true; };
  double memory_per_point() const override { return sizeof(double); };

  // multi-threading
  LASRmultichm* clone() const override { return new LASRmultichm(*this); };

private:
  struct Maximum
  {
    double x, y, z;
    int layer;
    double ztop;
    double depth;
    double zrep;
    double share;
    bool keep;
  };

  bool build_index(PointCloud* las, const Grid& grid);
  double update_chm(bool peel, size_t& alive);
  void find_maxima(const Grid& grid, std::vector<Maximum>& maxima, int layer);
  void features(PointCloud* las, std::vector<Maximum>& maxima);
  void gate(std::vector<Maximum>& maxima);
  void select_trees(std::vector<Maximum>& maxima) const;
  void record(const std::vector<Maximum>& trees, const Header* header);
  double window_at(double v) const;

  double res;
  double ws;
  double min_height;
  double layer_thickness;
  double dist_2d;
  double dist_3d;
  bool use_max;

  bool variable_ws = false;
  std::vector<double> ws_lut;
  double ws_lut_min = 0;
  double ws_lut_step = 0.5;
  double max_ws = 0;

  // Depth gate of the multichm_depth arm. The defaults leave every candidate in and keep the
  // package's distance filter: depth_max 0 is an infinite depth, cover_min negative no closed stand
  double depth_max = 0;
  double near_2d = 0;
  double zmin = 0;
  double zmin_surface = 0;
  double cover_min = -1;
  bool lit_union = false;
  bool report_zrep = false;
  bool depth_gate = false;

  // Per chunk features the gate reads, measured on the chunk's own extent
  bool closed = false;
  bool has_return_number = false;
  double cover = 0;
  double frdens = 0;
  double r_col = 1.0;

  // The heights are grouped by cell in 'z' and sorted. The peeling threshold is the same for every
  // point of a cell so the points alive are always a prefix of the run. 'k' is its length.
  std::vector<uint32_t> offset;
  std::vector<double> z;
  std::vector<uint32_t> k;
  std::vector<double> chm;
  std::vector<char> has_lm;
  std::vector<int> active;
  std::vector<std::pair<int,int>> window;

  std::vector<PointLAS> lm;

  std::shared_ptr<unsigned int> counter;
  std::shared_ptr<std::unordered_map<uint64_t, unsigned int>> unicity_table;
};

#endif
