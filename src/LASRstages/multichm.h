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
  double need_buffer() const override { return MAX3(ws, dist_2d, dist_3d); };
  bool set_parameters(const nlohmann::json&) override;
  std::string get_name() const override { return "multichm"; };
  std::vector<PointLAS>& get_maxima() override { return lm; };
  bool is_parallelized() const override { return true; };

  // multi-threading
  LASRmultichm* clone() const override { return new LASRmultichm(*this); };

private:
  struct Maximum { double x, y, z; };

  bool build_index(PointCloud* las, const Grid& grid);
  float update_chm(bool peel, size_t& alive);
  void find_maxima(const Grid& grid, std::vector<Maximum>& maxima);
  void select_trees(std::vector<Maximum>& maxima) const;
  void record(const std::vector<Maximum>& trees, const Header* header);

  double res;
  double ws;
  double min_height;
  double layer_thickness;
  double dist_2d;
  double dist_3d;
  bool use_max;

  // The heights are grouped by cell in 'z' and sorted. The peeling threshold is the same for every
  // point of a cell so the points alive are always a prefix of the run. 'k' is its length.
  std::vector<uint32_t> offset;
  std::vector<float> z;
  std::vector<uint32_t> k;
  std::vector<float> chm;
  std::vector<char> has_lm;
  std::vector<int> active;
  std::vector<std::pair<int,int>> window;

  std::vector<PointLAS> lm;

  std::shared_ptr<unsigned int> counter;
  std::shared_ptr<std::unordered_map<uint64_t, unsigned int>> unicity_table;
};

#endif
