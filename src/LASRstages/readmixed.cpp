#include "readmixed.h"

#include "EPTio.h"
#include "LASio.h"
#include "FileCollection.h"

bool LASRmixedreader::build_sources(Chunk& chunk)
{
  std::vector<std::string> las_files;
  std::vector<std::string> las_neighbours;

  for (const auto& file : chunk.main_files)
    if (!is_ept_endpoint(file)) las_files.push_back(file);

  for (const auto& file : chunk.neighbour_files)
    if (!is_ept_endpoint(file)) las_neighbours.push_back(file);

  // A circular query is already narrower than its own box and is left alone
  double qxmin = chunk.xmin;
  double qymin = chunk.ymin;
  double qxmax = chunk.xmax;
  double qymax = chunk.ymax;

  if (chunk.aoi != nullptr && chunk.shape != ShapeType::CIRCLE)
  {
    qxmin = MAX(qxmin, chunk.aoi->xmin());
    qymin = MAX(qymin, chunk.aoi->ymin());
    qxmax = MIN(qxmax, chunk.aoi->xmax());
    qymax = MIN(qymax, chunk.aoi->ymax());
  }

  // LASio merges the LAS files and reconciles their scale and offset, so it is the reference
  if (!las_files.empty())
  {
    auto lasio = std::unique_ptr<LASio>(new LASio());
    lasio->query(las_files, las_neighbours, qxmin, qymin, qxmax, qymax,
                 chunk.buffer, chunk.shape == ShapeType::CIRCLE, filters);
    sources.emplace_back(las_files[0], std::move(lasio));
  }

  for (const auto& file : chunk.main_files)
  {
    if (!is_ept_endpoint(file)) continue;

    auto eptio = std::unique_ptr<EPTio>(new EPTio());
    eptio->set_aoi(chunk.aoi);
    eptio->query(file, chunk.xmin, chunk.ymin, chunk.xmax, chunk.ymax,
                 chunk.buffer, chunk.shape == ShapeType::CIRCLE, filters);
    sources.emplace_back(file, std::move(eptio));
  }

  return true;
}
