#ifndef LASRREADEPT_H
#define LASRREADEPT_H

#include "Stage.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class EPTio;

class LASReptreader: public Stage
{
public:
  LASReptreader();
  LASReptreader(const LASReptreader& other);
  ~LASReptreader();
  bool process(Header*& header) override;
  bool process(Point*& point) override;
  bool process(PointCloud*& las) override;
  bool set_chunk(Chunk& chunk) override;
  bool need_points() const override { return false; };
  bool is_streamable() const override { return true; };
  std::string get_name() const override { return "reader_ept"; }
  void clear(bool) override;

  // multi-threading
  LASReptreader* clone() const override { return new LASReptreader(*this); };

private:
  Header* header;
  // One reader per entry of chunk.main_files. Points into ept_cache, which is the
  // owner: an endpoint stays open across chunks instead of every chunk re-opening it.
  std::vector<std::pair<std::string, EPTio*>> sources;
  std::unordered_map<std::string, std::unique_ptr<EPTio>> ept_cache;
  size_t current_source;
  bool streaming;
};

#endif
