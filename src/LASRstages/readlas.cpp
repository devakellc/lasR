#include "readlas.h"

#include "LASio.h"

LASRlasreader::LASRlasreader()
{
  header = nullptr;
  lasio = nullptr;
  streaming = true;
}

bool LASRlasreader::set_chunk(Chunk& chunk)
{
  Stage::set_chunk(chunk);

  // New chunk -> new reader for a new file. We can delete the previous reader and build a new one
  if (lasio)
  {
    lasio->close();
    delete lasio;
    lasio = nullptr;
  }

  lasio = new LASio();

  // A circular query is already narrower than its own box and is left alone
  double qxmin = chunk.xmin;
  double qymin = chunk.ymin;
  double qxmax = chunk.xmax;
  double qymax = chunk.ymax;

  if (chunk.aoi != nullptr && chunk.shape != ShapeType::CIRCLE)
  {
    qxmin = MAX(qxmin, chunk.aoi->xmin());
    qymin = MAX(qymin, chunk.aoi->ymin());
    qxmax = MIN(qxmax, chunk.aoi->xmax());
    qymax = MIN(qymax, chunk.aoi->ymax());
  }

  try
  {
    lasio->query(
        chunk.main_files,
        chunk.neighbour_files,
        qxmin,
        qymin,
        qxmax,
        qymax,
        chunk.buffer,
        chunk.shape == ShapeType::CIRCLE,
        filters);
  }
  catch (const std::exception& e)
  {
    last_error = e.what();
    return false;
  }

  return true;
}

bool LASRlasreader::process(Header*& header)
{
  // LASRlasreader is responsible for populating the header.
  // It is called first before LASRlasreader::process(Point) (streaming) or LASRlasreader::process(LAS) (in memory)
  // If the point is null then we create one Header. This object own the Header
  if (header != nullptr) return true;

  header = new Header;
  lasio->populate_header(header);

  this->header = header;

  return true;
}

// Streaming mode
bool LASRlasreader::process(Point*& point)
{
  if (point == nullptr)
    point = new Point(&header->schema);

  AOIposition position = AOI_INSIDE;

  do
  {
    if (lasio->read_point(point))
    {
      position = aoi_position(point->get_x(), point->get_y());
      if (position != AOI_INSIDE || point->inside_buffer(xmin, ymin, xmax, ymax, circular))
        point->set_buffered();
    }
    else
    {
      // In streaming mode this triggers a stop
      delete point;
      point = nullptr;
    }
  } while (point != nullptr && (pointfilter.filter(point) || position == AOI_OUTSIDE));

  return true;
}

// In memory mode
bool LASRlasreader::process(PointCloud*& las)
{
  if (las != nullptr) { delete las; las = nullptr; }
  if (las == nullptr) las = new PointCloud(header);

  streaming = false;

  progress->reset();
  progress->set_total(header->number_of_point_records);
  progress->set_prefix("read_las");

  Point p(&header->schema);

  while (lasio->read_point(&p))
  {
    if (progress->interrupted()) break;
    if (pointfilter.filter(&p)) continue;

    AOIposition position = aoi_position(p.get_x(), p.get_y());
    if (position == AOI_OUTSIDE) continue;

    if (position == AOI_BUFFER || p.inside_buffer(xmin, ymin, xmax, ymax, circular)) p.set_buffered();
    if (!las->add_point(p)) return false;

    progress->update(lasio->p_count());
    progress->show();
  }

  progress->done();
  if (verbose) print(" Number of point read %d\n", las->npoints);

  if (verbose) print("Building a spatial index\n");
  las->update_header();

  return true;
}

LASRlasreader::~LASRlasreader()
{
  if (lasio)
  {
    lasio->close();
    delete lasio;
    lasio = nullptr;
  }
}

void LASRlasreader::clear(bool)
{
  // Called at the end of the pipeline. We can delete the header
  if (streaming && header)
  {
    delete header;
    header = nullptr;
  }
}