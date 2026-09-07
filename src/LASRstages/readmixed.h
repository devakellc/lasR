#ifndef LASRREADMIXED_H
#define LASRREADMIXED_H

#include "multireader.h"

class LASRmixedreader: public LASRmultireader
{
public:
  std::string get_name() const override { return "reader_mixed"; }

  // multi-threading
  LASRmixedreader* clone() const override { return new LASRmixedreader(*this); };

protected:
  bool build_sources(Chunk& chunk) override;
};

#endif
