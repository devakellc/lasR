#ifndef LASRTRANSFORMCRS_H
#define LASRTRANSFORMCRS_H

#include "Stage.h"
#include "CRS.h"

class OGRCoordinateTransformation;

// Reprojects the point cloud from its current CRS into a user-given target CRS
class LASRtransformcrs : public Stage
{
public:
  LASRtransformcrs();
  LASRtransformcrs(const LASRtransformcrs& other);
  // The copy ctor rebuilds its own transform per clone; forbid the shallow-copying default
  LASRtransformcrs& operator=(const LASRtransformcrs&) = delete;
  ~LASRtransformcrs();

  bool set_parameters(const nlohmann::json&) override;
  bool process(PointCloud*& las) override;
  void set_crs(const CRS& crs) override;
  bool set_chunk(Chunk& chunk) override;
  void get_extent(double& xmin, double& ymin, double& xmax, double& ymax) override;
  double translate_buffer_to_input(double downstream_buffer) const override;

  std::string get_name() const override { return "transform_crs"; }

  // multi-threading: each clone must own its own (non thread-safe) transform object
  LASRtransformcrs* clone() const override { return new LASRtransformcrs(*this); }

private:
  bool build_transform();
  bool resolve_vertical();

private:
  CRS source_crs;
  CRS target_crs;
  bool transform_z;
  OGRCoordinateTransformation* transform;
  double target_to_source_buffer_scale;
  bool target_to_source_buffer_scale_valid;
};

#endif
