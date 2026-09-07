#ifndef LASRMULTIREADER_H
#define LASRMULTIREADER_H

#include "Stage.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

class Fileio;

class LASRmultireader: public Stage
{
public:
  LASRmultireader();
  LASRmultireader(const LASRmultireader& other);
  ~LASRmultireader();
  bool process(Header*& header) override;
  bool process(Point*& point) override;
  bool process(PointCloud*& las) override;
  bool set_chunk(Chunk& chunk) override;
  bool need_points() const override { return false; };
  bool is_streamable() const override { return true; };
  void clear(bool) override;

protected:
  // One reader per source of the chunk
  std::vector<std::pair<std::string, std::unique_ptr<Fileio>>> sources;
  virtual bool build_sources(Chunk& chunk) = 0;

private:
  Header* header;
  size_t current_source;
  bool streaming;
};

#endif
