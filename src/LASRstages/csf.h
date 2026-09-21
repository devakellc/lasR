#ifndef CSF_H
#define CSF_H

#include "Stage.h"

// sizeof(csf::Particle), which the vendored header does not expose
#define CSF_PARTICLE_BYTES 224

class LASRcsf: public Stage
{
public:
  LASRcsf();
  bool process(PointCloud*& las) override;
  double need_buffer() const override { return cloth_resolution*5; };

  // setPointCloud takes its vector by value, so three copies of the coordinates coexist
  double memory_per_point() const override { return 3*3*sizeof(double) + 2*sizeof(int); };

  double memory_per_area() const override { return cloth_resolution > 0 ? (CSF_PARTICLE_BYTES + sizeof(double))/(cloth_resolution*cloth_resolution) : 0; };
  bool is_parallelized() const override { return true; };
  bool set_parameters(const nlohmann::json&) override;
  std::string get_name() const override { return "csf"; };

  // multi-threading
  LASRcsf* clone() const override { return new LASRcsf(*this); };

private:
  bool slope_smooth;
  float class_threshold;
  float cloth_resolution;
  int  rigidness;
  int iterations;
  float time_step;
  int classification;
};

#endif