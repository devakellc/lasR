#ifndef LASRREADEPT_H
#define LASRREADEPT_H

#include "multireader.h"

#include <memory>
#include <string>
#include <unordered_map>

class EPTio;

class LASReptreader: public LASRmultireader
{
public:
  std::string get_name() const override { return "reader_ept"; }

  // multi-threading
  LASReptreader* clone() const override { return new LASReptreader(*this); };

protected:
  bool build_sources(Chunk& chunk) override;

private:
  std::unordered_map<std::string, std::shared_ptr<EPTio>> ept_cache;
};

#endif
