#include "readept.h"

#include "EPTio.h"

LASReptreader::LASReptreader()
{
  header = nullptr;
  current_source = 0;
  streaming = true;
}

LASReptreader::LASReptreader(const LASReptreader& other) : Stage(other)
{
  // The sources are rebuilt by set_chunk on the clone
  header = nullptr;
  current_source = 0;
  streaming = other.streaming;
}

bool LASReptreader::set_chunk(Chunk& chunk)
{
  Stage::set_chunk(chunk);

  sources.clear();
  current_source = 0;

  try
  {
    for (const auto& file : chunk.main_files)
    {
      auto eptio = std::unique_ptr<EPTio>(new EPTio());
      eptio->query(file, chunk.xmin, chunk.ymin, chunk.xmax, chunk.ymax,
                   chunk.buffer, chunk.shape == ShapeType::CIRCLE, filters);
      sources.emplace_back(file, std::move(eptio));
    }
  }
  catch (const std::exception& e)
  {
    last_error = e.what();
    return false;
  }

  return true;
}

bool LASReptreader::process(Header*& header)
{
  if (header != nullptr) return true;
  if (sources.empty()) { last_error = "EPT reader requires at least one source in the chunk"; return false; }

  header = new Header;

  try
  {
    sources[0].second->populate_header(header);

    int64_t npoints = header->number_of_point_records;
    std::vector<Header> others(sources.size() - 1);
    for (size_t i = 1 ; i < sources.size() ; i++)
    {
      Header& other = others[i-1];
      sources[i].second->populate_header(&other);

      if (other.x_scale_factor != header->x_scale_factor ||
          other.y_scale_factor != header->y_scale_factor ||
          other.z_scale_factor != header->z_scale_factor ||
          other.x_offset != header->x_offset ||
          other.y_offset != header->y_offset ||
          other.z_offset != header->z_offset)
      {
        last_error = "EPT endpoints with different scale or offset cannot be merged";
        return false;
      }

      npoints += other.number_of_point_records;
    }
    header->number_of_point_records = npoints;

    if (!others.empty())
    {
      AttributeSchema merged;
      for (const auto& attribute : header->schema.attributes)
      {
        bool in_all = true;
        for (const auto& other : others)
          if (!other.schema.has_attribute(attribute.name)) { in_all = false; break; }

        if (in_all) merged.add_attribute(attribute);
      }
      header->schema = merged;
    }
  }
  catch (const std::exception& e)
  {
    last_error = e.what();
    return false;
  }

  this->header = header;

  return true;
}

// Streaming mode
bool LASReptreader::process(Point*& point)
{
  if (point == nullptr)
    point = new Point(&header->schema);

  do
  {
    bool got = false;
    while (current_source < sources.size())
    {
      if (sources[current_source].second->read_point(point)) { got = true; break; }
      current_source++;
    }

    if (got)
    {
      if (point->inside_buffer(xmin, ymin, xmax, ymax, circular))
        point->set_buffered();
    }
    else
    {
      delete point;
      point = nullptr;
    }
  } while (point != nullptr && pointfilter.filter(point));

  return true;
}

// In memory mode
bool LASReptreader::process(PointCloud*& las)
{
  if (las != nullptr) { delete las; las = nullptr; }
  if (las == nullptr) las = new PointCloud(header);

  streaming = false;

  progress->reset();
  progress->set_total(header->number_of_point_records);
  progress->set_prefix("read_ept");

  Point p(&header->schema);

  int64_t read = 0;
  for (auto& source : sources)
  {
    while (source.second->read_point(&p))
    {
      if (progress->interrupted()) break;
      if (pointfilter.filter(&p)) continue;
      if (p.inside_buffer(xmin, ymin, xmax, ymax, circular)) p.set_buffered();
      if (!las->add_point(p)) return false;

      progress->update(read + source.second->p_count());
      progress->show();
    }
    read += source.second->p_count();
    if (progress->interrupted()) break;
  }

  progress->done();
  if (verbose) print(" Number of point read %d\n", las->npoints);

  if (verbose) print("Building a spatial index\n");
  las->update_header();

  return true;
}

LASReptreader::~LASReptreader()
{
}

void LASReptreader::clear(bool)
{
  if (streaming && header)
  {
    delete header;
    header = nullptr;
  }
}
