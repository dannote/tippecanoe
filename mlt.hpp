#ifndef MLT_HPP
#define MLT_HPP

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include "mvt.hpp"

// MLT (MapLibre Tile) format encoding support
// Based on the MapLibre Tile Spec: https://github.com/maplibre/maplibre-tile-spec

namespace mlt {

// Physical stream types (4 bits in header)
enum PhysicalStreamType {
	STREAM_PRESENT = 0,
	STREAM_DATA = 1,
	STREAM_OFFSET = 2,
	STREAM_LENGTH = 3,
};

// Logical level techniques (3 bits each in header)
enum LogicalLevelTechnique {
	LLT_NONE = 0,
	LLT_DELTA = 1,
	LLT_COMPONENTWISE_DELTA = 2,
	LLT_RLE = 3,
	LLT_MORTON = 4,
	LLT_PDE = 5,  // Pseudodecimal encoding
};

// Physical level techniques (2 bits in header)
enum PhysicalLevelTechnique {
	PLT_NONE = 0,
	PLT_FAST_PFOR = 1,
	PLT_VARINT = 2,
	PLT_ALP = 3,
};

// Dictionary types for DATA streams (4 bits in lower nibble)
enum DictionaryType {
	DICT_NONE = 0,
	DICT_SINGLE = 1,
	DICT_SHARED = 2,
	DICT_VERTEX = 3,
	DICT_MORTON = 4,
	DICT_FSST = 5,
};

// Offset types for OFFSET streams (4 bits in lower nibble)
enum OffsetType {
	OFFSET_VERTEX = 0,
	OFFSET_INDEX = 1,
	OFFSET_STRING = 2,
	OFFSET_KEY = 3,
};

// Length types for LENGTH streams (4 bits in lower nibble)
enum LengthType {
	LENGTH_VAR_BINARY = 0,
	LENGTH_GEOMETRIES = 1,
	LENGTH_PARTS = 2,
	LENGTH_RINGS = 3,
	LENGTH_TRIANGLES = 4,
	LENGTH_SYMBOL = 5,
	LENGTH_DICTIONARY = 6,
};

// Geometry types (matches MVT but with multi-geometry support)
enum GeometryType {
	GEOM_POINT = 0,
	GEOM_LINESTRING = 1,
	GEOM_POLYGON = 2,
	GEOM_MULTIPOINT = 3,
	GEOM_MULTILINESTRING = 4,
	GEOM_MULTIPOLYGON = 5,
};

// Column type codes for embedded metadata
enum ColumnTypeCode {
	// ID columns (0-3)
	COL_ID_UINT32 = 0,
	COL_ID_UINT64 = 1,
	COL_ID_UINT32_NULLABLE = 2,
	COL_ID_UINT64_NULLABLE = 3,
	// Geometry (4)
	COL_GEOMETRY = 4,
	// Scalar types (10-29): even = non-nullable, odd = nullable
	COL_BOOLEAN = 10,
	COL_BOOLEAN_NULLABLE = 11,
	COL_INT8 = 12,
	COL_INT8_NULLABLE = 13,
	COL_UINT8 = 14,
	COL_UINT8_NULLABLE = 15,
	COL_INT32 = 16,
	COL_INT32_NULLABLE = 17,
	COL_UINT32 = 18,
	COL_UINT32_NULLABLE = 19,
	COL_INT64 = 20,
	COL_INT64_NULLABLE = 21,
	COL_UINT64 = 22,
	COL_UINT64_NULLABLE = 23,
	COL_FLOAT = 24,
	COL_FLOAT_NULLABLE = 25,
	COL_DOUBLE = 26,
	COL_DOUBLE_NULLABLE = 27,
	COL_STRING = 28,
	COL_STRING_NULLABLE = 29,
	// Complex types
	COL_STRUCT = 30,
};

// Stream metadata structure
struct StreamMetadata {
	PhysicalStreamType physical_type;
	int logical_subtype;  // DictionaryType, OffsetType, or LengthType depending on physical_type
	LogicalLevelTechnique llt1;
	LogicalLevelTechnique llt2;
	PhysicalLevelTechnique plt;
	uint32_t num_values;
	uint32_t byte_length;
	// For RLE streams
	uint32_t runs = 0;
	uint32_t num_rle_values = 0;
	// For Morton streams
	uint32_t num_bits = 0;
	uint32_t coordinate_shift = 0;
};

// Property column info for tracking nullable/type info
struct PropertyColumnInfo {
	std::string name;
	int mvt_type;  // mvt_value_type
	bool nullable;
	std::vector<size_t> present;  // indices of features that have this property
};

// Forward declarations
class mlt_tile;

// Encoding functions
void encode_varint(std::string &out, uint64_t value);
void encode_signed_varint(std::string &out, int64_t value);
uint32_t encode_zigzag32(int32_t value);
uint64_t encode_zigzag64(int64_t value);

// Main encoder class
class mlt_tile {
public:
	// Convert from MVT tile to MLT format
	std::string encode(const mvt_tile &tile);

private:
	// Encode a single layer as a feature table
	std::string encode_layer(const mvt_layer &layer);

	// Encode embedded metadata (layer name, extent, columns)
	std::string encode_metadata(const mvt_layer &layer,
				    bool has_ids,
				    bool nullable_ids,
				    bool use_64bit_ids,
				    const std::vector<PropertyColumnInfo> &property_columns);

	// Encode ID column
	std::string encode_id_column(const mvt_layer &layer, bool nullable, bool use_64bit);

	// Encode geometry column
	std::string encode_geometry_column(const mvt_layer &layer);

	// Encode a property column
	std::string encode_property_column(const mvt_layer &layer,
					   const PropertyColumnInfo &col_info);

	// Encode individual streams
	std::string encode_stream_metadata(const StreamMetadata &meta);
	std::string encode_int32_stream(const std::vector<int32_t> &values,
					LogicalLevelTechnique llt1,
					LogicalLevelTechnique llt2);
	std::string encode_int64_stream(const std::vector<int64_t> &values,
					LogicalLevelTechnique llt1,
					LogicalLevelTechnique llt2);
	std::string encode_string_stream(const std::vector<std::string> &values,
					 const std::vector<bool> *present = nullptr);
	std::string encode_boolean_stream(const std::vector<bool> &values);
	std::string encode_float_stream(const std::vector<float> &values);
	std::string encode_double_stream(const std::vector<double> &values);
	std::string encode_present_stream(const std::vector<bool> &present);

	// Helper: analyze properties across all features
	std::vector<PropertyColumnInfo> analyze_properties(const mvt_layer &layer);

	// Helper: get MVT value for a feature's property
	const mvt_value *get_feature_property(const mvt_layer &layer,
					      const mvt_feature &feature,
					      const std::string &key);
};

}  // namespace mlt

#endif  // MLT_HPP
