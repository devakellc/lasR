#include "transformcrs.h"

#include <ogr_spatialref.h>

#include <cmath>
#include <vector>
#include <limits>
#include <algorithm>

LASRtransformcrs::LASRtransformcrs()
{
  transform = nullptr;
  target_to_source_buffer_scale = 1.0;
  target_to_source_buffer_scale_valid = false;
}

LASRtransformcrs::LASRtransformcrs(const LASRtransformcrs& other) : Stage(other)
{
  source_crs = other.source_crs;
  target_crs = other.target_crs;
  target_to_source_buffer_scale = other.target_to_source_buffer_scale;
  target_to_source_buffer_scale_valid = other.target_to_source_buffer_scale_valid;
  // OGRCoordinateTransformation is not thread-safe: each clone rebuilds its own
  transform = nullptr;
}

LASRtransformcrs::~LASRtransformcrs()
{
  if (transform != nullptr)
  {
    OGRCoordinateTransformation::DestroyCT(transform);
    transform = nullptr;
  }
}

bool LASRtransformcrs::set_parameters(const nlohmann::json& stage)
{
  int epsg = stage.value("epsg", 0);
  std::string wkt = stage.value("wkt", "");

  try
  {
    if (epsg > 0)
      target_crs = CRS(epsg, true);
    else if (!wkt.empty())
      target_crs = CRS(wkt, true);
    else
    {
      last_error = "transform_crs requires a valid 'epsg' code or 'wkt' string";
      return false;
    }
  }
  catch (const std::exception& e)
  {
    last_error = e.what();
    return false;
  }

  if (!target_crs.is_valid())
  {
    last_error = "transform_crs: invalid target CRS";
    return false;
  }

  return true;
}

void LASRtransformcrs::set_crs(const CRS& crs)
{
  source_crs = crs;
  this->crs = target_crs; // downstream stages and writers see the target CRS
}

bool LASRtransformcrs::build_transform()
{
  if (transform != nullptr) return true;

  if (!source_crs.is_valid())
  {
    last_error = "transform_crs: the source CRS is unknown. Add set_crs() upstream or read files that carry a CRS.";
    return false;
  }

  if (!target_crs.is_valid())
  {
    last_error = "transform_crs: the target CRS is invalid.";
    return false;
  }

  OGRSpatialReference oSourceSRS = source_crs.get_crs();
  OGRSpatialReference oTargetSRS = target_crs.get_crs();

  // Traditional GIS axis order (x = lon/easting) avoids a swap under PROJ's default authority order
  oSourceSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  oTargetSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  CPLPushErrorHandler(CPLQuietErrorHandler);
  transform = OGRCreateCoordinateTransformation(&oSourceSRS, &oTargetSRS);
  CPLPopErrorHandler();

  if (transform == nullptr)
  {
    last_error = "transform_crs: failed to create a coordinate transformation between the source and target CRS.";
    return false;
  }

  return true;
}

void LASRtransformcrs::get_extent(double& xmin, double& ymin, double& xmax, double& ymax)
{
  // Reproject the coverage extent so downstream stages (e.g. rasterize's master raster)
  // are sized in the target CRS; left unchanged when the source CRS is not known yet
  if (source_crs.is_valid() && target_crs.is_valid())
  {
    const double sxmin = xmin, symin = ymin, sxmax = xmax, symax = ymax;
    if (reproject_bbox(source_crs, target_crs, xmin, ymin, xmax, ymax))
    {
      const double src_diag = std::hypot(sxmax - sxmin, symax - symin);
      const double tgt_diag = std::hypot(xmax - xmin, ymax - ymin);
      if (src_diag > 0 && tgt_diag > 0)
      {
        target_to_source_buffer_scale = src_diag / tgt_diag;
        target_to_source_buffer_scale_valid = true;
      }

      this->xmin = xmin;
      this->ymin = ymin;
      this->xmax = xmax;
      this->ymax = ymax;
    }
  }
}

double LASRtransformcrs::translate_buffer_to_input(double downstream_buffer) const
{
  if (!target_to_source_buffer_scale_valid) return downstream_buffer;

  // A downstream halo (e.g. triangulate()'s 20 m) is in target units; convert back to source
  // units for the reader. Degrees are not a distance, so a geographic side is left unscaled
  if (source_crs.is_geographic() && target_crs.is_geographic())
    return downstream_buffer;

  if (target_crs.is_geographic() && !source_crs.is_geographic())
    return downstream_buffer;

  return downstream_buffer * target_to_source_buffer_scale;
}

bool LASRtransformcrs::set_chunk(Chunk& chunk)
{
  Stage::set_chunk(chunk);

  // chunk.crs carries the CRS an earlier CRS-changing stage left, or the file's own CRS
  if (chunk.crs.is_valid() && !(chunk.crs == source_crs))
  {
    source_crs = chunk.crs;

    if (transform != nullptr)
    {
      OGRCoordinateTransformation::DestroyCT(transform);
      transform = nullptr;
    }
  }

  if (source_crs.is_valid() && target_crs.is_valid())
  {
    // Built here rather than lazily in process() so an unbuildable pair fails at this chunk
    if (!build_transform()) return false;

    const double sxmin = chunk.xmin, symin = chunk.ymin, sxmax = chunk.xmax, symax = chunk.ymax;
    double x0 = sxmin, y0 = symin, x1 = sxmax, y1 = symax;

    if (reproject_bbox(transform, x0, y0, x1, y1))
    {
      // The reader wants the buffer in source units; downstream stages want it in target units
      const double src_diag = std::hypot(sxmax - sxmin, symax - symin);
      const double tgt_diag = std::hypot(x1 - x0, y1 - y0);
      if (src_diag > 0 && tgt_diag > 0 &&
          !(source_crs.is_geographic() && target_crs.is_geographic()))
      {
        chunk.buffer *= tgt_diag / src_diag;
      }
      buffer = chunk.buffer;

      this->xmin = x0;
      this->ymin = y0;
      this->xmax = x1;
      this->ymax = y1;

      chunk.xmin = x0;
      chunk.ymin = y0;
      chunk.xmax = x1;
      chunk.ymax = y1;
    }
    else
    {
      // Outside the transform domain: leave the extent as-is, process() drops the points
      warning("transform_crs: could not reproject a chunk extent (outside the transformation domain).\n");
    }
  }

  // A later stage, including another transform_crs, must see the CRS this stage produces
  if (target_crs.is_valid()) chunk.crs = target_crs;

  return true;
}

bool LASRtransformcrs::process(PointCloud*& las)
{
  if (las == nullptr || las->npoints == 0) return true;

  if (!build_transform()) return false;

  AttributeSchema& schema = las->header->schema;
  Attribute& attr_x = schema.attributes[AttributeCore::X];
  Attribute& attr_y = schema.attributes[AttributeCore::Y];

  // get_x()/get_y() and the writers only decode INT32, FLOAT and DOUBLE; anything else reads as 0
  auto is_supported = [](AttributeType t)
  { return t == AttributeType::INT32 || t == AttributeType::FLOAT || t == AttributeType::DOUBLE; };
  if (!is_supported(attr_x.type) || !is_supported(attr_y.type))
  {
    last_error = "transform_crs: unsupported X/Y storage type (expected int, float or double).";
    return false;
  }

  // LAS stores X/Y as scaled int32; PCD and other formats store them directly as float/double
  const bool x_int = (attr_x.type == AttributeType::INT32);
  const bool y_int = (attr_y.type == AttributeType::INT32);

  // Z is preserved as-is, matching gdaltransform/sf/terra outside an explicit vertical CRS

  // A projected scale (e.g. 0.01 m) reused for a geographic target gives ~1 km resolution, and
  // a geographic scale (e.g. 1e-7 deg) reused for a projected target overflows the int32 range,
  // so the scale is picked for the target rather than kept from the source -- except int32 ->
  // projected, where the source scale is a real quantization step worth keeping
  double new_sx;
  double new_sy;
  if (target_crs.is_geographic())
  {
    new_sx = new_sy = 1e-7; // ~1.1 cm at the equator
  }
  else if (x_int && y_int && !source_crs.is_geographic())
  {
    // Projected -> projected with real (INT32) scales: keep them.
    new_sx = attr_x.scale_factor;
    new_sy = attr_y.scale_factor;
  }
  else
  {
    // Projected target from a geographic source, or float/double storage whose schema scale is
    // a placeholder: 1 cm is a sensible default resolution for a projected CRS.
    new_sx = new_sy = 0.01;
  }

  // An offset near the reprojected data keeps the stored/written integers small; write_las()
  // needs it for float/double storage too. The bbox centroid can itself be outside the
  // transform domain (e.g. a UTM-zone boundary), so probe it, then the corners and edge
  // midpoints, and keep the first that reprojects; 0 if none do (every point is dropped below)
  double ox = 0.0;
  double oy = 0.0;
  {
    const double mnx = las->header->min_x, mxx = las->header->max_x;
    const double mny = las->header->min_y, mxy = las->header->max_y;
    const double cx = (mnx + mxx) / 2, cy = (mny + mxy) / 2;
    const double cand_x[] = { cx, mnx, mxx, mnx, mxx, cx,  cx,  mnx, mxx };
    const double cand_y[] = { cy, mny, mny, mxy, mxy, mny, mxy, cy,  cy  };
    for (size_t i = 0; i < sizeof(cand_x) / sizeof(cand_x[0]); ++i)
    {
      double tx = cand_x[i], ty = cand_y[i];
      if (transform->Transform(1, &tx, &ty, nullptr)) { ox = tx; oy = ty; break; }
    }
  }

  // Batched: OGRCoordinateTransformation's per-call overhead makes arrays much faster
  const size_t BATCH = 65536;
  std::vector<double> xs(BATCH), ys(BATCH);
  std::vector<unsigned char*> ptrs(BATCH);
  std::vector<int> ok(BATCH);

  size_t n_outside = 0; // dropped: outside the transformation domain
  size_t n_range = 0;   // dropped: not representable as a 32-bit integer

  // Stores one reprojected coordinate; false if an int32 target would overflow, so the caller
  // can drop the point rather than wrap it to a garbage location
  auto store = [](unsigned char* base, const Attribute& a, bool is_int, double value, double new_s, double off) -> bool
  {
    unsigned char* ptr = base + a.offset;
    if (is_int)
    {
      const double scaled = (value - off) / new_s;
      const double max_i = static_cast<double>(std::numeric_limits<int>::max()) + 0.5;
      const double min_i = static_cast<double>(std::numeric_limits<int>::min()) - 0.5;
      if (!std::isfinite(scaled) || scaled > max_i || scaled < min_i) return false;
      long long raw = std::llround(scaled);
      if (raw > std::numeric_limits<int>::max() || raw < std::numeric_limits<int>::min()) return false;
      *reinterpret_cast<int*>(ptr) = static_cast<int>(raw);
    }
    else if (a.type == AttributeType::FLOAT)
      *reinterpret_cast<float*>(ptr) = static_cast<float>(value);
    else // DOUBLE
      *reinterpret_cast<double*>(ptr) = value;
    return true;
  };

  Point p;
  p.set_schema(&schema);

  auto flush = [&](size_t count)
  {
    if (count == 0) return;
    transform->Transform((int)count, xs.data(), ys.data(), nullptr, ok.data());
    for (size_t i = 0; i < count; ++i)
    {
      p.data = ptrs[i];
      if (!ok[i])
      {
        // A point outside the transform domain is dropped.
        p.set_deleted();
        n_outside++;
        continue;
      }
      bool ok_x = store(ptrs[i], attr_x, x_int, xs[i], new_sx, ox);
      bool ok_y = store(ptrs[i], attr_y, y_int, ys[i], new_sy, oy);
      if (!ok_x || !ok_y)
      {
        p.set_deleted();
        n_range++;
        continue;
      }
      // Z is preserved: its raw value, scale and offset are left unchanged.
    }
  };

  size_t n = 0;
  while (las->read_point())
  {
    xs[n] = las->point.get_x();
    ys[n] = las->point.get_y();
    ptrs[n] = las->point.data;
    n++;
    if (n == BATCH) { flush(n); n = 0; }
  }
  flush(n);

  // write_las() quantizes to LAS int32 from these regardless of source storage type
  las->header->x_scale_factor = new_sx;
  las->header->y_scale_factor = new_sy;
  las->header->x_offset = ox;
  las->header->y_offset = oy;

  // The in-memory schema scale/offset matter only for int32 storage; float/double accessors
  // expect identity scale/offset, and write_las() reads the header ones for those axes instead
  if (x_int)
  {
    attr_x.scale_factor = new_sx;
    attr_x.value_offset = ox;
  }
  if (y_int)
  {
    attr_y.scale_factor = new_sy;
    attr_y.value_offset = oy;
  }

  las->header->crs = target_crs;

  const size_t n_dropped = n_outside + n_range;
  if (n_dropped > 0) las->delete_deleted();

  las->update_header();

  if (las->npoints == 0)
  {
    // update_header() leaves an inverted bbox (min > max) for an empty cloud; reset it so an
    // empty result is not propagated downstream as a corrupt extent
    las->header->min_x = las->header->max_x = ox;
    las->header->min_y = las->header->max_y = oy;
    las->header->min_z = las->header->max_z = 0.0;
    warning("transform_crs: all %lu point(s) fell outside the target CRS domain; the output is empty.\n", (unsigned long)n_dropped);
  }
  else if (n_dropped > 0)
  {
    if (n_range > 0)
      warning("transform_crs: dropped %lu point(s) outside the transformation domain and %lu point(s) not representable in the target CRS.\n", (unsigned long)n_outside, (unsigned long)n_range);
    else
      warning("transform_crs: dropped %lu point(s) outside the transformation domain.\n", (unsigned long)n_outside);
  }

  return true;
}
