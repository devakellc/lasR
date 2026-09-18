#ifndef PITFILL_H
#define PITFILL_H

#include "Stage.h"

class LASRpitfill: public StageRaster
{
public:
  LASRpitfill() = default;
  bool process() override;
  double need_buffer() const override;
  bool set_parameters(const nlohmann::json&) override;
  bool connect(const std::list<std::unique_ptr<Stage>>&, const std::string& uuid) override;
  std::string get_name() const override { return "pit_fill"; }

  // The focal filter builds its answer in a raster of its own before swapping it in
  double memory_per_area() const override { return 2*StageRaster::memory_per_area(); };

  // multi-threading
  LASRpitfill* clone() const override { return new LASRpitfill(*this); };

private:
  int lap_size;
  float thr_lap;
  float thr_spk;
  int med_size;
  int dil_radius;
};

#endif