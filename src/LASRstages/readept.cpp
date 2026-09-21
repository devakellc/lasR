#include "readept.h"

#include "EPTio.h"

bool LASReptreader::build_sources(Chunk& chunk)
{
  for (size_t i = 0 ; i < chunk.main_files.size() ; i++)
  {
    const std::string& file = chunk.main_files[i];

    // Keyed by position too: the same endpoint can be listed twice in a chunk
    std::shared_ptr<EPTio>& eptio = ept_cache[std::to_string(i) + ":" + file];
    if (!eptio) eptio = std::shared_ptr<EPTio>(new EPTio());

    eptio->query(file, chunk.xmin, chunk.ymin, chunk.xmax, chunk.ymax,
                 chunk.buffer, chunk.shape == ShapeType::CIRCLE, filters);
    sources.emplace_back(file, eptio);
  }

  return true;
}
