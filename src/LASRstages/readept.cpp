#include "readept.h"

#include "EPTio.h"

bool LASReptreader::build_sources(Chunk& chunk)
{
  for (const auto& file : chunk.main_files)
  {
    auto eptio = std::unique_ptr<EPTio>(new EPTio());
    eptio->query(file, chunk.xmin, chunk.ymin, chunk.xmax, chunk.ymax,
                 chunk.buffer, chunk.shape == ShapeType::CIRCLE, filters);
    sources.emplace_back(file, std::move(eptio));
  }

  return true;
}
