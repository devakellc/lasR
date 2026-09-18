#include "setcrs.h"

LASRsetcrs::LASRsetcrs()
{
  crs = CRS();
}

bool LASRsetcrs::set_parameters(const nlohmann::json& stage)
{
  int epsg = stage.value("epsg", 0);
  std::string wkt = stage.value("wkt", "");

  try
  {
    if (epsg > 0) crs = CRS(epsg, true);
    else if (wkt.size() > 0) crs = CRS(wkt, true);
  }
  catch(const std::exception& e)
  {
    last_error = e.what();
    return false;
  }

  return true;
}

bool LASRsetcrs::process(Header*& header)
{
  header->crs = crs;
  return true;
}

bool LASRsetcrs::set_chunk(Chunk& chunk)
{
  Stage::set_chunk(chunk);
  // Declares the CRS a later stage, including transform_crs, must trust over the file's own
  if (crs.is_valid()) chunk.crs = crs;
  return true;
}