#ifndef CHUNK_H
#define CHUNK_H

#include "CRS.h"
#include "Shape.h"
#include <memory>
#include <string>

struct Chunk
{
  Chunk()
  {
    clear();
  };

  bool is_empty()
  {
    return main_files.empty();
  }

  void clear()
  {
    xmin = 0;
    ymin = 0;
    xmax = 0;
    ymax = 0;
    id = 0;
    shape = ShapeType::UNKNOWN;
    crs = CRS();
    aoi = nullptr;
    buffer = 0;
    process = true;
    is_query = false;
    qxmin = 0;
    qymin = 0;
    qxmax = 0;
    qymax = 0;
    name.clear();
    main_files.clear();
    neighbour_files.clear();
  };

  // The region this chunk was asked for, as it reached the collection: xmin..ymax above are clamped
  // to the extent of the files, which would move and shrink a circle reaching past the data. A chunk
  // that is a whole file was asked for nothing: it reads its own bounds.
  void query_bounds(double& bxmin, double& bymin, double& bxmax, double& bymax) const
  {
    bxmin = is_query ? qxmin : xmin;
    bymin = is_query ? qymin : ymin;
    bxmax = is_query ? qxmax : xmax;
    bymax = is_query ? qymax : ymax;
  };

  // # nocov start
  void dump() const
  {
    printf("name: %s\n", name.c_str());
    printf("bbox %.1lf %.1lf %.1lf %.1lf\n", xmin, ymin, xmax, ymax);
    printf("buffer %.1lf\n", buffer);
    printf("Files:\n");
    for (const auto& file : main_files) printf("  %s\n", file.c_str());
    printf("Neighbour:\n");
    for (const auto& file : neighbour_files) printf("  %s\n", file.c_str());
  };
  // # nocov end

  double xmin;
  double ymin;
  double xmax;
  double ymax;
  double buffer;
  bool process;
  int id;
  ShapeType shape;
  CRS crs;
  std::shared_ptr<const PolygonShape> aoi; // area of interest clipping the chunk, null when there is none
  bool is_query;      // the chunk comes from a query; the region it was asked for is q* below
  double qxmin;       // the query's own rectangle, before the clamp to the extent of the files
  double qymin;
  double qxmax;
  double qymax;
  std::string name;
  std::vector<std::string> main_files;
  std::vector<std::string> neighbour_files;
};

#endif