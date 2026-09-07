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

  // LASio merges the LAS files and reconciles their scale and offset, so it is the reference
  if (!las_files.empty())
  {
    auto lasio = std::unique_ptr<LASio>(new LASio());
    lasio->query(las_files, las_neighbours, chunk.xmin, chunk.ymin, chunk.xmax, chunk.ymax,
                 chunk.buffer, chunk.shape == ShapeType::CIRCLE, filters);
    sources.emplace_back(las_files[0], std::move(lasio));
  }

  for (const auto& file : chunk.main_files)
  {
    if (!is_ept_endpoint(file)) continue;

    auto eptio = std::unique_ptr<EPTio>(new EPTio());
    eptio->query(file, chunk.xmin, chunk.ymin, chunk.xmax, chunk.ymax,
                 chunk.buffer, chunk.shape == ShapeType::CIRCLE, filters);
    sources.emplace_back(file, std::move(eptio));
  }

  return true;
}
