#include <cstring>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include "mlt.hpp"
#include "protozero/varint.hpp"

namespace mlt {

// Encode unsigned varint
void encode_varint(std::string &out, uint64_t value) {
	while (value > 0x7F) {
		out.push_back((value & 0x7F) | 0x80);
		value >>= 7;
	}
	out.push_back(value & 0x7F);
}

// Encode signed varint with zigzag encoding
void encode_signed_varint(std::string &out, int64_t value) {
	uint64_t zigzag = encode_zigzag64(value);
	encode_varint(out, zigzag);
}

// ZigZag encode a 32-bit signed integer
uint32_t encode_zigzag32(int32_t value) {
	return (static_cast<uint32_t>(value) << 1) ^ (value >> 31);
}

// ZigZag encode a 64-bit signed integer
uint64_t encode_zigzag64(int64_t value) {
	return (static_cast<uint64_t>(value) << 1) ^ (value >> 63);
}

// Encode a length-prefixed UTF-8 string
static void encode_string_with_length(std::string &out, const std::string &str) {
	encode_varint(out, str.size());
	out.append(str);
}

// Convert MVT geometry type to MLT geometry type
static GeometryType mvt_to_mlt_geom_type(int mvt_type, bool is_multi) {
	switch (mvt_type) {
	case mvt_point:
		return is_multi ? GEOM_MULTIPOINT : GEOM_POINT;
	case mvt_linestring:
		return is_multi ? GEOM_MULTILINESTRING : GEOM_LINESTRING;
	case mvt_polygon:
		return is_multi ? GEOM_MULTIPOLYGON : GEOM_POLYGON;
	default:
		return GEOM_POINT;
	}
}

// Check if MVT geometry is multi-geometry (has multiple moveto commands)
static bool is_multi_geometry(const std::vector<mvt_geometry> &geom, int type) {
	if (type == mvt_point) {
		return geom.size() > 1;
	}
	int moveto_count = 0;
	for (const auto &g : geom) {
		if (g.op == mvt_moveto) {
			moveto_count++;
			if (moveto_count > 1) return true;
		}
	}
	return false;
}

// Encode stream metadata header
std::string mlt_tile::encode_stream_metadata(const StreamMetadata &meta) {
	std::string out;
	
	// Byte 1: [PhysicalStreamType:4][LogicalSubtype:4]
	uint8_t byte1 = (static_cast<uint8_t>(meta.physical_type) << 4) | (meta.logical_subtype & 0x0F);
	out.push_back(byte1);
	
	// Byte 2: [LLT1:3][LLT2:3][PLT:2]
	uint8_t byte2 = (static_cast<uint8_t>(meta.llt1) << 5) |
			(static_cast<uint8_t>(meta.llt2) << 2) |
			(static_cast<uint8_t>(meta.plt) & 0x03);
	out.push_back(byte2);
	
	// Variable length: numValues, byteLength
	encode_varint(out, meta.num_values);
	encode_varint(out, meta.byte_length);
	
	// RLE-specific fields
	if ((meta.llt1 == LLT_RLE || meta.llt2 == LLT_RLE) && meta.plt != PLT_NONE) {
		encode_varint(out, meta.runs);
		encode_varint(out, meta.num_rle_values);
	}
	
	// Morton-specific fields
	if (meta.llt1 == LLT_MORTON) {
		encode_varint(out, meta.num_bits);
		encode_varint(out, meta.coordinate_shift);
	}
	
	return out;
}

// Encode int32 stream with specified logical level techniques
std::string mlt_tile::encode_int32_stream(const std::vector<int32_t> &values,
					   LogicalLevelTechnique llt1,
					   LogicalLevelTechnique llt2) {
	if (values.empty()) {
		return "";
	}
	
	std::string data;
	uint32_t runs = 0;
	uint32_t num_rle_values = values.size();
	
	// Apply logical encoding based on technique
	if (llt1 == LLT_RLE || llt2 == LLT_RLE) {
		// RLE encoding: [runLengths...][values...]
		std::vector<uint32_t> run_lengths;
		std::vector<int32_t> run_values;
		
		int32_t current_val = values[0];
		uint32_t current_count = 1;
		
		for (size_t i = 1; i < values.size(); i++) {
			if (values[i] == current_val) {
				current_count++;
			} else {
				run_lengths.push_back(current_count);
				run_values.push_back(current_val);
				current_val = values[i];
				current_count = 1;
			}
		}
		run_lengths.push_back(current_count);
		run_values.push_back(current_val);
		
		runs = run_lengths.size();
		
		// If Delta+RLE, delta-encode the values
		if (llt1 == LLT_DELTA && llt2 == LLT_RLE) {
			std::vector<int32_t> delta_values;
			delta_values.push_back(run_values[0]);
			for (size_t i = 1; i < run_values.size(); i++) {
				delta_values.push_back(run_values[i] - run_values[i-1]);
			}
			run_values = delta_values;
		}
		
		// Encode run lengths
		for (uint32_t len : run_lengths) {
			encode_varint(data, len);
		}
		// Encode values with zigzag
		for (int32_t val : run_values) {
			encode_varint(data, encode_zigzag32(val));
		}
	} else if (llt1 == LLT_DELTA) {
		// Delta encoding
		std::vector<int32_t> deltas;
		deltas.push_back(values[0]);
		for (size_t i = 1; i < values.size(); i++) {
			deltas.push_back(values[i] - values[i-1]);
		}
		// Zigzag + varint
		for (int32_t d : deltas) {
			encode_varint(data, encode_zigzag32(d));
		}
	} else {
		// No logical encoding, just zigzag + varint
		for (int32_t val : values) {
			encode_varint(data, encode_zigzag32(val));
		}
	}
	
	// Build stream with metadata header
	StreamMetadata meta;
	meta.physical_type = STREAM_DATA;
	meta.logical_subtype = DICT_NONE;
	meta.llt1 = llt1;
	meta.llt2 = llt2;
	meta.plt = PLT_VARINT;
	meta.num_values = (llt1 == LLT_RLE || llt2 == LLT_RLE) ? runs * 2 : values.size();
	meta.byte_length = data.size();
	meta.runs = runs;
	meta.num_rle_values = num_rle_values;
	
	std::string result = encode_stream_metadata(meta);
	result.append(data);
	return result;
}

// Encode int64 stream
std::string mlt_tile::encode_int64_stream(const std::vector<int64_t> &values,
					   LogicalLevelTechnique llt1,
					   LogicalLevelTechnique llt2) {
	if (values.empty()) {
		return "";
	}
	
	std::string data;
	uint32_t runs = 0;
	uint32_t num_rle_values = values.size();
	
	if (llt1 == LLT_RLE || llt2 == LLT_RLE) {
		std::vector<uint32_t> run_lengths;
		std::vector<int64_t> run_values;
		
		int64_t current_val = values[0];
		uint32_t current_count = 1;
		
		for (size_t i = 1; i < values.size(); i++) {
			if (values[i] == current_val) {
				current_count++;
			} else {
				run_lengths.push_back(current_count);
				run_values.push_back(current_val);
				current_val = values[i];
				current_count = 1;
			}
		}
		run_lengths.push_back(current_count);
		run_values.push_back(current_val);
		
		runs = run_lengths.size();
		
		// Encode run lengths
		for (uint32_t len : run_lengths) {
			encode_varint(data, len);
		}
		// Encode values with zigzag
		for (int64_t val : run_values) {
			encode_varint(data, encode_zigzag64(val));
		}
	} else if (llt1 == LLT_DELTA) {
		std::vector<int64_t> deltas;
		deltas.push_back(values[0]);
		for (size_t i = 1; i < values.size(); i++) {
			deltas.push_back(values[i] - values[i-1]);
		}
		for (int64_t d : deltas) {
			encode_varint(data, encode_zigzag64(d));
		}
	} else {
		for (int64_t val : values) {
			encode_varint(data, encode_zigzag64(val));
		}
	}
	
	StreamMetadata meta;
	meta.physical_type = STREAM_DATA;
	meta.logical_subtype = DICT_NONE;
	meta.llt1 = llt1;
	meta.llt2 = llt2;
	meta.plt = PLT_VARINT;
	meta.num_values = (llt1 == LLT_RLE || llt2 == LLT_RLE) ? runs * 2 : values.size();
	meta.byte_length = data.size();
	meta.runs = runs;
	meta.num_rle_values = num_rle_values;
	
	std::string result = encode_stream_metadata(meta);
	result.append(data);
	return result;
}

// Encode present (nullability) stream as boolean RLE
std::string mlt_tile::encode_present_stream(const std::vector<bool> &present) {
	// Pack booleans into bytes (8 booleans per byte)
	size_t num_bytes = (present.size() + 7) / 8;
	std::string packed;
	packed.resize(num_bytes, 0);
	
	for (size_t i = 0; i < present.size(); i++) {
		if (present[i]) {
			size_t byte_idx = i / 8;
			size_t bit_idx = i % 8;
			packed[byte_idx] |= (1 << bit_idx);
		}
	}
	
	// Prepend the RLE header byte (256 - num_bytes) mod 256
	std::string data;
	data.push_back(static_cast<char>((256 - num_bytes) & 0xFF));
	data.append(packed);
	
	StreamMetadata meta;
	meta.physical_type = STREAM_PRESENT;
	meta.logical_subtype = 0;
	meta.llt1 = LLT_NONE;
	meta.llt2 = LLT_NONE;
	meta.plt = PLT_VARINT;
	meta.num_values = present.size();
	meta.byte_length = data.size();
	
	std::string result = encode_stream_metadata(meta);
	result.append(data);
	return result;
}

// Encode boolean stream
std::string mlt_tile::encode_boolean_stream(const std::vector<bool> &values) {
	size_t num_bytes = (values.size() + 7) / 8;
	std::string packed;
	packed.resize(num_bytes, 0);
	
	for (size_t i = 0; i < values.size(); i++) {
		if (values[i]) {
			size_t byte_idx = i / 8;
			size_t bit_idx = i % 8;
			packed[byte_idx] |= (1 << bit_idx);
		}
	}
	
	std::string data;
	data.push_back(static_cast<char>((256 - num_bytes) & 0xFF));
	data.append(packed);
	
	StreamMetadata meta;
	meta.physical_type = STREAM_DATA;
	meta.logical_subtype = DICT_NONE;
	meta.llt1 = LLT_NONE;
	meta.llt2 = LLT_NONE;
	meta.plt = PLT_VARINT;
	meta.num_values = values.size();
	meta.byte_length = data.size();
	
	std::string result = encode_stream_metadata(meta);
	result.append(data);
	return result;
}

// Encode float stream (little-endian)
std::string mlt_tile::encode_float_stream(const std::vector<float> &values) {
	std::string data;
	data.resize(values.size() * sizeof(float));
	memcpy(&data[0], values.data(), data.size());
	
	StreamMetadata meta;
	meta.physical_type = STREAM_DATA;
	meta.logical_subtype = DICT_NONE;
	meta.llt1 = LLT_NONE;
	meta.llt2 = LLT_NONE;
	meta.plt = PLT_NONE;
	meta.num_values = values.size();
	meta.byte_length = data.size();
	
	std::string result = encode_stream_metadata(meta);
	result.append(data);
	return result;
}

// Encode double stream (little-endian)
std::string mlt_tile::encode_double_stream(const std::vector<double> &values) {
	std::string data;
	data.resize(values.size() * sizeof(double));
	memcpy(&data[0], values.data(), data.size());
	
	StreamMetadata meta;
	meta.physical_type = STREAM_DATA;
	meta.logical_subtype = DICT_NONE;
	meta.llt1 = LLT_NONE;
	meta.llt2 = LLT_NONE;
	meta.plt = PLT_NONE;
	meta.num_values = values.size();
	meta.byte_length = data.size();
	
	std::string result = encode_stream_metadata(meta);
	result.append(data);
	return result;
}

// Encode string stream with dictionary compression
std::string mlt_tile::encode_string_stream(const std::vector<std::string> &values,
					    const std::vector<bool> *present) {
	std::string result;
	
	// Build dictionary of unique strings
	std::unordered_map<std::string, uint32_t> dict_map;
	std::vector<std::string> dict_values;
	std::vector<uint32_t> offsets;
	
	for (size_t i = 0; i < values.size(); i++) {
		if (present && !(*present)[i]) {
			continue;  // Skip null values
		}
		
		const std::string &s = values[i];
		auto it = dict_map.find(s);
		if (it == dict_map.end()) {
			uint32_t idx = dict_values.size();
			dict_map[s] = idx;
			dict_values.push_back(s);
			offsets.push_back(idx);
		} else {
			offsets.push_back(it->second);
		}
	}
	
	// Present stream (if nullable)
	if (present) {
		result.append(encode_present_stream(*present));
	}
	
	// Offset stream (indices into dictionary)
	{
		std::string offset_data;
		for (uint32_t off : offsets) {
			encode_varint(offset_data, off);
		}
		
		StreamMetadata meta;
		meta.physical_type = STREAM_OFFSET;
		meta.logical_subtype = OFFSET_STRING;
		meta.llt1 = LLT_NONE;
		meta.llt2 = LLT_NONE;
		meta.plt = PLT_VARINT;
		meta.num_values = offsets.size();
		meta.byte_length = offset_data.size();
		
		result.append(encode_stream_metadata(meta));
		result.append(offset_data);
	}
	
	// Length stream (string lengths in dictionary)
	{
		std::string length_data;
		for (const auto &s : dict_values) {
			encode_varint(length_data, s.size());
		}
		
		StreamMetadata meta;
		meta.physical_type = STREAM_LENGTH;
		meta.logical_subtype = LENGTH_DICTIONARY;
		meta.llt1 = LLT_NONE;
		meta.llt2 = LLT_NONE;
		meta.plt = PLT_VARINT;
		meta.num_values = dict_values.size();
		meta.byte_length = length_data.size();
		
		result.append(encode_stream_metadata(meta));
		result.append(length_data);
	}
	
	// Data stream (concatenated dictionary strings)
	{
		std::string string_data;
		for (const auto &s : dict_values) {
			string_data.append(s);
		}
		
		StreamMetadata meta;
		meta.physical_type = STREAM_DATA;
		meta.logical_subtype = DICT_SINGLE;
		meta.llt1 = LLT_NONE;
		meta.llt2 = LLT_NONE;
		meta.plt = PLT_NONE;
		meta.num_values = 0;  // Not applicable for raw string data
		meta.byte_length = string_data.size();
		
		result.append(encode_stream_metadata(meta));
		result.append(string_data);
	}
	
	return result;
}

// Get property value for a feature
const mvt_value *mlt_tile::get_feature_property(const mvt_layer &layer,
						 const mvt_feature &feature,
						 const std::string &key) {
	for (size_t i = 0; i + 1 < feature.tags.size(); i += 2) {
		if (feature.tags[i] < layer.keys.size() && layer.keys[feature.tags[i]] == key) {
			if (feature.tags[i + 1] < layer.values.size()) {
				return &layer.values[feature.tags[i + 1]];
			}
		}
	}
	return nullptr;
}

// Analyze properties across all features to determine column types
std::vector<PropertyColumnInfo> mlt_tile::analyze_properties(const mvt_layer &layer) {
	std::map<std::string, PropertyColumnInfo> prop_map;
	
	for (size_t fi = 0; fi < layer.features.size(); fi++) {
		const mvt_feature &feature = layer.features[fi];
		std::unordered_set<std::string> seen_keys;
		
		for (size_t i = 0; i + 1 < feature.tags.size(); i += 2) {
			if (feature.tags[i] >= layer.keys.size() || feature.tags[i + 1] >= layer.values.size()) {
				continue;
			}
			
			const std::string &key = layer.keys[feature.tags[i]];
			const mvt_value &val = layer.values[feature.tags[i + 1]];
			
			seen_keys.insert(key);
			
			auto it = prop_map.find(key);
			if (it == prop_map.end()) {
				PropertyColumnInfo info;
				info.name = key;
				info.mvt_type = val.type;
				info.nullable = false;
				info.present.push_back(fi);
				prop_map[key] = info;
			} else {
				it->second.present.push_back(fi);
				// Update type if mixed (promote to more general type)
				if (it->second.mvt_type != val.type) {
					// If mixing numeric types, use double
					if (it->second.mvt_type >= mvt_float && it->second.mvt_type <= mvt_sint &&
					    val.type >= mvt_float && val.type <= mvt_sint) {
						it->second.mvt_type = mvt_double;
					} else {
						// Fall back to string for mixed types
						it->second.mvt_type = mvt_string;
					}
				}
			}
		}
	}
	
	// Mark columns as nullable if not all features have the property
	std::vector<PropertyColumnInfo> result;
	for (auto &kv : prop_map) {
		kv.second.nullable = kv.second.present.size() < layer.features.size();
		result.push_back(kv.second);
	}
	
	return result;
}

// Encode embedded metadata for a layer
std::string mlt_tile::encode_metadata(const mvt_layer &layer,
				       bool has_ids,
				       bool nullable_ids,
				       bool use_64bit_ids,
				       const std::vector<PropertyColumnInfo> &property_columns) {
	std::string out;
	
	// Layer name
	encode_string_with_length(out, layer.name);
	
	// Extent
	encode_varint(out, layer.extent);
	
	// Column count: id (optional) + geometry + properties
	size_t column_count = 1;  // geometry is always present
	if (has_ids) column_count++;
	column_count += property_columns.size();
	encode_varint(out, column_count);
	
	// ID column (if present)
	if (has_ids) {
		int type_code;
		if (use_64bit_ids) {
			type_code = nullable_ids ? COL_ID_UINT64_NULLABLE : COL_ID_UINT64;
		} else {
			type_code = nullable_ids ? COL_ID_UINT32_NULLABLE : COL_ID_UINT32;
		}
		encode_varint(out, type_code);
		// ID columns don't need explicit name
	}
	
	// Geometry column
	encode_varint(out, COL_GEOMETRY);
	// Geometry columns don't need explicit name
	
	// Property columns
	for (const auto &col : property_columns) {
		int type_code;
		switch (col.mvt_type) {
		case mvt_string:
			type_code = col.nullable ? COL_STRING_NULLABLE : COL_STRING;
			break;
		case mvt_float:
			type_code = col.nullable ? COL_FLOAT_NULLABLE : COL_FLOAT;
			break;
		case mvt_double:
			type_code = col.nullable ? COL_DOUBLE_NULLABLE : COL_DOUBLE;
			break;
		case mvt_int:
		case mvt_sint:
			type_code = col.nullable ? COL_INT64_NULLABLE : COL_INT64;
			break;
		case mvt_uint:
			type_code = col.nullable ? COL_UINT64_NULLABLE : COL_UINT64;
			break;
		case mvt_bool:
			type_code = col.nullable ? COL_BOOLEAN_NULLABLE : COL_BOOLEAN;
			break;
		default:
			type_code = col.nullable ? COL_STRING_NULLABLE : COL_STRING;
			break;
		}
		encode_varint(out, type_code);
		encode_string_with_length(out, col.name);
	}
	
	return out;
}

// Encode ID column
std::string mlt_tile::encode_id_column(const mvt_layer &layer, bool nullable, bool use_64bit) {
	std::string result;
	
	std::vector<bool> present;
	std::vector<int64_t> ids;
	
	for (const auto &feature : layer.features) {
		if (nullable) {
			present.push_back(feature.has_id);
		}
		if (feature.has_id) {
			ids.push_back(static_cast<int64_t>(feature.id));
		}
	}
	
	// Present stream (if nullable)
	if (nullable) {
		result.append(encode_present_stream(present));
	}
	
	// ID data stream - try RLE first if there are sequential IDs
	bool use_rle = false;
	if (ids.size() >= 2) {
		// Check if IDs form a simple sequence (common case)
		bool is_sequence = true;
		int64_t delta = ids[1] - ids[0];
		for (size_t i = 2; i < ids.size() && is_sequence; i++) {
			if (ids[i] - ids[i-1] != delta) {
				is_sequence = false;
			}
		}
		use_rle = is_sequence;
	}
	
	if (use_rle && ids.size() >= 2) {
		// Use Delta+RLE for sequential IDs
		result.append(encode_int64_stream(ids, LLT_DELTA, LLT_RLE));
	} else {
		// Use plain delta encoding
		result.append(encode_int64_stream(ids, LLT_DELTA, LLT_NONE));
	}
	
	return result;
}

// Encode geometry column
std::string mlt_tile::encode_geometry_column(const mvt_layer &layer) {
	std::string result;
	
	// Collect geometry types
	std::vector<int32_t> geom_types;
	
	// Topology data
	std::vector<int32_t> geometry_lengths;  // For multi-geometries
	std::vector<int32_t> part_lengths;      // For polygons/linestrings
	std::vector<int32_t> ring_lengths;      // For polygon rings
	
	// Vertex data (interleaved x, y)
	std::vector<int32_t> vertices;
	
	bool has_multi = false;
	bool has_rings = false;
	bool has_parts = false;
	
	for (const auto &feature : layer.features) {
		const auto &geom = feature.geometry;
		bool is_multi = is_multi_geometry(geom, feature.type);
		GeometryType mlt_type = mvt_to_mlt_geom_type(feature.type, is_multi);
		geom_types.push_back(static_cast<int32_t>(mlt_type));
		
		if (is_multi) has_multi = true;
		if (feature.type == mvt_polygon) has_rings = true;
		if (feature.type == mvt_polygon || feature.type == mvt_linestring) has_parts = true;
		
		// Parse geometry and extract topology
		std::vector<std::vector<std::vector<std::pair<long long, long long>>>> rings_data;
		std::vector<std::pair<long long, long long>> current_ring;
		
		for (size_t i = 0; i < geom.size(); i++) {
			const auto &g = geom[i];
			
			if (g.op == mvt_moveto) {
				if (!current_ring.empty()) {
					// Save previous ring
					if (rings_data.empty()) {
						rings_data.push_back({});
					}
					if (rings_data.back().empty()) {
						rings_data.back().push_back({});
					}
					rings_data.back().back() = current_ring;
					rings_data.back().push_back({});
					current_ring.clear();
				}
				current_ring.push_back({g.x, g.y});
			} else if (g.op == mvt_lineto) {
				current_ring.push_back({g.x, g.y});
			} else if (g.op == mvt_closepath) {
				// Close polygon ring
				if (!current_ring.empty()) {
					if (rings_data.empty()) {
						rings_data.push_back({});
					}
					rings_data.back().push_back(current_ring);
					current_ring.clear();
				}
			}
		}
		
		// Handle remaining points
		if (!current_ring.empty()) {
			if (rings_data.empty()) {
				rings_data.push_back({});
			}
			rings_data.back().push_back(current_ring);
		}
		
		// Record topology and vertices based on geometry type
		if (is_multi && mlt_type >= GEOM_MULTIPOINT) {
			geometry_lengths.push_back(rings_data.size());
		}
		
		for (const auto &part : rings_data) {
			if (feature.type == mvt_polygon && part.size() > 0) {
				part_lengths.push_back(part.size());
			} else if (feature.type == mvt_linestring && is_multi) {
				// For multi-linestring, each part is a separate linestring
			}
			
			for (const auto &ring : part) {
				if (feature.type == mvt_polygon) {
					ring_lengths.push_back(ring.size());
				} else if (feature.type == mvt_linestring) {
					part_lengths.push_back(ring.size());
				}
				
				// Add vertices
				for (const auto &coord : ring) {
					vertices.push_back(static_cast<int32_t>(coord.first));
					vertices.push_back(static_cast<int32_t>(coord.second));
				}
			}
		}
	}
	
	// Count number of streams
	int num_streams = 1;  // geometry type stream
	if (has_multi) num_streams++;  // geometry lengths
	if (has_parts) num_streams++;  // part lengths
	if (has_rings) num_streams++;  // ring lengths
	num_streams++;  // vertex buffer
	
	// Write stream count
	encode_varint(result, num_streams);
	
	// Geometry type stream (use RLE if all same type)
	bool all_same_type = true;
	for (size_t i = 1; i < geom_types.size() && all_same_type; i++) {
		if (geom_types[i] != geom_types[0]) {
			all_same_type = false;
		}
	}
	
	if (all_same_type && !geom_types.empty()) {
		// Constant geometry type - use RLE with single run
		std::string data;
		encode_varint(data, geom_types.size());  // run length
		encode_varint(data, encode_zigzag32(geom_types[0]));  // value
		
		StreamMetadata meta;
		meta.physical_type = STREAM_DATA;
		meta.logical_subtype = DICT_NONE;
		meta.llt1 = LLT_RLE;
		meta.llt2 = LLT_NONE;
		meta.plt = PLT_VARINT;
		meta.num_values = 2;  // 1 run length + 1 value
		meta.byte_length = data.size();
		meta.runs = 1;
		meta.num_rle_values = geom_types.size();
		
		result.append(encode_stream_metadata(meta));
		result.append(data);
	} else {
		result.append(encode_int32_stream(geom_types, LLT_RLE, LLT_NONE));
	}
	
	// Geometry lengths stream (for multi-geometries)
	if (has_multi && !geometry_lengths.empty()) {
		std::string data;
		for (int32_t len : geometry_lengths) {
			encode_varint(data, len);
		}
		
		StreamMetadata meta;
		meta.physical_type = STREAM_LENGTH;
		meta.logical_subtype = LENGTH_GEOMETRIES;
		meta.llt1 = LLT_NONE;
		meta.llt2 = LLT_NONE;
		meta.plt = PLT_VARINT;
		meta.num_values = geometry_lengths.size();
		meta.byte_length = data.size();
		
		result.append(encode_stream_metadata(meta));
		result.append(data);
	}
	
	// Part lengths stream
	if (has_parts && !part_lengths.empty()) {
		std::string data;
		for (int32_t len : part_lengths) {
			encode_varint(data, len);
		}
		
		StreamMetadata meta;
		meta.physical_type = STREAM_LENGTH;
		meta.logical_subtype = LENGTH_PARTS;
		meta.llt1 = LLT_NONE;
		meta.llt2 = LLT_NONE;
		meta.plt = PLT_VARINT;
		meta.num_values = part_lengths.size();
		meta.byte_length = data.size();
		
		result.append(encode_stream_metadata(meta));
		result.append(data);
	}
	
	// Ring lengths stream
	if (has_rings && !ring_lengths.empty()) {
		std::string data;
		for (int32_t len : ring_lengths) {
			encode_varint(data, len);
		}
		
		StreamMetadata meta;
		meta.physical_type = STREAM_LENGTH;
		meta.logical_subtype = LENGTH_RINGS;
		meta.llt1 = LLT_NONE;
		meta.llt2 = LLT_NONE;
		meta.plt = PLT_VARINT;
		meta.num_values = ring_lengths.size();
		meta.byte_length = data.size();
		
		result.append(encode_stream_metadata(meta));
		result.append(data);
	}
	
	// Vertex buffer stream (interleaved x, y with delta encoding)
	{
		std::string data;
		int32_t prev_x = 0, prev_y = 0;
		for (size_t i = 0; i + 1 < vertices.size(); i += 2) {
			int32_t dx = vertices[i] - prev_x;
			int32_t dy = vertices[i + 1] - prev_y;
			encode_varint(data, encode_zigzag32(dx));
			encode_varint(data, encode_zigzag32(dy));
			prev_x = vertices[i];
			prev_y = vertices[i + 1];
		}
		
		StreamMetadata meta;
		meta.physical_type = STREAM_DATA;
		meta.logical_subtype = DICT_VERTEX;
		meta.llt1 = LLT_COMPONENTWISE_DELTA;
		meta.llt2 = LLT_NONE;
		meta.plt = PLT_VARINT;
		meta.num_values = vertices.size();
		meta.byte_length = data.size();
		
		result.append(encode_stream_metadata(meta));
		result.append(data);
	}
	
	return result;
}

// Encode a property column
std::string mlt_tile::encode_property_column(const mvt_layer &layer,
					      const PropertyColumnInfo &col_info) {
	std::string result;
	size_t num_features = layer.features.size();
	
	// Build present vector and collect values
	std::vector<bool> present(num_features, false);
	std::unordered_set<size_t> present_set(col_info.present.begin(), col_info.present.end());
	
	for (size_t i = 0; i < num_features; i++) {
		present[i] = present_set.count(i) > 0;
	}
	
	// Add stream count for string columns
	if (col_info.mvt_type == mvt_string) {
		int num_streams = col_info.nullable ? 4 : 3;  // present + offset + length + data
		encode_varint(result, num_streams);
	}
	
	switch (col_info.mvt_type) {
	case mvt_string: {
		std::vector<std::string> values(num_features);
		for (size_t i = 0; i < num_features; i++) {
			if (present[i]) {
				const mvt_value *v = get_feature_property(layer, layer.features[i], col_info.name);
				if (v) {
					values[i] = v->get_string_value();
				}
			}
		}
		result.append(encode_string_stream(values, col_info.nullable ? &present : nullptr));
		break;
	}
	
	case mvt_bool: {
		if (col_info.nullable) {
			result.append(encode_present_stream(present));
		}
		std::vector<bool> values;
		for (size_t i = 0; i < num_features; i++) {
			if (present[i]) {
				const mvt_value *v = get_feature_property(layer, layer.features[i], col_info.name);
				if (v) {
					values.push_back(v->numeric_value.bool_value);
				}
			}
		}
		result.append(encode_boolean_stream(values));
		break;
	}
	
	case mvt_float: {
		if (col_info.nullable) {
			result.append(encode_present_stream(present));
		}
		std::vector<float> values;
		for (size_t i = 0; i < num_features; i++) {
			if (present[i]) {
				const mvt_value *v = get_feature_property(layer, layer.features[i], col_info.name);
				if (v) {
					values.push_back(v->numeric_value.float_value);
				}
			}
		}
		result.append(encode_float_stream(values));
		break;
	}
	
	case mvt_double: {
		if (col_info.nullable) {
			result.append(encode_present_stream(present));
		}
		std::vector<double> values;
		for (size_t i = 0; i < num_features; i++) {
			if (present[i]) {
				const mvt_value *v = get_feature_property(layer, layer.features[i], col_info.name);
				if (v) {
					values.push_back(v->to_double());
				}
			}
		}
		result.append(encode_double_stream(values));
		break;
	}
	
	case mvt_int:
	case mvt_sint:
	case mvt_uint: {
		if (col_info.nullable) {
			result.append(encode_present_stream(present));
		}
		std::vector<int64_t> values;
		for (size_t i = 0; i < num_features; i++) {
			if (present[i]) {
				const mvt_value *v = get_feature_property(layer, layer.features[i], col_info.name);
				if (v) {
					values.push_back(mvt_value_to_long_long(*v));
				}
			}
		}
		result.append(encode_int64_stream(values, LLT_NONE, LLT_NONE));
		break;
	}
	
	default:
		// Treat as string
		{
			std::vector<std::string> values(num_features);
			for (size_t i = 0; i < num_features; i++) {
				if (present[i]) {
					const mvt_value *v = get_feature_property(layer, layer.features[i], col_info.name);
					if (v) {
						values[i] = v->toString();
					}
				}
			}
			encode_varint(result, col_info.nullable ? 4 : 3);
			result.append(encode_string_stream(values, col_info.nullable ? &present : nullptr));
		}
		break;
	}
	
	return result;
}

// Encode a single layer as a feature table
std::string mlt_tile::encode_layer(const mvt_layer &layer) {
	if (layer.features.empty()) {
		return "";
	}
	
	// Analyze features to determine column structure
	bool has_ids = false;
	bool nullable_ids = false;
	bool use_64bit_ids = false;
	
	for (const auto &feature : layer.features) {
		if (feature.has_id) {
			has_ids = true;
			if (feature.id > UINT32_MAX) {
				use_64bit_ids = true;
			}
		} else if (has_ids) {
			nullable_ids = true;
		}
	}
	
	// If some features have IDs and some don't, IDs are nullable
	if (has_ids) {
		for (const auto &feature : layer.features) {
			if (!feature.has_id) {
				nullable_ids = true;
				break;
			}
		}
	}
	
	// Analyze properties
	auto property_columns = analyze_properties(layer);
	
	// Encode layer content
	std::string content;
	
	// Embedded metadata
	content.append(encode_metadata(layer, has_ids, nullable_ids, use_64bit_ids, property_columns));
	
	// ID column (if any features have IDs)
	if (has_ids) {
		content.append(encode_id_column(layer, nullable_ids, use_64bit_ids));
	}
	
	// Geometry column
	content.append(encode_geometry_column(layer));
	
	// Property columns
	for (const auto &col_info : property_columns) {
		content.append(encode_property_column(layer, col_info));
	}
	
	// Wrap in block with length prefix and tag
	std::string block;
	std::string inner;
	encode_varint(inner, 1);  // Tag 0x01 for embedded metadata format
	inner.append(content);
	
	encode_varint(block, inner.size());  // Block length
	block.append(inner);
	
	return block;
}

// Main encode function: convert MVT tile to MLT format
std::string mlt_tile::encode(const mvt_tile &tile) {
	std::string result;
	
	for (const auto &layer : tile.layers) {
		result.append(encode_layer(layer));
	}
	
	return result;
}

}  // namespace mlt
