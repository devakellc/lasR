#ifndef RANDOMWALKER_H
#define RANDOMWALKER_H

#include "Stage.h"

#include <vector>

class LASRrandomwalker : public StageRaster
{
public:
  LASRrandomwalker() = default;
  bool process(PointCloud*& las) override;
  double need_buffer() const override { return 10; };
  bool connect(const std::list<std::unique_ptr<Stage>>&, const std::string& uuid) override;
  bool set_parameters(const nlohmann::json&) override;
  std::string get_name() const override { return "random_walker"; }
  bool is_parallelized() const override { return true; };

  // multi-threading
  LASRrandomwalker* clone() const override { return new LASRrandomwalker(*this); };

private:
  void walk(int s, int seed_cell, const std::vector<float>& z, const std::vector<int>& seed_of,
            std::vector<float>& prob, std::vector<int>& owner);

  double th_tree;
  double th_cr;
  double beta;
  double radius;
};

#endif
