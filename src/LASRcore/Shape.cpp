#include "Shape.h"

#include <utility>    // std::swap
#include <functional> // std::hash
#include <cmath>
#include <limits>

#include <ogr_geometry.h>

/* ====================
 * POINT
 * ====================*/

PointXY::PointXY() : x(0), y(0) {}
PointXY::PointXY(double x, double y) : x(x), y(y) {}

size_t PointXY::hash() const
{
  // Create individual hashes for x and y
  size_t hashX = std::hash<double>{}(x);
  size_t hashY = std::hash<double>{}(y);

  // Combine the individual hashes using a hash combiner
  size_t seed = 0;
  seed ^= hashX + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  seed ^= hashY + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  return seed;
}

/*bool PointXY::operator<(const PointXY& other) const
{
  if (x != other.x) return x < other.x;
  else return y < other.y;
}


bool PointXYZ::operator<(const PointXYZ& other) const
{
  if (x != other.x) return x < other.x;
  else if (y != other.y) return y < other.y;
  else return z < other.z;
}*/

bool PointXY::operator==(const PointXY& other) const
{
  return (x == other.x) && (y == other.y);
}

PointXYI::PointXYI() : PointXY(), i(0) {}
PointXYI::PointXYI(double x, double y) : PointXY(x,y), i(0) {}
PointXYI::PointXYI(double x, double y, unsigned int i) : PointXY(x,y), i(i) {}

PointXYZ::PointXYZ() : PointXY(), z(0) {}
PointXYZ::PointXYZ(double x, double y) : PointXY(x,y), z(0) {}
PointXYZ::PointXYZ(double x, double y, double z) : PointXY(x,y), z(z) {}

double PointXYZ::distance(const PointXYZ& p) const
{
  return std::sqrt(pow(x - p.x, 2) + pow(y - p.y, 2) + pow(z - p.z, 2));
}

PointXYZ PointXYZ::operator-(const PointXYZ &other) const
{
  return PointXYZ(x - other.x, y - other.y, z - other.z);
}

PointXYZ PointXYZ::operator*(double scalar) const
{
  return PointXYZ(x * scalar, y * scalar, z * scalar);
}

double PointXYZ::dot(const PointXYZ &other) const
{
  return x * other.x + y * other.y + z * other.z;
}

PointXYZ PointXYZ::operator+(const PointXYZ &other) const
{
  return PointXYZ(x + other.x, y + other.y, z + other.z);
}

bool PointXYZ::operator==(const PointXYZ& other) const
{
  return (x == other.x) && (y == other.y) && (z == other.z);
}

/* ====================
 * SHAPE
 * ====================*/

Shape::~Shape() {}
Shape3D::~Shape3D() {}

/* ====================
 * TRIANGLE
 * ====================*/

TriangleXYZ::TriangleXYZ(const Point& a, const Point& b, const Point& c)
{
  A.x = a.get_x();
  A.y = a.get_y();
  A.z = a.get_z();

  B.x = b.get_x();
  B.y = b.get_y();
  B.z = b.get_z();

  C.x = c.get_x();
  C.y = c.get_y();
  C.z = c.get_z();
}

TriangleXYZ::TriangleXYZ(const PointXYZ& A, const PointXYZ& B, const PointXYZ& C)
{
  this->A = A;
  this->B = B;
  this->C = C;
}

void TriangleXYZ::make_clock_wise()
{
  if (orientation() == COUNTERCLOCKWISE) std::swap(this->B, this->C);
}

void TriangleXYZ::make_counter_clock_wise()
{
  if (orientation() == CLOCKWISE) std::swap(this->B, this->C);
}

PointXYZ TriangleXYZ::normal() const
{
  // Compute edges
  PointXYZ edge1(B.x - A.x, B.y - A.y, B.z - A.z);
  PointXYZ edge2(C.x - A.x, C.y - A.y, C.z - A.z);

  // Compute cross product
  PointXYZ n;
  n.x = edge1.y * edge2.z - edge1.z * edge2.y;
  n.y = edge1.z * edge2.x - edge1.x * edge2.z;
  n.z = edge1.x * edge2.y - edge1.y * edge2.x;

  // Normalize the vector
  double length = sqrt(n.dot(n));
  n.x /= length;
  n.y /= length;
  n.z /= length;

  // Ensure the z-component is oriented upward
  /*if (n.z < 0)
  {
    n.x = -n.x;
    n.y = -n.y;
    n.z = -n.z;
  }*/

  return n;
}

PointXYZ TriangleXYZ::centroid() const
{
  PointXYZ centroid;
  centroid.x = (A.x + B.x + C.x)/3;
  centroid.y = (A.y + B.y + C.y)/3;
  centroid.z = (A.z + B.z + C.z)/3;
  return centroid;
}

bool TriangleXYZ::contains(const PointXY& P) const
{
  if (P.x < xmin() - EPSILON || P.x > xmax() + EPSILON || P.y < ymin() - EPSILON || P.y > ymax() + EPSILON)
    return false;

  // move to (0,0) to gain arithmetic precision
  double x_offset = xmin();
  double y_offset = ymin();
  PointXY a(A.x - x_offset, A.y - y_offset);
  PointXY b(B.x - x_offset, B.y - y_offset);
  PointXY c(C.x - x_offset, C.y - y_offset);
  PointXY p(P.x - x_offset, P.y - y_offset);

  double denominator = (a.x*(b.y - c.y) + a.y*(c.x - b.x) + b.x*c.y - b.y*c.x);
  double t1 = (p.x*(c.y - a.y) + p.y*(a.x - c.x) - a.x*c.y + a.y*c.x) / denominator;
  double t2 = (p.x*(b.y - a.y) + p.y*(a.x - b.x) - a.x*b.y + a.y*b.x) / -denominator;
  double s = t1 + t2;

  if (0 <= t1 && t1 <= 1 && 0 <= t2 && t2 <= 1 && s <= 1)
    return true;

  // see http://totologic.blogspot.com/2014/01/accurate-point-in-triangle-test.html
  if (square_distance_point_to_segment(a, b, p) <= EPSILON) return true;
  if (square_distance_point_to_segment(b, c, p) <= EPSILON) return true;
  if (square_distance_point_to_segment(c, a, p) <= EPSILON) return true;

  return false;
}

bool TriangleXYZ::contains(double x, double y) const
{
  PointXY p(x, y);
  return contains(p);
}

double TriangleXYZ::square_max_edge_size() const
{
  // ABC is resented by vector u,v and w
  PointXYZ u(A.x - B.x, A.y - B.y);
  PointXYZ v(A.x - C.x, A.y - C.y);
  PointXYZ w(B.x - C.x, B.y - C.y);

  // Compute the AB, AC, BC edge comparable length
  double edge_AB = u.x * u.x + u.y * u.y;
  double edge_AC = v.x * v.x + v.y * v.y;
  double edge_BC = w.x * w.x + w.y * w.y;
  return MAX3(edge_AB, edge_AC, edge_BC);
}

void TriangleXYZ::linear_interpolation(PointXYZ& p) const
{
  double dx1 = p.x - A.x;
  double dy1 = p.y - A.y;
  double dx2 = B.x - A.x;
  double dy2 = B.y - A.y;
  double dx3 = C.x - A.x;
  double dy3 = C.y - A.y;
  p.z = A.z + ((dy1 * dx3 - dx1 * dy3) * (B.z - A.z) + (dx1 * dy2 - dy1 * dx2) * (C.z - A.z)) / (dx3 * dy2 - dx2 * dy3);
}

double TriangleXYZ::square_distance_point_to_segment(const PointXY& p1, const PointXY& p2, const PointXY& p) const
{
  double p1_p2_squareLength = (p2.x - p1.x)*(p2.x - p1.x) + (p2.y - p1.y)*(p2.y - p1.y);
  double dot_product = ((p.x - p1.x)*(p2.x - p1.x) + (p.y - p1.y)*(p2.y - p1.y)) / p1_p2_squareLength;

  if ( dot_product < 0 )
  {
    return (p.x - p1.x)*(p.x - p1.x) + (p.y - p1.y)*(p.y - p1.y);
  }
  else if ( dot_product <= 1 )
  {
    double p_p1_squareLength = (p1.x - p.x)*(p1.x - p.x) + (p1.y - p.y)*(p1.y - p.y);
    return p_p1_squareLength - dot_product * dot_product * p1_p2_squareLength;
  }
  else
  {
    return (p.x - p2.x)*(p.x - p2.x) + (p.y - p2.y)*(p.y - p2.y);
  }
}

int TriangleXYZ::orientation() const
{
  double val = (B.x - A.x) * (C.y - B.y) - (B.y - A.y) * (C.x - B.x);
  if (val == 0) return COLINEAR;
  return (val > 0) ? CLOCKWISE : COUNTERCLOCKWISE;
}

/* ====================
 * Edge
 * ====================*/

/*Edge::Edge(double x1, double y1, double x2, double y2)
{
  A.x = x1;
  B.x = x2;
  A.y = y1;
  B.y = y2;
}*/

/*bool Edge::operator<(const Edge& other) const
{
  if (xmin() != other.xmin()) return xmin() < other.xmin();
  if (ymin() != other.ymin()) return ymin() < other.ymin();
  if (xmax() != other.xmax()) return xmax() < other.xmax();
  return ymax() < other.ymax();
}*/

bool Edge::operator==(const Edge& other) const
{
  return (A == other.A && B == other.B) || (A == other.B && B == other.A);
}

std::size_t Edge::hash() const
{
  size_t hashX = std::hash<double>{}(xmin());
  size_t hashY = std::hash<double>{}(ymax());

  size_t seed = 0;
  seed ^= hashX + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  seed ^= hashY + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  return seed;
}

/* ====================
 * POLYGON
 * ====================*/

PolygonXY::PolygonXY(const std::vector<PointXY>& coords)
{
  coordinates = coords;
}

void PolygonXY::push_back(const PointXY& p)
{
  coordinates.push_back(p);
}

bool PolygonXY::is_closed() const
{
  if (coordinates.size() == 0) return false;
  return &coordinates.front() == &coordinates.back();
}

void PolygonXY::close()
{
  if (!is_closed()) coordinates.push_back(coordinates.front());
}

bool PolygonXY::is_clockwise() const
{
  return signed_area() < 0;
}

double PolygonXY::signed_area() const
{
  if (coordinates.size() < 3) return 0.0;
  double area = 0.0;
  for (size_t i = 0, j = coordinates.size() - 1; i < coordinates.size(); j = i++) {
    area += (coordinates[j].x * coordinates[i].y - coordinates[i].x * coordinates[j].y);
  }
  return 0.5 * area;
}


void PolygonXY::make_clockwise() {
  if (!is_clockwise()) {
    std::reverse(coordinates.begin(), coordinates.end());
  }
}

void PolygonXY::make_counterclockwise() {
  if (is_clockwise()) {
    std::reverse(coordinates.begin(), coordinates.end());
  }
}

// Simple point-in-polygon check using ray casting
bool PolygonXY::contains(const PointXY& p) const
{
  size_t n = coordinates.size();
  if (n < 3) return false; // degenerate polygon

  bool inside = false;
  for (size_t i = 0, j = n - 1; i < n; j = i++)
  {
    const auto& pi = coordinates[i];
    const auto& pj = coordinates[j];

    // Check if point is on an edge (optional, can be treated as inside)
    if ((std::min(pi.y, pj.y) <= p.y && p.y <= std::max(pi.y, pj.y)) &&
        (std::min(pi.x, pj.x) <= p.x && p.x <= std::max(pi.x, pj.x)))
    {
      // Check if point is exactly on the line segment
      double dx = pj.x - pi.x;
      double dy = pj.y - pi.y;
      if (std::abs(dy * (p.x - pi.x) - dx * (p.y - pi.y)) < 1e-12) {
        return true; // point is on edge
      }
    }

    // Ray casting
    if ((pi.y > p.y) != (pj.y > p.y))
    {
      double x_intersect = pi.x + (p.y - pi.y) * (pj.x - pi.x) / (pj.y - pi.y);
      if (p.x < x_intersect) inside = !inside;
    }
  }

  return inside;
}


bool PolygonXY::contains(const PolygonXY& other) const
{
  if (other.coordinates.empty()) return false;
  for (const auto& pt : other.coordinates)
  {
    if (!contains(pt)) return false;
  }
  return true;
}

/* ====================
 * POLYGON SHAPE
 * ====================*/

// Liang-Barsky clipping: the edge meets the box when the parametric ranges of the two slabs overlap
static bool edge_crosses_box(const Edge& e, double xmin, double ymin, double xmax, double ymax)
{
  double dx = e.B.x - e.A.x;
  double dy = e.B.y - e.A.y;
  double p[4] = {-dx, dx, -dy, dy};
  double q[4] = {e.A.x-xmin, xmax-e.A.x, e.A.y-ymin, ymax-e.A.y};
  double t0 = 0;
  double t1 = 1;

  for (int i = 0 ; i < 4 ; i++)
  {
    if (p[i] == 0)
    {
      if (q[i] < 0) return false;
      continue;
    }

    double t = q[i]/p[i];

    if (p[i] < 0)
    {
      if (t > t1) return false;
      if (t > t0) t0 = t;
    }
    else
    {
      if (t < t0) return false;
      if (t < t1) t1 = t;
    }
  }

  return true;
}

PolygonShape::PolygonShape()
{
  band_height = 0;
  minx = std::numeric_limits<double>::max();
  miny = std::numeric_limits<double>::max();
  maxx = std::numeric_limits<double>::lowest();
  maxy = std::numeric_limits<double>::lowest();
}

void PolygonShape::add_ring(const PolygonXY& ring)
{
  size_t n = ring.coordinates.size();
  if (n < 3) return;

  for (size_t i = 0, j = n-1 ; i < n ; j = i++)
  {
    const PointXY& A = ring.coordinates[j];
    const PointXY& B = ring.coordinates[i];

    minx = MIN(minx, B.x);
    miny = MIN(miny, B.y);
    maxx = MAX(maxx, B.x);
    maxy = MAX(maxy, B.y);

    // A closed ring repeats its first vertex. The resulting edge has no length and would divide by
    // zero when measuring a distance
    if (A.x == B.x && A.y == B.y) continue;

    edges.push_back(Edge(A, B));
  }
}

void PolygonShape::build()
{
  bands.clear();
  if (edges.empty()) return;

  // A few edges per band: enough to shorten the ray casting without inflating the index when a
  // single edge spans the whole height
  int nbands = MAX(1, MIN(512, (int)edges.size()/2));
  band_height = (maxy > miny) ? (maxy-miny)/nbands : 0;
  if (band_height == 0) nbands = 1;

  bands.resize(nbands);
  for (size_t i = 0 ; i < edges.size() ; i++)
  {
    int first = band_of(edges[i].ymin());
    int last = band_of(edges[i].ymax());
    for (int b = first ; b <= last ; b++) bands[b].push_back((int)i);
  }
}

int PolygonShape::band_of(double y) const
{
  if (band_height == 0) return 0;
  int b = (int)((y-miny)/band_height);
  if (b < 0) b = 0;
  if (b >= (int)bands.size()) b = (int)bands.size()-1;
  return b;
}

double PolygonShape::square_distance_to_edge(const Edge& e, double x, double y)
{
  double dx = e.B.x - e.A.x;
  double dy = e.B.y - e.A.y;
  double t = ((x-e.A.x)*dx + (y-e.A.y)*dy) / (dx*dx + dy*dy);

  if (t < 0) t = 0;
  if (t > 1) t = 1;

  double px = e.A.x + t*dx - x;
  double py = e.A.y + t*dy - y;
  return px*px + py*py;
}

bool PolygonShape::contains(double x, double y) const
{
  if (x < minx || x > maxx || y < miny || y > maxy) return false;
  if (bands.empty()) return false;

  bool inside = false;
  for (int i : bands[band_of(y)])
  {
    const PointXY& A = edges[i].A;
    const PointXY& B = edges[i].B;

    if ((A.y > y) != (B.y > y))
    {
      double xintersect = A.x + (y-A.y)*(B.x-A.x)/(B.y-A.y);
      if (x < xintersect) inside = !inside;
    }
  }

  return inside;
}

// Distance to the boundary only. Containment is a separate question, asked first by the callers so
// that the scan below runs on the few points falling outside
bool PolygonShape::near_boundary(double x, double y, double distance) const
{
  if (bands.empty()) return false;
  if (x < minx-distance || x > maxx+distance || y < miny-distance || y > maxy+distance) return false;

  double square_distance = distance*distance;
  int first = band_of(y-distance);
  int last = band_of(y+distance);

  for (int b = first ; b <= last ; b++)
    for (int i : bands[b])
      if (square_distance_to_edge(edges[i], x, y) <= square_distance) return true;

  return false;
}

bool PolygonShape::intersects(double xmin, double ymin, double xmax, double ymax) const
{
  if (xmax < minx || xmin > maxx || ymax < miny || ymin > maxy) return false;
  if (bands.empty()) return false;

  // A corner inside the shape covers a box swallowed by the polygon, an edge crossing the box covers
  // both a partial overlap and a polygon swallowed by the box. A box lying in a hole matches neither
  if (contains(xmin, ymin) || contains(xmax, ymin) || contains(xmin, ymax) || contains(xmax, ymax)) return true;

  int first = band_of(ymin);
  int last = band_of(ymax);

  for (int b = first ; b <= last ; b++)
    for (int i : bands[b])
      if (edge_crosses_box(edges[i], xmin, ymin, xmax, ymax)) return true;

  return false;
}

PolygonShape* PolygonShape::from_wkt(const std::string& wkt, std::string& error)
{
  OGRGeometry* geometry = nullptr;
  const char* text = wkt.c_str();

  if (OGRGeometryFactory::createFromWkt(&text, nullptr, &geometry) != OGRERR_NONE || geometry == nullptr)
  {
    error = "cannot parse the geometry '" + wkt + "'";
    return nullptr;
  }

  std::vector<OGRPolygon*> polygons;
  OGRwkbGeometryType type = wkbFlatten(geometry->getGeometryType());

  if (type == wkbPolygon)
  {
    polygons.push_back(geometry->toPolygon());
  }
  else if (type == wkbMultiPolygon)
  {
    OGRMultiPolygon* multi = geometry->toMultiPolygon();
    for (int i = 0 ; i < multi->getNumGeometries() ; i++)
    {
      OGRGeometry* part = multi->getGeometryRef(i);
      if (wkbFlatten(part->getGeometryType()) == wkbPolygon) polygons.push_back(part->toPolygon());
    }
  }
  else
  {
    error = "the geometry must be a POLYGON or a MULTIPOLYGON";
    OGRGeometryFactory::destroyGeometry(geometry);
    return nullptr;
  }

  PolygonShape* shape = new PolygonShape;

  for (const auto* polygon : polygons)
  {
    for (int i = -1 ; i < polygon->getNumInteriorRings() ; i++)
    {
      const OGRLinearRing* ring = (i < 0) ? polygon->getExteriorRing() : polygon->getInteriorRing(i);
      if (ring == nullptr) continue;

      PolygonXY coordinates;
      for (int j = 0 ; j < ring->getNumPoints() ; j++) coordinates.push_back(PointXY(ring->getX(j), ring->getY(j)));
      shape->add_ring(coordinates);
    }
  }

  OGRGeometryFactory::destroyGeometry(geometry);

  if (shape->edges.empty())
  {
    error = "the geometry has no ring";
    delete shape;
    return nullptr;
  }

  shape->build();

  return shape;
}

bool wkt_part_bboxes(const std::string& wkt, std::vector<double>& xmin, std::vector<double>& ymin, std::vector<double>& xmax, std::vector<double>& ymax, std::string& error)
{
  OGRGeometry* geometry = nullptr;
  const char* text = wkt.c_str();

  if (OGRGeometryFactory::createFromWkt(&text, nullptr, &geometry) != OGRERR_NONE || geometry == nullptr)
  {
    error = "cannot parse the geometry '" + wkt + "'";
    return false;
  }

  std::vector<OGRGeometry*> parts;
  OGRwkbGeometryType type = wkbFlatten(geometry->getGeometryType());

  if (type == wkbPolygon)
  {
    parts.push_back(geometry);
  }
  else if (type == wkbMultiPolygon)
  {
    OGRMultiPolygon* multi = geometry->toMultiPolygon();
    for (int i = 0 ; i < multi->getNumGeometries() ; i++)
    {
      OGRGeometry* part = multi->getGeometryRef(i);
      if (wkbFlatten(part->getGeometryType()) == wkbPolygon) parts.push_back(part);
    }
  }
  else
  {
    error = "the geometry must be a POLYGON or a MULTIPOLYGON";
    OGRGeometryFactory::destroyGeometry(geometry);
    return false;
  }

  for (const auto* part : parts)
  {
    if (part->IsEmpty()) continue;

    OGREnvelope envelope;
    part->getEnvelope(&envelope);
    xmin.push_back(envelope.MinX);
    ymin.push_back(envelope.MinY);
    xmax.push_back(envelope.MaxX);
    ymax.push_back(envelope.MaxY);
  }

  OGRGeometryFactory::destroyGeometry(geometry);

  if (xmin.empty())
  {
    error = "the geometry has no polygon";
    return false;
  }

  return true;
}
