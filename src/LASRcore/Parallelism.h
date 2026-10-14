#ifndef LASR_PARALLELISM_H
#define LASR_PARALLELISM_H

#include <algorithm>

// Shared by execution and its read-only plan. Preserve the executor's ordering:
// a single chunk swaps the loops before stage constraints are applied.
inline int adjust_parallelism(int& files, int& points, int chunks,
                               bool parallelized, bool parallelizable,
                               bool use_rcapi)
{
  if (chunks == 1 && files > 1) std::swap(files, points);
  if (files > chunks) files = chunks;
  if (!parallelized && points > 1) points = 1;
  int reason = 0;
  if ((!parallelizable || use_rcapi) && files > 1)
  {
    reason = !parallelizable ? 1 : 2;
    points = files;
    files = 1;
  }
  return reason;
}

#endif
