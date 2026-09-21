#include "CRS.h"
#include "print.h"

#include <ogr_spatialref.h>

#include <stdio.h>
#include <algorithm>
#include <limits>
#include <vector>

CRS::CRS()
{
  valid = false;
  epsg = 0;
}

CRS::CRS(int code, bool err)
{
  valid = false;
  epsg = code;
  if (epsg == 0) return;

  CPLPushErrorHandler(CPLQuietErrorHandler);

  if (oSRS.importFromEPSG(epsg) != OGRERR_NONE)
  {
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "EPSG:%d %s\n", epsg, CPLGetLastErrorMsg());
    CPLPopErrorHandler();
    if (err) throw std::runtime_error(buffer);
    return;
  }

  valid = true;

  // Get wkt
  char *pszNewWKT;
  char **papszOptions = nullptr;
  papszOptions = CSLSetNameValue(papszOptions, "FORMAT", "WKT2");
  oSRS.exportToWkt(&pszNewWKT, papszOptions);
  wkt = std::string(pszNewWKT);

  CPLFree(pszNewWKT);
  CSLDestroy(papszOptions);

  CPLPopErrorHandler();
}

CRS::CRS(const std::string& str, bool err)
{
  valid = false;
  epsg = 0;
  wkt = str;
  if (wkt.empty()) return;

  CPLPushErrorHandler(CPLQuietErrorHandler);

  // May not be WKT but still something GDAL understands, such as "EPSG:3857+5703". The
  // limitations keep SetFromUserInput() from treating an unreadable string as a filename to
  // open or a URL to fetch: this constructor also runs on WKT read from LAS/EPT files.
  if (oSRS.importFromWkt(wkt.c_str()) != OGRERR_NONE &&
      oSRS.SetFromUserInput(wkt.c_str(), OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS_get()) != OGRERR_NONE)
  {
    char buffer[2048];
    snprintf(buffer, sizeof(buffer), "WKT string: %s", CPLGetLastErrorMsg());
    CPLPopErrorHandler();
    if (err) throw std::runtime_error(buffer);
    return;
  }

  valid = true;

  const char* authority_code = oSRS.GetAuthorityCode(NULL);
  if (authority_code != NULL)
  {
    epsg = std::stoi(authority_code);
  }

  // SetFromUserInput() accepts shorthand (e.g. "EPSG:3857+5703") that OSRImportFromWkt does
  // not read back; store the canonical WKT so write_las() and other readers recover the CRS.
  char* pszNewWKT;
  char** papszOptions = nullptr;
  papszOptions = CSLSetNameValue(papszOptions, "FORMAT", "WKT2");
  oSRS.exportToWkt(&pszNewWKT, papszOptions);
  wkt = std::string(pszNewWKT);
  CPLFree(pszNewWKT);
  CSLDestroy(papszOptions);

  CPLPopErrorHandler();
}

OGRSpatialReference CRS::get_crs() const
{
  return oSRS;
}

int CRS::get_epsg() const { return epsg; }
std::string CRS::get_wkt() const { return wkt; }
bool CRS::is_valid() const { return valid; }

double CRS::get_linear_units() const
{
  double dfLinearUnitSize;
  const char* pszLinearUnitName;
  dfLinearUnitSize = oSRS.GetLinearUnits(&pszLinearUnitName);
  return dfLinearUnitSize;
}

bool CRS::is_meters() const
{
  return get_linear_units() == 1.0f;
}

bool CRS::is_feets() const
{
  double value = get_linear_units();
  return std::fabs(value - 0.3048) < 1e-4;
}

bool CRS::is_geographic() const
{
  return valid && oSRS.IsGeographic();
}

bool CRS::is_compound() const
{
  return valid && oSRS.IsCompound();
}

// A compound CRS names a vertical CRS, a 3D one such as EPSG:4979 carries ellipsoidal heights
bool CRS::has_vertical() const
{
  return valid && (oSRS.IsCompound() || oSRS.GetAxesCount() == 3);
}

int CRS::get_vertical_epsg() const
{
  if (!is_compound()) return 0;
  const char* code = oSRS.GetAuthorityCode("VERT_CS");
  return (code != nullptr) ? atoi(code) : 0;
}

bool CRS::operator==(const CRS& other) const
{
  if (epsg == other.epsg && valid == other.valid && wkt == other.wkt) return true;

  // The same CRS can be written in several WKT
  return valid && other.valid && oSRS.IsSame(&other.oSRS);
}

// # nocov start
void CRS::dump() const
{

  int err = oSRS.Validate();
  if (err != OGRERR_NONE)
    print("Spatial reference is not valid: error %d\n", err);
  else
    print("Spatial reference is valid.\n");

  print("  EPSG: %d\n", epsg);
  print("  WKT: %s\n", wkt.substr(0,50).c_str());

  return;

  char* pszWKT = nullptr;
  oSRS.exportToPrettyWkt(&pszWKT);
  if (pszWKT)
  {
    print("WKT: %s\n", pszWKT);
    CPLFree(pszWKT);
  }
}

// # nocov end

bool reproject_bbox(const CRS& source, const CRS& target, double& xmin, double& ymin, double& xmax, double& ymax)
{
  if (!source.is_valid() || !target.is_valid()) return false;

  OGRSpatialReference oSourceSRS = source.get_crs();
  OGRSpatialReference oTargetSRS = target.get_crs();

  // Use traditional GIS axis order (x = lon/easting, y = lat/northing) so coordinates
  // are not swapped under modern PROJ authority-compliant axis ordering.
  oSourceSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  oTargetSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

  CPLPushErrorHandler(CPLQuietErrorHandler);
  OGRCoordinateTransformation* ct = OGRCreateCoordinateTransformation(&oSourceSRS, &oTargetSRS);
  CPLPopErrorHandler();

  if (ct == nullptr) return false;

  bool ok = reproject_bbox(ct, xmin, ymin, xmax, ymax);
  OGRCoordinateTransformation::DestroyCT(ct);
  return ok;
}

bool reproject_bbox(OGRCoordinateTransformation* ct, double& xmin, double& ymin, double& xmax, double& ymax)
{
  if (ct == nullptr) return false;

  // Nothing to do for an empty/unset extent.
  if (xmin > xmax || ymin > ymax) return true;

  const int N = 8; // number of samples per edge
  std::vector<double> xs;
  std::vector<double> ys;
  xs.reserve(4 * (N + 1));
  ys.reserve(4 * (N + 1));

  for (int i = 0; i <= N; ++i)
  {
    double tx = xmin + (xmax - xmin) * i / N;
    double ty = ymin + (ymax - ymin) * i / N;

    xs.push_back(tx);   ys.push_back(ymin); // bottom edge
    xs.push_back(tx);   ys.push_back(ymax); // top edge
    xs.push_back(xmin); ys.push_back(ty);   // left edge
    xs.push_back(xmax); ys.push_back(ty);   // right edge
  }

  std::vector<int> ok(xs.size(), 0);
  ct->Transform((int)xs.size(), xs.data(), ys.data(), nullptr, ok.data());

  double nxmin = std::numeric_limits<double>::max();
  double nymin = std::numeric_limits<double>::max();
  double nxmax = std::numeric_limits<double>::lowest();
  double nymax = std::numeric_limits<double>::lowest();
  bool any = false;

  for (size_t i = 0; i < xs.size(); ++i)
  {
    if (!ok[i]) continue;
    any = true;
    nxmin = std::min(nxmin, xs[i]);
    nymin = std::min(nymin, ys[i]);
    nxmax = std::max(nxmax, xs[i]);
    nymax = std::max(nymax, ys[i]);
  }

  if (!any) return false;

  xmin = nxmin;
  ymin = nymin;
  xmax = nxmax;
  ymax = nymax;
  return true;
}


CRS make_compound(const CRS& horizontal, const CRS& vertical)
{
  if (!horizontal.is_valid() || !vertical.is_valid()) return CRS();
  if (horizontal.is_compound()) return CRS();

  OGRSpatialReference h = horizontal.get_crs();
  OGRSpatialReference v = vertical.get_crs();
  OGRSpatialReference compound;

  std::string name = std::string(h.GetName() ? h.GetName() : "unknown") + " + " + (v.GetName() ? v.GetName() : "unknown");

  CPLPushErrorHandler(CPLQuietErrorHandler);
  OGRErr err = compound.SetCompoundCS(name.c_str(), &h, &v);
  CPLPopErrorHandler();

  if (err != OGRERR_NONE) return CRS();

  char* pszWKT = nullptr;
  char** papszOptions = nullptr;
  papszOptions = CSLSetNameValue(papszOptions, "FORMAT", "WKT2");
  compound.exportToWkt(&pszWKT, papszOptions);
  std::string swkt(pszWKT);
  CRS out(swkt);
  CPLFree(pszWKT);
  CSLDestroy(papszOptions);

  return out;
}
