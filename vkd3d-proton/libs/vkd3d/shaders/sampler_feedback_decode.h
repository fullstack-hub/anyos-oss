#ifndef SAMPLER_FEEDBACK_DECODE_MIN_MIP_H_
#define SAMPLER_FEEDBACK_DECODE_MIN_MIP_H_

layout(push_constant, std430) uniform Registers
{
	ivec2 src_offset;
	ivec2 dst_offset;
	ivec2 resolution;
	ivec2 paired_resolution;
	vec2 inv_paired_resolution;
	vec2 inv_feedback_resolution;
	int mip_levels;
	int mip_level;
};

#ifdef ARRAY
layout(set = 0, binding = 1) uniform usampler2DArray Input;
#else
layout(set = 0, binding = 1) uniform usampler2D Input;
#endif

// AnyOS: the feedback map is two interleaved R32_UINT planes. Logical texel
// (x, y) lives at (2x, y) for mips 0-7 and (2x + 1, y) for mips 8-14 plus the
// encoded MinMip in the top 4 bits. Fetch exactly what the upstream
// textureGather() with a nearest CLAMP_TO_EDGE sampler at an integer corner c
// returned: texels c - 1 and c in each axis, clamped to the (padded) map.
#ifdef ARRAY
uint fetch_plane(ivec2 coord, int plane, float layer)
{
	return texelFetch(Input, ivec3(2 * coord.x + plane, coord.y, int(layer)), 0).x;
}
#else
uint fetch_plane(ivec2 coord, int plane, float layer)
{
	return texelFetch(Input, ivec2(2 * coord.x + plane, coord.y), 0).x;
}
#endif

bool fetch_mip(vec2 unnormalized_feedback_coord, int mip, float layer)
{
	// Get bottom-right (Y) as target region,
	// and the potential neighbors in top-left (W), left (X) and up (Z).
	// If we detect any writes in those neighbors, propagate them to the bottom-right.
	ivec2 map_size = textureSize(Input, 0).xy;
	map_size.x >>= 1;
	ivec2 c = ivec2(unnormalized_feedback_coord);
	ivec2 lo = clamp(c - 1, ivec2(0), map_size - 1);
	ivec2 hi = clamp(c, ivec2(0), map_size - 1);

	int plane = 0;
	if (mip >= 8)
	{
		plane = 1;
		mip -= 8;
	}

	uvec4 values;
	values.x = fetch_plane(ivec2(lo.x, hi.y), plane, layer);
	values.y = fetch_plane(ivec2(hi.x, hi.y), plane, layer);
	values.z = fetch_plane(ivec2(hi.x, lo.y), plane, layer);
	values.w = fetch_plane(ivec2(lo.x, lo.y), plane, layer);

	// Shift down to get the mip level we care about. Every mip gets 4 bits of data.
	uvec4 per_mip_bits = bitfieldExtract(values, 4 * mip, 4);

	// Check self-access and neighbor access.
	// Clamp behavior is okay here. If we intend sample region (0, 0)
	// there cannot be neighbor access unless we also have self-access,
	// so false positive is not possible here.

	const uint HORIZ = 2;
	const uint VERT = 4;
	const uint DIAG = 8;
	const uint SELF = 1;
	return any(notEqual(per_mip_bits & uvec4(HORIZ, SELF, VERT, DIAG), uvec4(0)));
}

int sampler_feedback_decode_min_mip(ivec2 icoord, float layer)
{
	// Use a small epsilon here so that POT scaling does not add false positives, or we get funny rounding errors.
	// Aim to sample at corners, but not *exactly* at corners to avoid straddling into regions we don't really care about.
	// This epsilon is computed in FP32, *not* in subtexels.
	const float EPSILON = 1.0 / (256.0 * 256.0);
	vec2 top_left_coord = vec2(icoord) + EPSILON;
	vec2 bottom_right_coord = top_left_coord + (1.0 - 2.0 * EPSILON);

	// Compute the relevant footprint.
	vec2 top_left_coord_normalized = top_left_coord * inv_paired_resolution;
	vec2 bottom_right_coord_normalized = bottom_right_coord * inv_paired_resolution;

	// Flooring gets us the behavior we want.
	// We'll sample exactly in-between 4 texel centers such that coordiate we floor becomes the bottom-right texel.

	if (fetch_mip(vec2(icoord), 0, layer))
		return 0;

	// Extract encoded min-mip. This serves as our starting min-bound.
	int result = 0xff;
	uint high_bits = fetch_plane(icoord, 1, layer) >> 28;
	if (high_bits > 0)
		result = int(high_bits - 1);

	int scan_mip_levels = min(mip_levels, result);

	for (int i = 1; i < scan_mip_levels; i++)
	{
		// We need to intersect the LOD 0 region with the used region in a coarse LOD.
		// For simple POT, this is a trivial 2x2 expansion.
		// However, for NPOT, this gets ... hairy very quickly.
		// It's possible that a lower LOD access can cause a 3x3 access in the higher LOD.
		// Sample the lower LOD at each (approximate) corner to get a conservative estimate of access.
		// This is intended to match NV behavior.

		// Rescale the coordinates into coordinates matching lower resolution.
		vec2 mip_resolution = vec2(max(paired_resolution >> i, ivec2(1)));
		vec2 c0 = floor(mip_resolution * top_left_coord_normalized);
		vec2 c1 = floor(mip_resolution * bottom_right_coord_normalized);

		if (fetch_mip(c0, i, layer))
		{
			result = i;
			break;
		}

		if (any(notEqual(c0, c1)))
		{
			// Should only trigger for NPOT.
			if (fetch_mip(vec2(c1.x, c0.y), i, layer))
			{
				result = i;
				break;
			}

			if (fetch_mip(vec2(c0.x, c1.y), i, layer))
			{
				result = i;
				break;
			}

			if (fetch_mip(vec2(c1.x, c1.y), i, layer))
			{
				result = i;
				break;
			}
		}
	}

	return result;
}

bool sampler_feedback_decode_mip_used(ivec2 icoord, int mip, float layer)
{
	// This is much simpler.
	return fetch_mip(vec2(icoord + src_offset), mip, layer);
}

#endif

