#include "api.h"
#include "Engine.h"
#include "FileCollection.h"
#include "GDALdataset.h"
#include "Parallelism.h"
#include "PointFilter.h"
#include "openmp.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <set>

namespace api {
namespace {
using json = nlohmann::json;
namespace fs = std::filesystem;

struct Report
{
  json data = {{"schema_version", 1}, {"status", "incomplete"}, {"valid", nullptr},
               {"scope", "pipeline"}, {"diagnostics", json::array()},
               {"stages", json::array()}, {"inputs", json::array()},
               {"outputs", json::array()}, {"chunks", nullptr},
               {"capabilities", json::object()}, {"processing", json::object()}};
  bool errors = false;
  bool deferred = false;

  void add(const std::string& severity, const std::string& code,
           const std::string& message, int index = 0, const std::string& stage = "",
           const std::string& input = "", const std::string& attribute = "")
  {
    json d = {{"severity", severity}, {"code", code}, {"message", message}};
    if (index) d["stage_index"] = index;
    if (!stage.empty()) d["stage"] = stage;
    if (!input.empty()) d["input"] = input;
    if (!attribute.empty()) d["attribute"] = attribute;
    data["diagnostics"].push_back(d);
    errors |= severity == "error";
    deferred |= severity == "deferred";
  }

  json finish()
  {
    data["status"] = errors ? "invalid" : deferred ? "incomplete" : "valid";
    data["valid"] = errors ? json(false) : deferred ? json(nullptr) : json(true);
    return data;
  }
};

template<typename T> std::vector<T> values(const json& j)
{
  if (j.is_null()) return {};
  if (j.is_array()) return j.get<std::vector<T>>();
  return {j.get<T>()};
}

bool remote(const std::string& path)
{
  return path.find("://") != std::string::npos || path.rfind("/vsi", 0) == 0;
}

std::string path_key(const std::string& name)
{
  if (remote(name)) return name;
  std::error_code ec;
  fs::path p = fs::weakly_canonical(fs::absolute(fs::path(name)), ec);
  if (ec) p = fs::absolute(fs::path(name)).lexically_normal();
  std::string key = p.generic_string();
#ifdef _WIN32
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
#endif
  return key;
}

bool same_path(const std::string& a, const std::string& b)
{
  if (path_key(a) == path_key(b)) return true;
  if (remote(a) || remote(b)) return false;
  std::error_code ec;
  return fs::equivalent(fs::path(a), fs::path(b), ec) && !ec;
}

void finite_numbers(const json& j)
{
  if (j.is_number_float() && !std::isfinite(j.get<double>()))
    throw std::invalid_argument("Numeric parameters must be finite");
  if (j.is_structured()) for (const auto& v : j) finite_numbers(v);
}

void nonnegative(const json& obj, const std::string& key, bool positive = false)
{
  if (!obj.contains(key)) return;
  for (const auto& v : values<double>(obj.at(key)))
    if (!std::isfinite(v) || (positive ? v <= 0 : v < 0))
      throw std::invalid_argument(key + (positive ? " must be positive and finite" : " must be nonnegative and finite"));
  if (obj.at(key).is_null()) throw std::invalid_argument(key + " must be numeric");
}

bool check_structure(const json& stages, Report& report)
{
  if (!stages.is_array() || stages.empty())
    throw std::invalid_argument("pipeline must be a nonempty array of stages");
  std::map<std::string, int> seen;
  std::map<std::string, std::string> stage_names;
  bool catalog = false;
  int index = 0, readers = 0;
  for (const auto& s : stages)
  {
    std::string name = s.at("algoname").get<std::string>();
    if (name == "build_catalog")
    {
      if (catalog || index != 0) throw std::invalid_argument("build_catalog must occur once, before all stages");
      catalog = true;
      nonnegative(s, "buffer"); nonnegative(s, "chunk");
      continue;
    }
    ++index;
    const std::string uid = s.at("uid");
    if (uid.empty() || seen.count(uid))
      report.add("error", "duplicate_stage", "Stage identifiers must be nonempty and unique", index, name);
    if (name == "reader" || name == "reader_las") ++readers;
    for (const auto& key : {"connect", "connect1", "connect2"})
    {
      if (!s.contains(key)) continue;
      std::string connection = s.at(key);
      if (!seen.count(connection))
        report.add("error", "invalid_connection", "Connection must refer to a preceding stage: " + connection, index, name);
    }
    if ((name == "focal" || name == "pit_fill" || name == "transform_with" || name == "neighborhood_metrics") && !s.contains("connect"))
      report.add("error", "missing_connection", "Stage requires a connection to a preceding stage", index, name);
    if (name == "region_growing" && (!s.contains("connect1") || !s.contains("connect2")))
      report.add("error", "missing_connection", "region_growing requires raster and local-maximum connections", index, name);
    else if (name == "region_growing" && seen.count(s["connect1"]) && seen.count(s["connect2"]))
    {
      static const std::set<std::string> rasters = {"rasterize", "aggregate", "spikefree", "load_raster", "pit_fill", "focal", "region_growing"};
      const std::string a = stage_names.at(s["connect1"]), b = stage_names.at(s["connect2"]);
      if (!((rasters.count(a) && b == "local_maximum") || (rasters.count(b) && a == "local_maximum")))
        report.add("error", "invalid_connection", "region_growing requires one raster and one local_maximum stage", index, name);
    }
    seen[uid] = index;
    stage_names[uid] = name;
    try
    {
      finite_numbers(s);
      for (const auto& key : {"res", "res_x", "res_y", "res_z", "ws", "distance", "zwbin", "iwbin", "cloth_resolution", "time_step"}) nonnegative(s, key, true);
      for (const auto& key : {"window", "r", "k", "size", "radius"}) nonnegative(s, key);
      if (s.contains("k") && (!s["k"].is_number_integer() || s["k"].get<double>() > INT_MAX))
        throw std::invalid_argument("k must be a nonnegative integer within the supported range");
      if (name == "add_attribute")
      {
        static const std::set<std::string> types = {"uchar", "char", "ushort", "short", "uint", "int", "uint64", "int64", "float", "double"};
        if (!types.count(s.at("data_type").get<std::string>())) throw std::invalid_argument("Unsupported attribute data_type");
        if (s.at("name").get<std::string>().empty()) throw std::invalid_argument("Attribute name cannot be empty");
        if (s.contains("scale") && s.at("scale").get<double>() == 0)
          throw std::invalid_argument("Attribute scale must be nonzero");
      }
      if (name == "reader" || name == "reader_las")
      {
        auto query = [&](const std::vector<std::string>& keys)
        {
          bool present = false;
          for (const auto& key : keys) present |= s.contains(key);
          if (!present) return;
          size_t count = 0;
          for (size_t i = 0; i < keys.size(); ++i)
          {
            const auto v = values<double>(s.at(keys[i]));
            if (i == 0) count = v.size();
            if (v.size() != count || !count) throw std::invalid_argument("Reader query vectors must have the same nonzero length");
          }
          if (keys[0] == "xmin")
          {
            auto xmin = values<double>(s["xmin"]), ymin = values<double>(s["ymin"]);
            auto xmax = values<double>(s["xmax"]), ymax = values<double>(s["ymax"]);
            for (size_t i = 0; i < count; ++i)
              if (xmin[i] >= xmax[i] || ymin[i] >= ymax[i]) throw std::invalid_argument("Reader query bounds must have positive width and height");
          }
          else nonnegative(s, "radius", true);
        };
        query({"xmin", "ymin", "xmax", "ymax"});
        query({"xcenter", "ycenter", "radius"});
        if (s.contains("xmin") && s.contains("xcenter")) throw std::invalid_argument("Reader cannot combine circular and rectangular queries");
      }
      if (s.contains("filter"))
        for (const auto& f : values<std::string>(s["filter"]))
        {
          std::unique_ptr<Condition> condition(FilterParser().parse(f));
        }
      for (const auto& key : {"metrics", "method"})
        if (s.contains(key) && (name == "rasterize" || key == std::string("metrics")))
          for (const auto& metric : values<std::string>(s[key]))
            if (metric.empty() || metric.back() == '_') throw std::invalid_argument("Metric name cannot be empty or end in an underscore");
    }
    catch (const std::exception& e) { report.add("error", "invalid_parameters", e.what(), index, name); }
  }
  if (readers != 1) report.add("error", "invalid_reader", "Pipeline must contain exactly one reader");
  return !report.errors;
}

std::vector<std::string> names(const AttributeSchema& schema)
{
  std::vector<std::string> out;
  for (const auto& a : schema.attributes) out.push_back(a.name);
  return out;
}

json describe_schema(const AttributeSchema& schema)
{
  json out = json::array();
  for (const auto& a : schema.attributes)
    out.push_back({{"name", a.name}, {"type", a.attributeTypeToString()}, {"scale", a.scale_factor}, {"offset", a.value_offset}});
  return out;
}

json describe_crs(const CRS& crs)
{
  if (!crs.is_valid()) return nullptr;
  OGRSpatialReference srs = crs.get_crs();
  return {{"epsg", crs.get_epsg()}, {"wkt", crs.get_wkt()}, {"geographic", !!srs.IsGeographic()},
          {"linear_units", crs.get_linear_units()}};
}

bool equivalent_crs(const CRS& a, const CRS& b)
{
  OGRSpatialReference sa = a.get_crs(), sb = b.get_crs();
  sa.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  sb.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  return sa.IsSame(&sb);
}

struct Input
{
  std::string path;
  AttributeSchema schema;
  CRS crs;
  bool known;
  bool participates;
};

std::vector<std::string> requirements(const json& s, Report& report, int index)
{
  std::set<std::string> required;
  const std::string name = s.at("algoname");
  if (s.contains("filter")) for (const auto& f : values<std::string>(s["filter"]))
  {
    if (!f.empty() && f[0] == '-')
      report.add("deferred", "filter_attributes_unknown", "LASlib filter attribute requirements are not inspected", index, name);
    else
    {
      std::unique_ptr<Condition> condition(FilterParser().parse(f));
      if (condition) required.insert(condition->get_name());
    }
  }
  if (name == "edit_attribute") required.insert(s.at("attribute"));
  if (name.rfind("classify_with_", 0) == 0) required.insert("Classification");
  if (name == "local_maximum" || name == "triangulate" ||
      (name == "sampling_pixel" && s.value("method", "random") != "random"))
    required.insert(AttributeAccessor(s.value("use_attribute", "Z")).get_name());
  if ((name == "local_maximum" || name == "transform_with") && !s.value("store_in_attribute", "").empty())
    required.insert(s["store_in_attribute"]);
  const char* key = (name == "rasterize" && !s.contains("connect")) ? "method" : "metrics";
  if ((name == "rasterize" || name == "summarise" || name == "neighborhood_metrics") && s.contains(key))
    for (const auto& metric : values<std::string>(s[key]))
    {
      const size_t pos = metric.find('_');
      const std::string fun = pos == std::string::npos ? metric : metric.substr(pos + 1);
      if (fun != "count") required.insert(AttributeAccessor(map_attribute(pos == std::string::npos ? "Z" : metric.substr(0, pos))).get_name());
    }
  return {required.begin(), required.end()};
}

std::vector<Attribute> additions(const json& s)
{
  std::vector<Attribute> attrs;
  const std::string name = s.at("algoname");
  if (name == "add_attribute")
  {
    static const std::map<std::string, AttributeType> types = {{"uchar", UINT8}, {"char", INT8}, {"ushort", UINT16}, {"short", INT16}, {"uint", UINT32}, {"int", INT32}, {"uint64", UINT64}, {"int64", INT64}, {"float", FLOAT}, {"double", DOUBLE}};
    attrs.emplace_back(s.at("name"), types.at(s.at("data_type")), s.value("scale", 1.0), s.value("offset", 0.0), s.at("description"));
  }
  if (name == "add_rgb")
  {
    attrs.emplace_back("R", INT16, 0, 0, "Red channel");
    attrs.emplace_back("G", INT16, 0, 0, "Green channel");
    attrs.emplace_back("B", INT16, 0, 0, "Blue channe");
  }
  if (name == "svd")
  {
    std::string features = s.value("features", "");
    if (features.find('*') != std::string::npos) features = "CEapslocein";
    if (features.find('C') != std::string::npos) for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j)
      attrs.emplace_back("coeff" + std::to_string(i) + std::to_string(j), FLOAT, 1, 0, "Principal component coefficient");
    if (features.find('E') != std::string::npos) for (int i = 1; i <= 3; ++i)
      attrs.emplace_back("lambda" + std::to_string(i), FLOAT, 1, 0, "Eigen value " + std::to_string(i));
    const std::map<char, std::pair<std::string, std::string>> fields = {
      {'a', {"anisotropy", "anisotropy"}}, {'p', {"planarity", "planarity"}}, {'s', {"sphericity", "sphericity"}},
      {'l', {"linearity", "linearity"}}, {'o', {"omnivariance", "omnivariance"}}, {'c', {"curvature", "curvature"}},
      {'e', {"eigensum", "sum of eigen values"}}, {'i', {"inclination", "angle with vertical (degree)"}}};
    for (const auto& field : fields) if (features.find(field.first) != std::string::npos)
      attrs.emplace_back(field.second.first, FLOAT, 1, 0, field.second.second);
    if (features.find('n') != std::string::npos) for (const std::string axis : {"X", "Y", "Z"})
      attrs.emplace_back("normal" + axis, FLOAT, 1, 0, std::string(1, std::tolower(axis[0])) + " component of normal vector");
  }
  return attrs;
}

void inspect_schema(const json& s, json& stage, std::vector<Input>& inputs, Report& report)
{
  int index = stage["index"];
  const std::string name = s.at("algoname");
  const auto required = requirements(s, report, index);
  const auto added = additions(s);
  std::set<std::string> removed;
  stage["required_attributes"] = required;
  stage["added_attributes"] = json::array();
  for (const auto& a : added) stage["added_attributes"].push_back(a.name);
  const bool raster_maxima = name == "local_maximum" && s.contains("connect");
  if (raster_maxima)
    for (const auto& attribute : required)
      if (attribute != "X" && attribute != "Y" && attribute != "Z" && attribute != "Flags")
        report.add("error", "missing_attribute", "Raster local maxima use a derived cloud containing only X, Y, Z and Flags; attribute '" + attribute + "' is unavailable", index, name, "", attribute);
  for (auto& input : inputs)
  {
    if (!input.participates || !input.known) continue;
    if (!raster_maxima) for (const auto& attribute : required)
      if (!input.schema.has_attribute(attribute))
        report.add("error", "missing_attribute", "Required attribute '" + attribute + "' is missing at this stage; execution may otherwise use a zero fallback or fail", index, name, input.path, attribute);
    if (name == "transform_with" && !s.value("store_in_attribute", "").empty())
    {
      const auto* attr = input.schema.find_attribute(s["store_in_attribute"]);
      if (attr && attr->type != INT32 && attr->type != DOUBLE)
        report.add("error", "invalid_attribute_type", "Transformation target must have type int or double", index, name, input.path, attr->name);
    }
    for (const auto& a : added)
    {
      const auto* old = input.schema.find_attribute(a.name);
      const std::string alias = map_attribute(a.name);
      if ((old && *old != a) || (name == "add_attribute" && alias != a.name && input.schema.has_attribute(alias)))
        report.add("error", "attribute_collision", "Cannot add conflicting attribute '" + a.name + "'", index, name, input.path, a.name);
      else if (!old) input.schema.add_attribute(a);
    }
    std::vector<std::string> to_remove;
    const bool removes = name == "remove_attribute" || name == "remove_attributes" || name == "keep_attributes";
    if (removes && s.contains("name")) to_remove = {s.at("name")};
    if (removes && s.contains("names")) to_remove = s.at("names").get<std::vector<std::string>>();
    if (name == "remove_rgb") to_remove = {"R", "G", "B"};
    if (removes && s.contains("keep"))
    {
      const auto keep = s.at("keep").get<std::vector<std::string>>();
      for (const auto& a : input.schema.attributes)
        if (a.name != "X" && a.name != "Y" && a.name != "Z" && a.name != "x" && a.name != "y" && a.name != "z" && a.name != "flags" && std::find(keep.begin(), keep.end(), a.name) == keep.end())
          to_remove.push_back(a.name);
    }
    auto before = names(input.schema);
    for (const auto& a : to_remove) input.schema.remove_attribute(a);
    for (const auto& a : before) if (!input.schema.has_attribute(a)) removed.insert(a);
  }
  stage["removed_attributes"] = removed;
  if (name == "callback" || name == "aggregate")
  {
    report.add("deferred", "opaque_stage", "Injected code is not invoked; its requirements, schema effects and side effects cannot be validated", index, name);
    if (name == "callback" && !s.value("no_las_update", false)) for (auto& input : inputs) input.known = false;
  }
}

void inspect_crs(const json& stages, std::vector<Input>& inputs, FileCollection* catalog, Report& report)
{
  CRS first;
  bool catalog_writer = false;
  for (const auto& stage : stages) catalog_writer |= stage.at("algoname") == "write_vpc";
  for (const auto& input : inputs)
  {
    if (!input.participates && !catalog_writer) continue;
    if (!input.crs.is_valid())
      report.add("warning", "missing_crs", "Input CRS is missing; coordinate compatibility cannot be established", 0, "", input.path);
    else if (!first.is_valid()) first = input.crs;
    else if (!equivalent_crs(first, input.crs))
      report.add("error", "mixed_crs", "Participating input files have incompatible CRS; assigning a CRS does not reproject coordinates", 0, "", input.path);
  }
  CRS current = catalog ? catalog->get_crs() : first;
  int index = 0;
  bool warned_units = false;
  for (const auto& s : stages)
  {
    const std::string name = s.at("algoname");
    if (name == "build_catalog") continue;
    ++index;
    if (name == "set_crs")
    {
      const int epsg = s.value("epsg", 0);
      const std::string wkt = s.value("wkt", "");
      current = epsg > 0 ? CRS(epsg) : CRS(wkt);
      if (!current.is_valid()) report.add("error", "invalid_crs", "set_crs requires a recognized EPSG code or valid WKT", index, name);
      warned_units = false;
    }
    if (name == "write_vpc" && !inputs.empty() && !current.is_valid())
      report.add("error", "missing_output_crs", "Writing a VPC requires a valid CRS; assign one before write_vpc", index, name);
    if (name == "write_vpc" && catalog && catalog->get_format() == DATAFRAME)
      report.add("error", "unsupported_input", "Cannot write a VPC from a data.frame", index, name);
    if (name == "load_raster")
    {
      GDALdataset::initialize_gdal();
      std::unique_ptr<GDALDataset, void(*)(GDALDataset*)> raster(
        static_cast<GDALDataset*>(GDALOpenEx(s.at("file").get<std::string>().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr)),
        [](GDALDataset* p) { if (p) GDALClose(p); });
      if (raster)
      {
        CRS raster_crs(raster->GetProjectionRef());
        if (!raster_crs.is_valid()) report.add("warning", "missing_raster_crs", "Raster CRS is missing; alignment cannot be established", index, name);
        else if (current.is_valid() && !equivalent_crs(current, raster_crs))
          report.add("error", "raster_crs_mismatch", "Raster CRS differs from the point-cloud CRS; load_raster does not reproject", index, name);
      }
    }
    const bool spatial = s.contains("res") || s.contains("ws") || s.contains("distance") || s.contains("r") ||
                         name == "triangulate" || name.rfind("classify_with_", 0) == 0;
    if (spatial && current.is_valid() && !warned_units)
    {
      OGRSpatialReference srs = current.get_crs();
      if (srs.IsGeographic())
        report.add("warning", "geographic_crs", "Spatial distances, grid sizes and buffers operate in coordinate units; this CRS uses angular coordinates", index, name);
      else if (!current.is_meters())
        report.add("warning", "nonmetric_crs", "Spatial parameters use coordinate units; this CRS does not use metres", index, name);
      warned_units = true;
    }
  }
}

struct Destination { std::string path; int index; std::string stage; };

void inspect_outputs(const json& stages, const json& processing, FileCollection* catalog,
                     const std::vector<Chunk>& chunks, bool chunks_known,
                     const std::vector<Input>& inputs, Report& report)
{
  std::vector<std::string> sources;
  for (const auto& input : inputs) if (input.path != "data.frame" && input.path != "xptr") sources.push_back(input.path);
  // Input rasters and catalog manifests must also survive execution.
  for (const auto& s : stages)
  {
    const std::string name = s.at("algoname");
    if (name == "load_raster") sources.push_back(s.at("file"));
    if (name == "build_catalog" && s.contains("files"))
      for (const auto& f : values<std::string>(s["files"])) sources.push_back(f);
  }
  std::map<std::string, std::string> source_keys;
  for (const auto& source : sources) source_keys.emplace(path_key(source), source);
  std::map<std::string, Destination> destinations;
  std::vector<Destination> existing_destinations;
  auto record = [&](const std::string& path, int index, const std::string& stage, bool may_overwrite_input = false)
  {
    if (remote(path))
    {
      report.add("deferred", "remote_output", "Remote output identity and write access are not inspected", index, stage, path);
      return;
    }
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    const std::string key = path_key(path);
    if (!may_overwrite_input)
    {
      const auto input = source_keys.find(key);
      if (input != source_keys.end())
        report.add("error", "output_overwrites_input", "Output would overwrite an input: " + path, index, stage, input->second);
      else if (exists) for (const auto& source : sources)
        if (same_path(path, source))
        {
          report.add("error", "output_overwrites_input", "Output would overwrite an input: " + path, index, stage, source);
          break;
        }
    }
    const auto previous = destinations.find(key);
    if (previous != destinations.end())
      report.add("error", "output_collision", "Output collision at '" + path + "' with " + previous->second.stage + " (stage " + std::to_string(previous->second.index) + ")", index, stage);
    else if (exists) for (const auto& previous : existing_destinations)
      if (same_path(path, previous.path))
      {
        report.add("error", "output_collision", "Output collision through a filesystem alias: " + path, index, stage);
        break;
      }
    Destination destination{path, index, stage};
    destinations.emplace(key, destination);
    if (exists) existing_destinations.push_back(destination);
    if (exists)
    {
      if (fs::is_directory(path, ec)) report.add("error", "output_is_directory", "Output path is a directory: " + path, index, stage);
      else report.add("warning", "output_exists", "Existing output may be overwritten during execution: " + path, index, stage);
    }
    fs::path parent = fs::absolute(path).parent_path();
    if (!fs::is_directory(parent, ec)) report.add("error", "output_parent_missing", "Output parent directory does not exist: " + parent.string(), index, stage);
  };
  auto add = [&](int index, const std::string& stage, const std::string& templ,
                 const std::string& mode, const std::vector<std::string>& paths, bool embedded = false)
  {
    report.data["outputs"].push_back({{"stage_index", index}, {"stage", stage}, {"template", templ}, {"mode", mode}, {"paths", paths}});
    for (const auto& path : paths) record(path, index, stage, embedded);
  };
  auto unique_paths = [](std::vector<std::string>& paths)
  {
    std::set<std::string> keys;
    paths.erase(std::remove_if(paths.begin(), paths.end(), [&](const std::string& path) {
      return !keys.insert(path_key(path)).second;
    }), paths.end());
  };
  int index = 0;
  bool explicit_lax = false;
  for (const auto& s : stages)
  {
    std::string name = s.at("algoname");
    if (name == "build_catalog") continue;
    ++index;
    const auto& detail = report.data["stages"].at(index - 1);
    const std::string output_type = detail.value("output_type", "");
    if (name == "write_lax")
    {
      explicit_lax = true;
      std::vector<std::string> paths;
      if (catalog && catalog->get_format() == LASFILE) for (const auto& file : catalog->get_files())
      {
        if (s.value("embedded", false)) paths.push_back(file.string());
        else { fs::path p = file; p.replace_extension(".lax"); paths.push_back(p.string()); }
      }
      unique_paths(paths);
      add(index, name, "", s.value("embedded", false) ? "in_place_index" : "sidecar", paths, s.value("embedded", false));
      continue;
    }
    std::string output = s.value("output", "");
    if (output.empty())
    {
      if (name == "write_las" || name == "write_pcd" || name == "write_vpc")
        report.add("error", "missing_output", "Writer requires a nonempty output path", index, name);
      else if (!output_type.empty()) add(index, name, output, "in_memory", {});
      continue;
    }
    if (name == "write_las" && output.size() >= 9 && output.compare(output.size()-9, 9, ".copc.las") == 0)
      output.erase(output.size()-9, 5);
    if (name == "write_vpc" && fs::path(output).extension() != ".vpc")
      report.add("error", "invalid_output_format", "A virtual point cloud output must have the extension '.vpc'", index, name);
    if (std::count(output.begin(), output.end(), '*') > 1)
      report.add("error", "invalid_output_template", "Only one '*' placeholder is supported in an output path", index, name);
    if (!output_type.empty())
    {
      GDALdataset::initialize_gdal();
      const size_t dot = output.find_last_of('.');
      const std::string ext = dot == std::string::npos ? "" : output.substr(dot+1);
      const auto driver_name = GDALdataset::extension2driver.find(ext);
      GDALDriver* driver = driver_name == GDALdataset::extension2driver.end() ? nullptr : GetGDALDriverManager()->GetDriverByName(driver_name->second.c_str());
      const char* supports = driver ? driver->GetMetadataItem(output_type == "raster" ? GDAL_DCAP_RASTER : GDAL_DCAP_VECTOR) : nullptr;
      const char* creates = driver ? driver->GetMetadataItem(GDAL_DCAP_CREATE) : nullptr;
      if (!supports || std::string(supports) != "YES" || !creates || std::string(creates) != "YES")
        report.add("error", "invalid_output_format", "No supported " + output_type + " output driver for: " + output, index, name);
      else
      {
        static const std::set<std::string> modeled = {"GTiff", "GPKG", "GeoJSON", "FlatGeobuf", "SQLite", "netCDF", "KML", "GPX", "DXF", "XLSX", "ODS", "CSV", "ESRI Shapefile"};
        if (!modeled.count(driver_name->second))
          report.add("deferred", "output_sidecars_unknown", "Auxiliary outputs of this GDAL driver are not modeled; additional path collisions may exist", index, name);
      }
    }
    std::vector<std::string> paths;
    const bool wildcard = output.find('*') != std::string::npos && name != "write_vpc";
    if (!wildcard) paths.push_back(output);
    else if (chunks_known)
    {
      for (const auto& chunk : chunks)
      {
        if (!chunk.process || chunk.main_files.empty()) continue;
        std::string path = output;
        path.replace(path.find('*'), 1, chunk.name);
        paths.push_back(path);
      }
    }
    else report.add("deferred", "output_paths_unknown", "Per-chunk output paths need complete input bounds and chunk metadata", index, name);
    add(index, name, output, wildcard ? "per_chunk" : "merged", paths);
    if (output_type == "vector" && fs::path(output).extension() == ".shp")
    {
      std::vector<std::string> sidecars;
      for (const auto& path : paths) for (const auto& ext : {".shx", ".dbf", ".prj", ".cpg"})
      {
        fs::path sidecar = path;
        sidecar.replace_extension(ext);
        sidecars.push_back(sidecar.string());
      }
      add(index, name, output, "sidecar", sidecars);
    }
  }
  if (catalog && !explicit_lax && catalog->get_format() == LASFILE && !catalog->check_spatial_index())
  {
    std::vector<std::string> paths;
    const auto& headers = catalog->get_headers();
    const auto& files = catalog->get_files();
    for (size_t i = 0; i < files.size(); ++i) if (!headers[i].spatial_index)
    {
      if (files.size() > 1 && chunks_known && !inputs[i].participates) continue;
      fs::path p = files[i]; p.replace_extension(".lax"); paths.push_back(p.string());
    }
    unique_paths(paths);
    if (!paths.empty()) add(0, "automatic_spatial_index", "", "sidecar", paths);
  }
  for (const auto& key : {"log_file", "profile_file", "progress_file"})
    if (!processing.value(key, "").empty()) add(0, key, processing.at(key), "merged", {processing.at(key)});
}

json make_plan(const json& config, Report& report)
{
  const json processing = config.value("processing", json::object());
  json stages = config.at("pipeline");
  if (stages.is_array()) for (const auto& stage : stages)
    if (stage.is_object() && stage.value("algoname", "") == "build_catalog") report.data["scope"] = "inputs";
  finite_numbers(processing);
  nonnegative(processing, "buffer"); nonnegative(processing, "chunk");
  const std::string strategy = processing.value("strategy", "concurrent-points");
  std::vector<int> ncores;
  if (processing.contains("ncores"))
  {
    for (const auto& v : values<double>(processing["ncores"]))
    {
      // Pipeline stores an unused second zero for non-nested strategies.
      const bool inactive = strategy != "nested" && !ncores.empty() && v == 0;
      if ((!inactive && v < 1) || v > INT_MAX || std::floor(v) != v) throw std::invalid_argument("Active ncores values must be positive integers");
      ncores.push_back(static_cast<int>(v));
    }
  }
  if (ncores.empty()) ncores.push_back(static_cast<int>(std::ceil(omp_get_num_threads()/2.0)));
  if (strategy != "sequential" && strategy != "concurrent-points" && strategy != "concurrent-files" && strategy != "nested")
    throw std::invalid_argument("Unknown parallel strategy: " + strategy);
  if (strategy == "nested" && ncores.size() != 2) throw std::invalid_argument("Nested strategy requires two ncores values");
  report.data["processing"]["requested"] = {{"strategy", strategy}, {"ncores", ncores},
    {"buffer", processing.value("buffer", 0.0)}, {"chunk", processing.value("chunk", 0.0)},
    {"concurrent_files", (strategy == "concurrent-files" || strategy == "nested") ? ncores[0] : 1},
    {"concurrent_points", strategy == "nested" ? ncores[1] : strategy == "concurrent-points" ? ncores[0] : 1}};
  // Checkpoint/resume is an optional companion feature. Inspect the whole
  // declared pipeline without reading or changing saved job state; never
  // certify its additional admission and resume checks as part of this plan.
  if (!processing.value("checkpoint", "").empty() || processing.value("resume", false))
    report.add("deferred", "checkpoint_validation_deferred", "Checkpoint admission and saved resume state are checked by execution, not by this plan; listed chunks describe the full pipeline");
  if (!check_structure(stages, report)) return report.finish();

  // Remote readers may probe point payloads (notably EPT), so do not open them
  // while promising a metadata-only plan. Local manifests retain unknown schema.
  bool had_catalog = false;
  bool deferred_catalog = false;
  for (auto& s : stages)
  {
    if (s.at("algoname") != "build_catalog") continue;
    had_catalog = true;
    report.data["scope"] = "inputs";
    if (s.value("type", "files") == "files")
      for (const auto& file : values<std::string>(s.at("files")))
        if (remote(file) || fs::path(file).filename() == "ept.json") deferred_catalog = true;
  }
  for (const auto& s : stages)
    if (s.at("algoname") == "load_raster" && remote(s.at("file")))
    {
      report.add("deferred", "remote_raster", "Remote raster metadata is not opened by planning; dependent stage checks remain incomplete");
      return report.finish();
    }
  json parse_stages = stages;
  if (deferred_catalog)
  {
    for (const auto& file : values<std::string>(stages[0].at("files")))
      report.data["inputs"].push_back({{"path", file}, {"attributes", nullptr}, {"bounds", nullptr}, {"crs", nullptr}, {"points", nullptr}});
    parse_stages.erase(parse_stages.begin());
    report.add("deferred", "remote_metadata", "Remote and EPT input metadata are not opened by planning; schemas, CRS, chunks and expanded outputs remain unchecked");
  }
  else if (!had_catalog) report.add("deferred", "missing_inputs", "No inputs supplied; input attributes, CRS, chunks and output expansion remain unchecked");

  Engine engine;
  if (!engine.parse(parse_stages, false, true))
  {
    const bool limited = last_error.find("Inspection chunk limit") != std::string::npos;
    const bool unknown = last_error.find("Inspection metadata unavailable") != std::string::npos;
    report.add(limited || unknown ? "deferred" : "error", limited ? "plan_limit" : unknown ? "remote_metadata" : "invalid_configuration", last_error);
    return report.finish();
  }
  FileCollection* catalog = engine.get_catalog();
  const bool bounds_known = catalog && catalog->has_complete_bounds();
  if (catalog && (catalog->get_format() == DATAFRAME || catalog->get_format() == XPTR) && catalog->get_number_chunks() > 1)
    report.add("error", "unsupported_chunking", "Multiple processing chunks for an in-memory input are not supported");
  std::vector<Chunk> chunks;
  std::set<std::string> participating;
  if (bounds_known)
  {
    report.data["chunks"] = json::array();
    for (int i = 0; i < catalog->get_number_chunks(); ++i)
    {
      Chunk chunk;
      if (!catalog->get_chunk(i, chunk)) throw std::runtime_error(last_error);
      if (chunk.process && !chunk.is_empty())
      {
        for (const auto& f : chunk.main_files) participating.insert(path_key(f));
        for (const auto& f : chunk.neighbour_files) participating.insert(path_key(f));
      }
      report.data["chunks"].push_back({{"index", i+1}, {"id", chunk.id}, {"name", chunk.name},
        {"process", chunk.process}, {"empty", chunk.is_empty()}, {"bounds", {chunk.xmin, chunk.ymin, chunk.xmax, chunk.ymax}},
        {"buffer", chunk.buffer}, {"main_files", chunk.main_files}, {"neighbor_files", chunk.neighbour_files}});
      chunks.push_back(chunk);
    }
  }
  else if (catalog) report.add("deferred", "unknown_bounds", "Complete input bounds are unavailable without reading points; chunks and per-chunk output paths remain unchecked");

  std::vector<Input> inputs;
  if (catalog)
  {
    const auto& headers = catalog->get_headers();
    const auto& files = catalog->get_files();
    for (size_t i = 0; i < headers.size(); ++i)
    {
      const auto& h = headers[i];
      const std::string file = files[i].string();
      bool participates = !bounds_known || participating.count(path_key(file));
      CRS crs = h.signature == "data.frame" ? catalog->get_crs() : h.crs;
      inputs.push_back({file, h.schema, crs, !h.schema.attributes.empty(), participates});
      json bounds = nullptr;
      if (std::isfinite(h.min_x) && std::isfinite(h.max_x) && std::isfinite(h.min_y) && std::isfinite(h.max_y) && h.min_x <= h.max_x && h.min_y <= h.max_y)
        bounds = {h.min_x, h.min_y, h.max_x, h.max_y};
      report.data["inputs"].push_back({{"path", file}, {"format", h.signature}, {"points", h.number_of_point_records},
        {"attributes", h.schema.attributes.empty() ? json(nullptr) : describe_schema(h.schema)}, {"crs", describe_crs(crs)},
        {"bounds", bounds}, {"participates", participates}, {"spatial_index", h.spatial_index}});
      if (participates && h.schema.attributes.empty())
        report.add("deferred", "unknown_schema", "Input attribute schema is unavailable in inspected metadata", 0, "", file);
    }
    // Different schemas are allowed by the reader, but can lose fields/types
    // when a chunk merges multiple files under the first file's layout.
    const Input* reference = nullptr;
    for (const auto& input : inputs) if (input.participates && input.known)
    {
      if (!reference) reference = &input;
      else if (describe_schema(reference->schema) != describe_schema(input.schema))
        report.add("warning", "schema_variation", "Participating input schemas differ; merged readers may use the first input's attribute layout", 0, "", input.path);
    }
  }
  json capabilities = engine.inspection_stages();
  std::map<std::string, json> by_uid;
  for (const auto& stage : capabilities) by_uid[stage.at("uid")] = stage;
  std::map<std::string, int> indices;
  int index = 0;
  bool seen_reader = false;
  for (const auto& s : stages)
  {
    const std::string name = s.at("algoname");
    if (name == "build_catalog") continue;
    const std::string uid = s.at("uid");
    json detail = by_uid.at(uid);
    detail.erase("uid");
    detail["index"] = ++index;
    detail["name"] = name;
    if (name == "reader" || name == "reader_las") seen_reader = true;
    if (!seen_reader && detail.value("read_points", false))
      report.add("error", "invalid_reader_order", "A stage that reads points must follow the reader", index, name);
    detail["dependencies"] = json::array();
    for (const auto& key : {"connect", "connect1", "connect2"}) if (s.contains(key)) detail["dependencies"].push_back(indices.at(s.at(key)));
    indices[uid] = index;
    inspect_schema(s, detail, inputs, report);
    report.data["stages"].push_back(detail);
    if (name == "write_pcd" && std::max(processing.value("buffer", 0.0), engine.need_buffer()) > 0)
      report.add("error", "pcd_buffer", "PCD output does not support a buffered pipeline; effective buffer is " + std::to_string(std::max(processing.value("buffer", 0.0), engine.need_buffer())), index, name);
    if ((name == "svd" || name == "neighborhood_metrics" || name == "classify_with_sor") && s.value("r", 0.0) == 0)
      report.add("warning", "data_dependent_neighborhood", "A finite buffer cannot guarantee complete k-nearest neighborhoods without inspecting point density", index, name);
  }
  report.data["capabilities"] = {{"streamable", engine.is_streamable()}, {"read_points", engine.need_points()},
    {"parallelizable", engine.is_parallelizable()}, {"parallelized", engine.is_parallelized()}, {"use_rcapi", engine.use_rcapi()}, {"required_buffer", engine.need_buffer()}};
  int files = 1, points = 1;
  const int requested_cores = ncores[0];
  ncores[0] = std::min(ncores[0], available_threads());
  if (strategy == "concurrent-points") points = ncores[0];
  if (strategy == "concurrent-files") files = ncores[0];
  if (strategy == "nested") { files = ncores[0]; points = ncores[1]; }
  if (bounds_known) adjust_parallelism(files, points, chunks.size(), engine.is_parallelized(), engine.is_parallelizable(), engine.use_rcapi());
  double requested_buffer = processing.value("buffer", 0.0);
  const double effective_buffer = std::max(requested_buffer, engine.need_buffer());
  report.data["processing"]["effective"] = {{"strategy", strategy}, {"ncores", ncores}, {"buffer", effective_buffer},
    {"chunk", processing.value("chunk", 0.0)}, {"concurrent_files", bounds_known ? json(files) : json(nullptr)},
    {"concurrent_points", bounds_known ? json(points) : json(nullptr)}};
  if (requested_cores > ncores[0]) report.add("warning", "cores_limited", "Requested cores exceed available threads and are clamped during execution");
  if (effective_buffer > requested_buffer)
    report.add("warning", "buffer_adjusted", "Requested buffer is raised to the stage minimum of " + std::to_string(effective_buffer));
  inspect_crs(stages, inputs, catalog, report);
  inspect_outputs(stages, processing, catalog, chunks, bounds_known, inputs, report);
  report.data["limitations"] = {"Plans inspect metadata and stage declarations, not point values, filtered counts or algorithm success.",
    "Output paths describe possible writes; empty chunks, filters and stop conditions can suppress outputs.",
    "Filesystem permissions, available disk space and external changes after planning are not guaranteed."};
  return report.finish();
}
} // namespace

nlohmann::json pipeline_plan(const nlohmann::json& config)
{
  Report report;
  try { return make_plan(config, report); }
  catch (const std::exception& e) { report.add("error", "invalid_configuration", e.what()); }
  catch (const std::string& e) { report.add("error", "invalid_configuration", e); }
  catch (const char* e) { report.add("error", "invalid_configuration", e); }
  return report.finish();
}
} // namespace api
