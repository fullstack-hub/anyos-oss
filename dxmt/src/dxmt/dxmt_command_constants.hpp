#pragma once

#ifdef __METAL__
#define CONSTANT constant
#else
#define CONSTANT
#endif

namespace dxmt {

constexpr CONSTANT auto kCustomBufferArgumentIndex0 = 30;
constexpr CONSTANT auto kCustomBufferArgumentIndex1 = 29;

constexpr CONSTANT auto kPresentFCIndex_BackbufferSizeMatched = 0x100;
constexpr CONSTANT auto kPresentFCIndex_BackbufferIsSRGB = 0x103;
constexpr CONSTANT auto kPresentFCIndex_HDRPQ = 0x101;
constexpr CONSTANT auto kPresentFCIndex_WithHDRMetadata = 0x102;
constexpr CONSTANT auto kPresentFCIndex_BackbufferIsMS = 0x104;
constexpr CONSTANT auto kPresentFCIndex_GammaEnabled = 0x105;

/* AnyOS: domain shader stream output (so_scan and so_write in
 * dxmt_command.metal).  The tessellation pipeline streams the domain shader's
 * vertices, indices and one record per workload (air_tessellation.metal);
 * so_scan turns the records into per-patch primitive offsets, clamps at the
 * bound buffers and advances their filled sizes and the active SO queries;
 * so_write stores every primitive's declared components at
 * base + (patch offset + primitive within the patch) * vertices. */
constexpr CONSTANT unsigned kStreamOutputMaxQueries = 4;

struct DXMTStreamOutputDraw {
  unsigned patch_count;
  unsigned vertices_per_primitive;
  unsigned slot_mask; /* bound buffers with a stride */
  unsigned query_count;
  unsigned buffer_size[4];
  unsigned stride[4];
  unsigned element_count;
  unsigned registers; /* streamed registers per vertex */
  unsigned record_capacity;
  unsigned padding;
};

struct DXMTStreamOutputState {
  unsigned written;
  unsigned needed;
  unsigned overflow;
  unsigned padding;
  unsigned base[4]; /* byte offset of this draw's first vertex in each buffer */
};

} // namespace dxmt
