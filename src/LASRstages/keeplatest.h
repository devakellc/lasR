#ifndef LASRKEEPLATEST_H
#define LASRKEEPLATEST_H

#include "Stage.h"

class LASRkeeplatest : public Stage
{
public:
  bool process(PointCloud*& las) override;
  double need_buffer() const override { return res; };
  bool set_parameters(const nlohmann::json&) override;
  std::string get_name() const override { return "keep_latest"; };

  // multi-threading
  LASRkeeplatest* clone() const override { return new LASRkeeplatest(*this); };

private:
  double res;
  double window;
  std::string attribute;
};

#endif
