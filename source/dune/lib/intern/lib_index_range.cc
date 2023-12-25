#include <mutex>

#include "lib_array.hh"
#include "lib_index_range.hh"
#include "lib_span.hh"
#include "lib_task.hh"
#include "lib_vector.hh"

namespace dune {

AlignedIndexRanges split_index_range_by_alignment(const IndexRange range, const int64_t alignment)
{
  lib_assert(is_power_of_2_i(alignment));
  const int64_t mask = alignment - 1;

  AlignedIndexRanges aligned_ranges;

  const int64_t start_chunk = range.start() & ~mask;
  const int64_t end_chunk = range.one_after_last() & ~mask;
  if (start_chunk == end_chunk) {
    aligned_ranges.prefix = range;
  }
  else {
    int64_t prefix_size = 0;
    int64_t suffix_size = 0;
    if (range.start() != start_chunk) {
      prefix_size = alignment - (range.start() & mask);
    }
    if (range.one_after_last() != end_chunk) {
      suffix_size = range.one_after_last() - end_chunk;
    }
    aligned_ranges.prefix = IndexRange(range.start(), prefix_size);
    aligned_ranges.suffix = IndexRange(end_chunk, suffix_size);
    aligned_ranges.aligned = IndexRange(aligned_ranges.prefix.one_after_last(),
                                        range.size() - prefix_size - suffix_size);
  }

  return aligned_ranges;
}

}  // namespace blender
