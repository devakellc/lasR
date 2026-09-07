#include "keeplatest.h"
#include "Grid.h"

#include <limits>
#include <vector>

bool LASRkeeplatest::set_parameters(const nlohmann::json& stage)
{
  res = stage.at("res");
  window = stage.value("window", 3600.0);
  attribute = stage.value("use_attribute", "gpstime");

  if (res <= 0)
  {
    last_error = "the resolution must be strictly positive";
    return false;
  }

  if (window <= 0)
  {
    last_error = "the window must be strictly positive";
    return false;
  }

  return true;
}

bool LASRkeeplatest::process(PointCloud*& las)
{
  if (!las->header->schema.has_attribute(attribute))
  {
    last_error = "No attribute '" + attribute + "' found";
    return false;
  }

  AttributeAccessor accessor(attribute);

  Grid grid(las->header->min_x, las->header->min_y, las->header->max_x, las->header->max_y, res);
  std::vector<double> latest(grid.get_ncells(), std::numeric_limits<double>::lowest());

  while (las->read_point())
  {
    if (pointfilter.filter(&las->point)) continue;

    int cell = grid.cell_from_xy(las->point.get_x(), las->point.get_y());
    double t = accessor(&las->point);
    if (t > latest[cell]) latest[cell] = t;
  }

  while (las->read_point())
  {
    if (pointfilter.filter(&las->point)) continue;

    int cell = grid.cell_from_xy(las->point.get_x(), las->point.get_y());
    if (latest[cell] - accessor(&las->point) > window) las->point.set_deleted();
  }

  las->update_header();
  las->delete_deleted();

  return true;
}
