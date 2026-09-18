#ifndef REGIONGROWING_H
#define REGIONGROWING_H

#include "Stage.h"

class LASRregiongrowing : public StageRaster
{
public:
  LASRregiongrowing() = default;
  bool process(PointCloud*& las) override;
  double need_buffer() const override { return 10; };
  bool connect(const std::list<std::unique_ptr<Stage>>&, const std::string& uuid) override;
  bool set_parameters(const nlohmann::json&) override;
  std::string get_name() const override { return "region_growing"; }

  // Every cell the regions grow into is kept in the list of its region, an int per cell at most
  double memory_per_area() const override { return 2*StageRaster::memory_per_area(); };

  // multi-threading
  LASRregiongrowing* clone() const override { return new LASRregiongrowing(*this); };


private:
  double th_seed;
  double th_crown;
  double th_tree;
  double DIST;
};

#endif