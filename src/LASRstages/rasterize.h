#ifndef LASRRASTERIZE_H
#define LASRRASTERIZE_H

#include "Stage.h"
#include "Metrics.h"
#include "NA.h"
#include "Interval.h"

class LASRrasterize : public StageRaster
{
public:
  LASRrasterize() = default;
  bool process(Point*& p) override;
  bool process(PointCloud*& las) override;
  double need_buffer() const override { return MAX(raster.get_xres(), window); };
  bool is_streamable() const override { return streamable; };
  bool is_parallelized() const override { return !streamable; };
  bool set_parameters(const nlohmann::json&) override;
  bool connect(const std::list<std::unique_ptr<Stage>>&, const std::string& uuid) override;
  std::string get_name() const override { return "rasterize"; };

  // A windowed rasterization groups the points by cell. A connected one takes the early path
  // instead and never groups: it interpolates into a value per cell
  double memory_per_area() const override
  {
    if (raster.get_xres() <= 0) return StageRaster::memory_per_area();

    double cells = 1.0/(raster.get_xres()*raster.get_yres());
    if (connections.size() > 0) return StageRaster::memory_per_area() + sizeof(double)*cells;
    if (!streamable) return StageRaster::memory_per_area() + 64.0*cells;
    return StageRaster::memory_per_area();
  };

  // The grouped branch above also keeps an Interval per point in the map it builds
  double memory_per_point() const override
  {
    if (raster.get_xres() <= 0) return StageRaster::memory_per_point();
    if (connections.size() == 0 && !streamable) return StageRaster::memory_per_point() + sizeof(Interval);
    return StageRaster::memory_per_point();
  };

  // multi-threading
  LASRrasterize* clone() const override { return new LASRrasterize(*this); };

private:
  std::vector<std::string> methods;
  bool streamable;
  double window;
  MetricManager metric_engine;
};

#endif
