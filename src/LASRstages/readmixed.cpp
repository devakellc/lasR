#include "readmixed.h"

#include "EPTio.h"
#include "LASio.h"
#include "FileCollection.h"

bool LASRmixedreader::build_sources(Chunk& chunk)
{
  std::vector<std::string> las_files;
  std::vector<std::string> las_neighbours;
  std::vector<std::string> ept_endpoints;

  // Both main and neighbour files can be of either kind: a chunk built file by file (not
  // under a query) puts any file overlapping its buffered extent in neighbour_files, EPT
  // endpoints included, and a chunk whose own main file is an EPT endpoint can just as well
  // have LAS neighbours. Both must still be read for the buffer to be complete.
  for (const auto& file : chunk.main_files)
  {
    if (is_ept_endpoint(file)) ept_endpoints.push_back(file);
    else las_files.push_back(file);
  }

  for (const auto& file : chunk.neighbour_files)
  {
    if (is_ept_endpoint(file)) ept_endpoints.push_back(file);
    else las_neighbours.push_back(file);
  }

  // LASio merges the LAS files and reconciles their scale and offset, so it is the reference
  if (!las_files.empty() || !las_neighbours.empty())
  {
    auto lasio = std::shared_ptr<LASio>(new LASio());
    lasio->query(las_files, las_neighbours, chunk.xmin, chunk.ymin, chunk.xmax, chunk.ymax,
                 chunk.buffer, chunk.shape == ShapeType::CIRCLE, filters);
    sources.emplace_back(las_files.empty() ? las_neighbours[0] : las_files[0], std::move(lasio));
  }

  for (const auto& endpoint : ept_endpoints)
  {
    std::shared_ptr<EPTio>& eptio = ept_cache[endpoint];
    if (!eptio) eptio = std::shared_ptr<EPTio>(new EPTio());
    eptio->set_aoi(chunk.aoi);

    // An endpoint already in the cache is already opened: query() only re-traverses the
    // hierarchy for the new extent, it does not re-parse ept.json or re-probe a tile
    eptio->query(endpoint, chunk.xmin, chunk.ymin, chunk.xmax, chunk.ymax,
                 chunk.buffer, chunk.shape == ShapeType::CIRCLE, filters);
    sources.emplace_back(endpoint, eptio);
  }

  return true;
}
