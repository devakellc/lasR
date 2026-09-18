#ifndef RANDOMWALKER_H
#define RANDOMWALKER_H

#include "Stage.h"

#include <vector>

class LASRrandomwalker : public StageRaster
{
public:
  LASRrandomwalker() = default;
  bool process(PointCloud*& las) override;
  // A seed up to max_cr/2 beyond the chunk can still own a boundary pixel, and its own solve
  // window then reaches another max_cr/2 further out
  double need_buffer() const override { return 2*radius; };
  bool connect(const std::list<std::unique_ptr<Stage>>&, const std::string& uuid) override;
  bool set_parameters(const nlohmann::json&) override;
  std::string get_name() const override { return "random_walker"; }
  bool is_parallelized() const override { return true; };

  // multi-threading
  LASRrandomwalker* clone() const override { return new LASRrandomwalker(*this); };

private:
  void walk(int s, int seed_cell, const std::vector<float>& z, const std::vector<int>& seed_of,
            const std::vector<float>& edge_w, std::vector<float>& prob, std::vector<int>& owner);

  double th_tree;
  double th_cr;
  double beta;
  double radius;
};

#endif
