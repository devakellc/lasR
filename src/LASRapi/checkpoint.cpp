#define R_NO_REMAP 1
#include "api.h"
#include "checkpoint_files.h"
#include "Engine.h"
#include "FileCollection.h"
#include "Progress.h"
#include "error.h"
#include "print.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>

namespace
{
namespace fs = std::filesystem;
using json = nlohmann::json;
using checkpoint_files::Lock;
using checkpoint_files::publish;

[[noreturn]] void fail(const std::string& message)
{
  throw std::runtime_error("Checkpoint: " + message);
}

std::string local_path(const std::string& path)
{
  if (path.empty() || path.find("://") != std::string::npos || path.rfind("/vsi", 0) == 0)
    fail("only local files are supported: " + path);
  return fs::weakly_canonical(fs::absolute(path)).string();
}

bool inside(const fs::path& path, const fs::path& directory)
{
  auto p = path.begin();
  for (auto d = directory.begin(); d != directory.end(); ++d, ++p)
    if (p == path.end() || *p != *d) return false;
  return true;
}

std::string extension(const fs::path& path)
{
  std::string ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
  return ext;
}

// Full-content, non-cryptographic checksums detect accidental modifications,
// including same-size rewrites whose modification times have been preserved.
json fingerprint(const std::string& path)
{
  if (!fs::is_regular_file(path)) fail("not a regular file: " + path);
  const auto size = fs::file_size(path);
  const auto mtime = fs::last_write_time(path);
  std::ifstream stream(path, std::ios::binary);
  if (!stream) fail("cannot read " + path);
  uint64_t hash = UINT64_C(14695981039346656037);
  char bytes[65536];
  while (stream.read(bytes, sizeof(bytes)) || stream.gcount())
  {
    for (std::streamsize i = 0; i < stream.gcount(); ++i)
    {
      hash ^= static_cast<unsigned char>(bytes[i]);
      hash *= UINT64_C(1099511628211);
    }
  }
  if (!stream.eof()) fail("failed while reading " + path);
  if (size != fs::file_size(path) || mtime != fs::last_write_time(path))
    fail("file changed while being checked: " + path);
  std::ostringstream hex;
  hex << std::hex << std::setfill('0') << std::setw(16) << hash;
  return {{"size", size}, {"fnv1a64", hex.str()}};
}

json engine_build()
{
  // Hash the actual loaded library, including incremental changes to any stage.
  return {{"binary", fingerprint(local_path(checkpoint_files::engine_path().string()))},
          {"gdal", GDALVersionInfo("RELEASE_NAME")}};
}

void save(const fs::path& path, const json& value)
{
  fs::path temporary = path.string() + ".tmp";
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    stream << value.dump(2) << '\n';
    stream.close();
    if (!stream) fail("cannot write " + temporary.string());
  }
  publish(temporary, path);
}

json read_json(const fs::path& path)
{
  try
  {
    std::ifstream stream(path);
    json value;
    stream >> value;
    return value;
  }
  catch (const std::exception&)
  {
    fail("invalid or unreadable metadata: " + path.string());
  }
}

// Validate before Engine::parse(): some stages open output files during parsing.
// An allowlist prevents future stages with untracked side effects from silently
// acquiring checkpoint support.
void normalize_pipeline(json& stages)
{
  const std::set<std::string> supported = {
    "build_catalog", "reader", "add_attribute", "add_rgb", "remove_rgb",
    "remove_attribute", "remove_attributes", "keep_attributes", "edit_attribute",
    "classify_with_csf", "classify_with_ptd", "classify_with_ipf",
    "classify_with_ivf", "classify_with_sor", "filter", "filter_grid", "sort",
    "triangulate", "transform_with", "set_crs", "sampling_pixel",
    "rasterize", "focal", "pit_fill", "spikefree",
    "hulls", "write_las", "write_pcd"
  };
  if (!stages.is_array() || stages.empty() || stages[0].value("algoname", "") != "build_catalog")
    fail("a file-backed catalog is required");
  std::map<std::string, std::string> ids;
  for (size_t i = 0; i < stages.size(); ++i)
  {
    std::string uid = stages[i].at("uid");
    if (!ids.emplace(uid, "stage_" + std::to_string(i)).second)
      fail("repeated stage objects are not supported; construct each stage separately");
  }
  size_t outputs = 0;
  for (auto& stage : stages)
  {
    std::string name = stage.at("algoname");
    if (!supported.count(name)) fail("stage '" + name + "' is not supported");
    if (name == "build_catalog" && stage.value("type", "files") != "files")
      fail("in-memory inputs are not supported");
    if (name == "sampling_pixel" && stage.value("method", "random") == "random")
      fail("random sampling is not supported");
    for (const auto& filter : get_vector<std::string>(stage.value("filter", json())))
      if (filter.find("random") != std::string::npos)
        fail("random filters are not supported");
    stage["uid"] = ids.at(stage.at("uid").get<std::string>());
    for (const char* key : {"connect", "connect1", "connect2"})
      if (stage.contains(key)) stage[key] = ids.at(stage[key].get<std::string>());
    std::string output = stage.value("output", "");
    if (output.empty()) continue;
    output = local_path(output);
    if (std::count(output.begin(), output.end(), '*') != 1 ||
        fs::path(output).parent_path().string().find('*') != std::string::npos)
      fail("every output needs one '*' in its filename; shared outputs cannot be resumed");
    if (name == "write_las" && output.size() >= 9 && output.substr(output.size()-9) == ".copc.las")
      output.erase(output.size()-9, 5);
    std::string ext = extension(output);
    if (ext != ".las" && ext != ".laz" && ext != ".pcd" && ext != ".tif" && ext != ".tiff" && ext != ".gpkg")
      fail("outputs must be LAS, LAZ, COPC, PCD, GeoTIFF or GeoPackage files");
    stage["output"] = output;
    ++outputs;
  }
  if (!outputs) fail("at least one per-chunk file output is required");

  std::vector<std::string> files;
  for (const auto& input : get_vector<std::string>(stages[0].at("files")))
  {
    std::string path = local_path(input);
    if (fs::is_directory(path))
    {
      for (const auto& stage : stages)
      {
        std::string output = stage.value("output", "");
        if (!output.empty() && inside(output, path))
          fail("outputs must be outside input directories");
      }
      std::vector<std::string> entries;
      for (const auto& entry : fs::directory_iterator(path))
      {
        std::string ext = extension(entry.path());
        if (entry.is_regular_file() && (ext == ".las" || ext == ".laz" || ext == ".pcd"))
          entries.push_back(local_path(entry.path().string()));
      }
      std::sort(entries.begin(), entries.end());
      files.insert(files.end(), entries.begin(), entries.end());
    }
    else files.push_back(path);
  }
  for (const auto& file : files)
  {
    std::string ext = extension(file);
    if (ext != ".las" && ext != ".laz" && ext != ".pcd" && ext != ".vpc")
      fail("inputs must be local LAS, LAZ, COPC, PCD or VPC files");
  }
  stages[0]["files"] = files;
}

struct Output
{
  std::string uid;
  std::string pattern;
  std::string final;
  fs::path directory;
  std::string temporary;
};
}

namespace api
{
ReturnType execute_checkpoint(nlohmann::json config)
{
  auto& options = config["processing"];
  auto& stages = config["pipeline"];
  const std::string directory = local_path(options.at("checkpoint"));
  const bool resume = options.value("resume", false);
  normalize_pipeline(stages);

  Engine pipeline;
  if (!pipeline.parse(stages, options.value("progress", false))) fail(last_error);
  FileCollection* catalog = pipeline.get_catalog();
  if (!catalog || (catalog->get_format() != LASFILE && catalog->get_format() != PCDFILE))
    fail("only local point-cloud catalogs are supported");

  const int n = catalog->get_number_chunks();
  std::vector<Chunk> chunks(n);
  std::set<std::string> input_paths;
  for (const auto& file : catalog->get_files()) input_paths.insert(local_path(file.string()));
  for (const auto& file : get_vector<std::string>(stages[0]["files"])) input_paths.insert(file);
  json inputs = json::object();
  std::map<std::string, std::pair<uintmax_t, fs::file_time_type>> input_stats;
  for (const auto& file : input_paths)
  {
    if (inside(file, directory)) fail("inputs must be outside the checkpoint directory");
    input_stats[file] = {fs::file_size(file), fs::last_write_time(file)};
    inputs[file] = fingerprint(file);
  }
  auto check_input = [&](const std::string& file)
  {
    const auto& stamp = input_stats.at(file);
    if (!fs::is_regular_file(file) || fs::file_size(file) != stamp.first ||
        fs::last_write_time(file) != stamp.second)
      fail("input changed during processing: " + file);
  };

  json plan = {{"pipeline", stages}, {"inputs", inputs}, {"chunks", json::array()},
               {"ncores", options.value("ncores", json::array({1, 0}))},
               {"strategy", options.value("strategy", "concurrent-points")}};
  std::vector<std::vector<Output>> outputs(n);
  std::set<std::string> output_paths;
  for (int i = 0; i < n; ++i)
  {
    Chunk& c = chunks[i];
    if (!catalog->get_chunk(i, c)) fail(last_error);
    plan["chunks"].push_back({{"name", c.name}, {"id", c.id},
      {"bbox", {c.xmin, c.ymin, c.xmax, c.ymax}}, {"buffer", c.buffer},
      {"shape", static_cast<int>(c.shape)}, {"process", c.process},
      {"main_files", c.main_files}, {"neighbour_files", c.neighbour_files}});
    if (c.is_empty() || !c.process) continue;
    if (c.name.empty() || fs::path(c.name).filename().string() != c.name)
      fail("invalid chunk name");
    for (const auto& stage : stages)
    {
      std::string pattern = stage.value("output", "");
      if (pattern.empty()) continue;
      std::string final = pattern;
      final.replace(final.find('*'), 1, c.name);
      final = local_path(final);
      if (input_paths.count(final) || inside(final, directory))
        fail("output collides with an input or checkpoint: " + final);
      if (!output_paths.insert(final).second)
        fail("output collision between stages or chunks: " + final);
      if (!fs::is_directory(fs::path(final).parent_path()))
        fail("output directory does not exist: " + final);
      if (!resume && fs::exists(final)) fail("output already exists: " + final);
      outputs[i].push_back({stage.at("uid"), pattern, final, {}, {}});
    }
  }

  // Logs must not overwrite inputs, outputs, or the checkpoint's own metadata.
  for (const char* key : {"log_file", "progress_file", "profile_file"})
  {
    std::string path = options.value(key, "");
    if (path.empty()) continue;
    path = local_path(path);
    if (input_paths.count(path) || output_paths.count(path) || inside(path, directory) ||
        (fs::exists(path) && fs::hard_link_count(path) > 1))
      fail("log path collides with job data: " + path);
    options[key] = path;
  }

  if (resume)
  {
    if (!fs::is_directory(directory)) fail("checkpoint directory does not exist");
  }
  else if (!fs::create_directory(directory))
    fail("directory already exists; use resume = TRUE or a new directory");
  Lock lock(fs::path(directory) / "lock");
  fs::path manifest_path = fs::path(directory) / "manifest.json";
  json manifest;
  // A checkpoint belongs to the installed engine build, not merely its package
  // version: two development builds can share a version but differ in behavior.
  const json build = engine_build();
  if (resume)
  {
    manifest = read_json(manifest_path);
    if (manifest.value("format", 0) != 1 || manifest.at("build") != build)
      fail("incompatible checkpoint format or engine build");
    if (manifest.at("plan") != plan)
      fail("inputs, pipeline, chunk plan or processing settings changed; use a new checkpoint");
    if (!fs::is_directory(fs::path(directory) / "completed"))
      fail("invalid checkpoint: missing completion directory");
  }
  else
  {
    std::random_device random;
    std::ostringstream token;
    token << std::hex << random() << random();
    manifest = {{"format", 1}, {"build", build}, {"token", token.str()}, {"plan", plan}};
    fs::create_directory(fs::path(directory) / "completed");
    save(manifest_path, manifest);
  }
  const std::string token = manifest.at("token");
  if (token.empty() || token.find_first_not_of("0123456789abcdef") != std::string::npos)
    fail("invalid checkpoint token");
  std::vector<json> receipts(n);
  std::vector<bool> completed(n, false);
  int done = 0;
  auto receipt_path = [&](int i) { return fs::path(directory) / "completed" / (std::to_string(i) + ".json"); };
  for (int i = 0; i < n; ++i)
  {
    for (auto& output : outputs[i])
    {
      output.directory = fs::path(output.final).parent_path() /
        (".lasr-" + token + "-" + std::to_string(i) + "-" + output.uid);
      output.temporary = (output.directory / fs::path(output.final).filename()).string();
    }
    if (!fs::exists(receipt_path(i))) continue;
    json receipt = read_json(receipt_path(i));
    if (receipt.at("chunk") != i || !receipt.at("outputs").is_object() ||
        receipt["outputs"].size() != outputs[i].size()) fail("invalid chunk receipt");
    bool valid = true;
    for (const auto& output : outputs[i])
    {
      const auto& record = receipt.at("outputs").at(output.uid);
      if (record.at("path") != output.final || !record.at("present").is_boolean())
        fail("invalid output receipt");
      if (record.at("present").get<bool>())
        valid = valid && fs::is_regular_file(output.final) && fingerprint(output.final) == record.at("fingerprint");
      else valid = valid && !fs::exists(output.final);
    }
    if (valid)
    {
      completed[i] = true;
      receipts[i] = receipt;
      ++done;
    }
    else fs::remove(receipt_path(i));
  }

  auto ncores = get_vector<int>(options.value("ncores", json::array({1, 0})));
  std::string strategy = options.value("strategy", "concurrent-points");
  if (ncores.empty() || ncores[0] < 1) fail("invalid thread count");
  int outer = 1, inner = 1;
  if (strategy == "concurrent-files") outer = ncores[0];
  else if (strategy == "concurrent-points") inner = ncores[0];
  else if (strategy == "nested")
  {
    if (ncores.size() < 2 || ncores[1] < 1) fail("invalid nested thread counts");
    outer = ncores[0]; inner = ncores[1];
  }
  else if (strategy != "sequential") fail("invalid processing strategy");
  outer = std::max(1, std::min({outer, n, available_threads()}));
  inner = std::min(inner, available_threads());
  if (!pipeline.is_parallelized()) inner = 1;
  if (outer > 1 && inner > 1) omp_set_max_active_levels(2);
  pipeline.set_ncpu(inner);
  pipeline.set_ncpu_concurrent_files(outer);
  pipeline.set_verbose(options.value("verbose", false));
  Progress progress;
  progress.set_prefix("Overall");
  progress.set_total(n);
  progress.set_display(options.value("progress", false));
  progress.set_ncpu(outer);
  progress.create_subprocess();
  pipeline.set_progress(&progress);
  std::ofstream log;
  if (!options.value("log_file", "").empty())
  {
    log.open(options.at("log_file").get<std::string>(), std::ios::app);
    if (!log) fail("cannot open log file");
  }
  auto update_progress = [&]() {
    progress.update(done, true);
    progress.show();
    std::string path = options.value("progress_file", "");
    if (!path.empty())
    {
      std::ofstream stream(path);
      stream << (n ? 100.0 * done / n : 100.0);
      if (!stream) fail("cannot write progress file");
    }
  };
  if (options.value("verbose", false)) print("Checkpoint: %d/%d chunks already complete\n", done, n);
  update_progress();
  if (Progress::interrupted()) fail("execution interrupted; unfinished chunks will be retried");
  if (done < n && !pipeline.pre_run()) fail(last_error);

  std::atomic<bool> failure(false);
  std::mutex state_mutex;
  std::string error;
  std::vector<std::vector<Profile>> profiles(n);
  #pragma omp parallel for num_threads(outer) schedule(dynamic)
  for (int i = 0; i < n; ++i)
  {
    if (completed[i] || failure || Progress::interrupted()) continue;
    try
    {
      std::map<std::string, std::vector<std::string>> written;
      if (!chunks[i].is_empty() && chunks[i].process)
      {
        std::map<std::string, std::string> redirects;
        for (const auto& output : outputs[i])
        {
          // Only this job's private staging directories are disposable.
          fs::remove_all(output.directory);
          fs::create_directory(output.directory);
          redirects[output.uid] = (output.directory / fs::path(output.pattern).filename()).string();
        }
        {
          // Destruction closes *all* shared GDAL handles before any publication.
          Engine worker(pipeline);
          worker.checkpoint_redirect(redirects);
          if (!worker.set_chunk(chunks[i]) || !worker.run()) fail(last_error);
          if (Progress::interrupted()) fail("execution interrupted; unfinished chunks will be retried");
          written = worker.checkpoint_outputs();
          profiles[i] = worker.profiler.profiles;
        }
      }
      for (const auto& file : chunks[i].main_files) check_input(local_path(file));
      for (const auto& file : chunks[i].neighbour_files) check_input(local_path(file));
      json receipt = {{"chunk", i}, {"outputs", json::object()}};
      for (const auto& output : outputs[i])
      {
        const auto& paths = written[output.uid];
        if (paths.size() > 1 || (!paths.empty() && paths[0] != output.temporary))
          fail("writer produced an unexpected output path");
        bool present = !paths.empty();
        json record = {{"path", output.final}, {"present", present}};
        if (present)
        {
          // Reject unexpected sidecars rather than publish an incomplete dataset.
          for (const auto& entry : fs::directory_iterator(output.directory))
            if (entry.path() != fs::path(output.temporary)) fail("writer produced unsupported sidecar files");
          record["fingerprint"] = fingerprint(output.temporary);
          publish(output.temporary, output.final, resume);
        }
        else if (resume) fs::remove(output.final);
        else if (fs::exists(output.final)) fail("output appeared during processing: " + output.final);
        receipt["outputs"][output.uid] = record;
        fs::remove_all(output.directory);
      }
      save(receipt_path(i), receipt);
      receipts[i] = receipt;
      std::lock_guard<std::mutex> guard(state_mutex);
      ++done;
      if (log.is_open()) { log << "Checkpoint chunk " << i << " complete\n"; log.flush(); }
      update_progress();
    }
    catch (const std::exception& e)
    {
      std::lock_guard<std::mutex> guard(state_mutex);
      if (!failure.exchange(true)) error = e.what();
    }
    catch (const std::string& e)
    {
      std::lock_guard<std::mutex> guard(state_mutex);
      if (!failure.exchange(true)) error = e;
    }
    catch (...)
    {
      std::lock_guard<std::mutex> guard(state_mutex);
      if (!failure.exchange(true)) error = "unknown worker failure";
    }
  }
  if (failure) fail(error);
  if (done != n || Progress::interrupted()) fail("execution interrupted; unfinished chunks will be retried");
  for (const auto& entry : input_stats) check_input(entry.first);

  std::map<std::string, std::vector<std::string>> all_outputs;
  for (int i = 0; i < n; ++i)
    for (const auto& output : outputs[i])
      if (receipts[i].at("outputs").at(output.uid).at("present").get<bool>())
        all_outputs[output.uid].push_back(output.final);
  pipeline.checkpoint_restore(all_outputs);
  for (const auto& profile : profiles)
    pipeline.profiler.profiles.insert(pipeline.profiler.profiles.end(), profile.begin(), profile.end());
  pipeline.profiler.write(options.value("profile_file", ""));
  progress.done(true);
#ifdef USING_R
  return pipeline.to_R();
#elif defined(USING_PYTHON)
  return std::make_pair(true, pipeline.to_json());
#else
  return true;
#endif
}
}
