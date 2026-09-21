#ifndef LASRTRIANGULATE_H
#define LASRTRIANGULATE_H

#include "Stage.h"

#include <cstddef>
#include "Vector.h"
#include "Shape.h"

#include <unordered_set>

class Raster;

namespace delaunator
{
  class Delaunator;
}

class LASRtriangulate : public StageVector
{
public:
  LASRtriangulate();
  bool process(PointCloud*& las) override;
  bool interpolate(std::vector<double>& res, const Raster* raster = nullptr);
  bool contour(std::vector<Edge>& edges) const;
  double need_buffer() const override { return 20.0; }

  // Coordinates and an index per point, the delaunator arrays, and the second index interpolating
  // through them builds alongside. Its buckets follow the cell count and are not counted
  double memory_per_point() const override { return 2*sizeof(double) + sizeof(int) + 15*sizeof(std::size_t) + sizeof(PointXYI) + sizeof(int); }
  void clear(bool last) override;
  bool write() override;
  bool set_parameters(const nlohmann::json&) override;
  std::string get_name() const override { return "triangulate"; }

  // multi-threading
  bool is_parallelizable() const override { return true; };
  LASRtriangulate* clone() const override { return new LASRtriangulate(*this); };

private:
  bool keep_large;
  double trim;
  unsigned int npoints;
  std::vector<double> coords;
  std::vector<int> index_map;
  std::string use_attribute;
  delaunator::Delaunator* d;
  PointCloud* las;
};

#endif
