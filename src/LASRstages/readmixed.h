#ifndef LASRREADMIXED_H
#define LASRREADMIXED_H

#include "multireader.h"

#include <memory>
#include <string>
#include <unordered_map>

class EPTio;

class LASRmixedreader: public LASRmultireader
{
public:
  std::string get_name() const override { return "reader_mixed"; }

  // multi-threading
  LASRmixedreader* clone() const override { return new LASRmixedreader(*this); };

protected:
  bool build_sources(Chunk& chunk) override;

private:
  // Endpoints stay open across chunks so a query only re-parses ept.json and
  // re-probes the hierarchy once per endpoint, not once per chunk
  std::unordered_map<std::string, std::shared_ptr<EPTio>> ept_cache;
};

#endif
