#ifndef CRS_H
#define CRS_H

#include "error.h"
#include <string>
#include <gdal_priv.h>

class CRS
{
public:
  CRS();
  CRS(int, bool warn = false);
  CRS(const std::string&, bool warn = false);
  OGRSpatialReference get_crs() const;
  int get_epsg() const;
  bool is_valid() const;
  double get_linear_units() const;
  bool is_meters() const;
  bool is_feets() const;
  bool is_geographic() const;
  bool is_compound() const;
  bool has_vertical() const;
  int get_vertical_epsg() const;
  void dump() const;
  bool operator==(const CRS& other) const;
  std::string get_wkt() const;

private:
  int epsg;
  bool valid;
  std::string wkt;
  OGRSpatialReference oSRS;
};

// Reproject an axis-aligned bounding box from `source` to `target` CRS. The boundary is
// densified before transforming so a curved (non-affine) reprojection is bounded correctly
// rather than only at the four corners. Returns false if the CRS are invalid, the
// transformation cannot be built, or no sample reprojects (the box is entirely outside the
// transform domain). An empty/unset box (min > max) is left unchanged and returns true.
// Builds and destroys its own transformation; a caller reprojecting many boxes between the
// same pair of CRS should keep one and call the other overload instead.
bool reproject_bbox(const CRS& source, const CRS& target, double& xmin, double& ymin, double& xmax, double& ymax);
bool reproject_bbox(OGRCoordinateTransformation* ct, double& xmin, double& ymin, double& xmax, double& ymax);

// Assemble a compound CRS. Returns an invalid CRS if an input is invalid or already compound.
CRS make_compound(const CRS& horizontal, const CRS& vertical);

#endif
