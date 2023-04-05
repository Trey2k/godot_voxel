#ifndef VOXEL_DETAIL_RENDERING_H
#define VOXEL_DETAIL_RENDERING_H

#include "../util/fixed_array.h"
#include "../util/godot/classes/ref_counted.h"
#include "../util/macros.h"
#include "../util/math/vector3f.h"
#include "../util/span.h"

//#define VOXEL_VIRTUAL_TEXTURE_USE_TEXTURE_ARRAY
// Texture arrays are handy but the maximum amount of layers is often too low (2048 on an nVidia 1060), which happens
// too frequently with block size 32. So instead we have to keep using 2D atlases with padding.
#ifdef VOXEL_VIRTUAL_TEXTURE_USE_TEXTURE_ARRAY
#include "../util/godot/classes/texture_array.h"
#endif
#include "../util/godot/classes/image.h"
#include "../util/godot/classes/texture_2d.h"

#include <vector>

namespace zylann::voxel {

class VoxelGenerator;
class VoxelData;

// TODO This system could be extended to more than just normals
// - Texturing data
// - Color
// - Some kind of depth (could be useful to fake water from far away)

// UV-mapping a voxel mesh is not trivial, but if mapping is required, an alternative is to subdivide the mesh into a
// grid of cells (we can use Transvoxel cells). In each cell, pick an axis-aligned projection working best with
// triangles of the cell using the average of their normals. A tile can then be generated by projecting its pixels on
// triangles, and be stored in an atlas. A shader can then read the atlas using a lookup texture to find the tile.

struct DetailRenderingSettings {
	// If enabled, an atlas of normalmaps will be generated for each cell of the voxel mesh, in order to add
	// more visual details using a shader.
	bool enabled = false;
	// LOD index from which normalmaps will start being generated.
	uint8_t begin_lod_index = 2;
	// Tile resolution that will be used starting from the beginning LOD. Resolution will double at each following
	// LOD index.
	uint8_t tile_resolution_min = 4;
	uint8_t tile_resolution_max = 8;
	// If the angle between geometry normals and computed normals exceeds this angle, their direction will be clamped.
	uint8_t max_deviation_degrees = 60;
	// If enabled, encodes normalmaps using octahedral compression, which trades a bit of quality for
	// significantly reduced memory usage (using 2 bytes per pixel instead of 3).
	bool octahedral_encoding_enabled = false;

	static constexpr uint8_t MIN_DEVIATION_DEGREES = 1;
	static constexpr uint8_t MAX_DEVIATION_DEGREES = 179;
};

unsigned int get_detail_texture_tile_resolution_for_lod(
		const DetailRenderingSettings &settings, unsigned int lod_index);

struct DetailTextureData {
	// Encoded normals
	std::vector<uint8_t> normals;
	struct Tile {
		uint8_t x;
		uint8_t y;
		uint8_t z;
		uint8_t axis;
	};
	std::vector<Tile> tiles;
	// Optionally used in case of partial tiles data, when only getting edited tiles.
	// If this is empty, it means indices are sequential so there is no need to store them here.
	std::vector<uint32_t> tile_indices;

	inline void clear() {
		normals.clear();
		tiles.clear();
	}
};

// To hold the current cell only. Not optimized for space. May use a more efficient structure per implementation of
// `ICellIterator`.
struct CurrentCellInfo {
	static const unsigned int MAX_TRIANGLES = 5;
	FixedArray<uint32_t, MAX_TRIANGLES> triangle_begin_indices;
	uint32_t triangle_count = 0;
	Vector3i position;
};

class ICellIterator {
public:
	virtual ~ICellIterator() {}
	virtual unsigned int get_count() const = 0;
	virtual bool next(CurrentCellInfo &info) = 0;
	virtual void rewind() = 0;
};

// For each non-empty cell of the mesh, choose an axis-aligned projection based on triangle normals in the cell.
// Sample voxels inside the cell to compute a tile of world space normals from the SDF.
// If the angle between the triangle and the computed normal is larger tham `max_deviation_radians`,
// the normal's direction will be clamped.
// If `out_edited_tiles` is provided, only tiles containing edited voxels will be processed.
void compute_detail_texture_data(ICellIterator &cell_iterator, Span<const Vector3f> mesh_vertices,
		Span<const Vector3f> mesh_normals, Span<const int> mesh_indices, DetailTextureData &texture_data,
		unsigned int tile_resolution, VoxelGenerator &generator, const VoxelData *voxel_data, Vector3i origin_in_voxels,
		Vector3i size_in_voxels, unsigned int lod_index, bool octahedral_encoding, float max_deviation_radians,
		bool edited_tiles_only);

struct DetailImages {
#ifdef VOXEL_VIRTUAL_TEXTURE_USE_TEXTURE_ARRAY
	Vector<Ref<Image>> atlas;
#else
	Ref<Image> atlas;
#endif
	Ref<Image> lookup;
};

struct DetailTextures {
#ifdef VOXEL_VIRTUAL_TEXTURE_USE_TEXTURE_ARRAY
	Ref<Texture2DArray> atlas;
#else
	Ref<Texture2D> atlas;
#endif
	Ref<Texture2D> lookup;
};

Ref<Image> store_lookup_to_image(const std::vector<DetailTextureData::Tile> &tiles, Vector3i block_size);

DetailImages store_normalmap_data_to_images(
		const DetailTextureData &data, unsigned int tile_resolution, Vector3i block_size, bool octahedral_encoding);

// Converts normalmap data into textures. They can be used in a shader to apply normals and obtain extra visual details.
// This may not be allowed to run in a different thread than the main thread if the renderer is not using Vulkan.
DetailTextures store_normalmap_data_to_textures(const DetailImages &data);

struct DetailTextureOutput {
	// Normalmap atlas used for smooth voxels.
	// If textures can't be created from threads, images are returned instead.
	DetailImages images;
	DetailTextures textures;
	// Can be false if textures are computed asynchronously. Will become true when it's done (and not change after).
	std::atomic_bool valid = { false };
};

// Given a number of items, tells which size a 2D square grid should be in order to contain them
inline unsigned int get_square_grid_size_from_item_count(unsigned int item_count) {
	return int(Math::ceil(Math::sqrt(double(item_count))));
}

// Copies data from a fully packed array into a sub-region of a 2D array (where each rows may be spaced apart).
inline void copy_2d_region_from_packed_to_atlased(Span<uint8_t> dst, Vector2i dst_size, Span<const uint8_t> src,
		Vector2i src_size, Vector2i dst_pos, unsigned int item_size_in_bytes) {
#ifdef DEBUG_ENABLED
	ZN_ASSERT(src_size.x >= 0 && src_size.y >= 0);
	ZN_ASSERT(dst_size.x >= 0 && dst_size.y >= 0);
	ZN_ASSERT(dst_pos.x >= 0 && dst_pos.y >= 0 && dst_pos.x + src_size.x <= dst_size.x &&
			dst_pos.y + src_size.y <= dst_size.y);
	ZN_ASSERT(src.size() == src_size.x * src_size.y * item_size_in_bytes);
	ZN_ASSERT(dst.size() == dst_size.x * dst_size.y * item_size_in_bytes);
	ZN_ASSERT(!src.overlaps(dst));
#endif
	const unsigned int dst_begin = (dst_pos.x + dst_pos.y * dst_size.x) * item_size_in_bytes;
	const unsigned int src_row_size = src_size.x * item_size_in_bytes;
	const unsigned int dst_row_size = dst_size.x * item_size_in_bytes;
	uint8_t *dst_p = dst.data() + dst_begin;
	const uint8_t *src_p = src.data();
	for (unsigned int src_y = 0; src_y < (unsigned int)src_size.y; ++src_y) {
		memcpy(dst_p, src_p, src_row_size);
		dst_p += dst_row_size;
		src_p += src_row_size;
	}
}

} // namespace zylann::voxel

#endif // VOXEL_DETAIL_RENDERING_H