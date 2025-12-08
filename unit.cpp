#define CATCH_CONFIG_MAIN
#include "catch/catch.hpp"
#include "text.hpp"
#include "sort.hpp"
#include "tile-cache.hpp"
#include "mvt.hpp"
#include "mlt.hpp"
#include "projection.hpp"
#include "geometry.hpp"
#include <unistd.h>
#include <limits.h>

TEST_CASE("UTF-8 enforcement", "[utf8]") {
	REQUIRE(check_utf8("") == std::string(""));
	REQUIRE(check_utf8("hello world") == std::string(""));
	REQUIRE(check_utf8("Καλημέρα κόσμε") == std::string(""));
	REQUIRE(check_utf8("こんにちは 世界") == std::string(""));
	REQUIRE(check_utf8("👋🌏") == std::string(""));
	REQUIRE(check_utf8("Hola m\xF3n") == std::string("\"Hola m\xF3n\" is not valid UTF-8 (0xF3 0x6E)"));
}

TEST_CASE("UTF-8 truncation", "[trunc]") {
	REQUIRE(truncate16("0123456789abcdefghi", 16) == std::string("0123456789abcdef"));
	REQUIRE(truncate16("0123456789éîôüéîôüç", 16) == std::string("0123456789éîôüéî"));
	REQUIRE(truncate16("0123456789😀😬😁😂😃😄😅😆", 16) == std::string("0123456789😀😬😁"));
	REQUIRE(truncate16("0123456789😀😬😁😂😃😄😅😆", 17) == std::string("0123456789😀😬😁"));
	REQUIRE(truncate16("0123456789あいうえおかきくけこさ", 16) == std::string("0123456789あいうえおか"));

	REQUIRE(truncate_string("789éîôüéîôüç", 3) == std::string("789"));
	REQUIRE(truncate_string("789éîôüéîôüç", 4) == std::string("789"));
	REQUIRE(truncate_string("789éîôüéîôüç", 5) == std::string("789é"));
	REQUIRE(truncate_string("789éîôüéîôüç", 6) == std::string("789é"));
	REQUIRE(truncate_string("789éîôüéîôüç", 7) == std::string("789éî"));
	REQUIRE(truncate_string("789éîôüéîôüç", 8) == std::string("789éî"));

	REQUIRE(truncate_string("0123456789😀😬😁😂😃😄😅😆", 10) == std::string("0123456789"));
	REQUIRE(truncate_string("0123456789😀😬😁😂😃😄😅😆", 11) == std::string("0123456789"));
	REQUIRE(truncate_string("0123456789😀😬😁😂😃😄😅😆", 12) == std::string("0123456789"));
	REQUIRE(truncate_string("0123456789😀😬😁😂😃😄😅😆", 13) == std::string("0123456789"));
	REQUIRE(truncate_string("0123456789😀😬😁😂😃😄😅😆", 14) == std::string("0123456789😀"));

	REQUIRE(truncate_string("😀", 4) == std::string("😀"));
	REQUIRE(truncate_string("😀", 3) == std::string(""));
	REQUIRE(truncate_string("😀", 2) == std::string(""));
	REQUIRE(truncate_string("😀", 1) == std::string(""));
	REQUIRE(truncate_string("😀", 0) == std::string(""));
}

int intcmp(const void *v1, const void *v2) {
	return *((int *) v1) - *((int *) v2);
}

TEST_CASE("External quicksort", "fqsort") {
	std::vector<FILE *> inputs;

	size_t written = 0;
	for (size_t i = 0; i < 5; i++) {
		std::string tmpname = "/tmp/in.XXXXXXX";
		int fd = mkstemp((char *) tmpname.c_str());
		unlink(tmpname.c_str());
		FILE *f = fdopen(fd, "w+b");
		inputs.emplace_back(f);
		size_t iterations = 2000 + rand() % 200;
		for (size_t j = 0; j < iterations; j++) {
			int n = rand();
			fwrite((void *) &n, sizeof(int), 1, f);
			written++;
		}
		rewind(f);
	}

	std::string tmpname = "/tmp/out.XXXXXX";
	int fd = mkstemp((char *) tmpname.c_str());
	unlink(tmpname.c_str());
	FILE *f = fdopen(fd, "w+b");

	fqsort(inputs, sizeof(int), intcmp, f, 256, "/tmp");
	rewind(f);

	int prev = INT_MIN;
	int here;
	size_t nread = 0;
	while (fread((void *) &here, sizeof(int), 1, f)) {
		REQUIRE(here >= prev);
		prev = here;
		nread++;
	}

	fclose(f);
	REQUIRE(nread == written);
}

mvt_tile mock_get_tile(zxy tile) {
	mvt_layer l;
	l.name = std::to_string(tile.z) + "/" + std::to_string(tile.x) + "/" + std::to_string(tile.y);
	mvt_tile t;
	t.layers.push_back(l);
	return t;
}

TEST_CASE("Tile-join cache", "tile cache") {
	tile_cache tc;
	tc.capacity = 5;

	REQUIRE(tc.get(zxy(11, 327, 791), mock_get_tile).layers[0].name == "11/327/791");
	REQUIRE(tc.get(zxy(11, 5, 7), mock_get_tile).layers[0].name == "11/5/7");
	REQUIRE(tc.get(zxy(11, 5, 8), mock_get_tile).layers[0].name == "11/5/8");
	REQUIRE(tc.get(zxy(11, 5, 9), mock_get_tile).layers[0].name == "11/5/9");
	REQUIRE(tc.get(zxy(11, 5, 10), mock_get_tile).layers[0].name == "11/5/10");
	REQUIRE(tc.get(zxy(11, 327, 791), mock_get_tile).layers[0].name == "11/327/791");
	REQUIRE(tc.overzoom_cache.size() == 5);
	REQUIRE(tc.overzoom_cache.find(zxy(11, 327, 791)) != tc.overzoom_cache.end());
	REQUIRE(tc.overzoom_cache.find(zxy(11, 5, 7)) != tc.overzoom_cache.end());

	// verify that additional gets evict the least-recently-used elements

	REQUIRE(tc.get(zxy(11, 5, 11), mock_get_tile).layers[0].name == "11/5/11");
	REQUIRE(tc.overzoom_cache.size() == 5);
	REQUIRE(tc.overzoom_cache.find(zxy(11, 5, 7)) == tc.overzoom_cache.end());

	REQUIRE(tc.get(zxy(11, 5, 12), mock_get_tile).layers[0].name == "11/5/12");
	REQUIRE(tc.overzoom_cache.size() == 5);
	REQUIRE(tc.overzoom_cache.find(zxy(11, 5, 8)) == tc.overzoom_cache.end());
}

TEST_CASE("Bit reversal", "bit reversal") {
	REQUIRE(bit_reverse(1) == 0x8000000000000000);
	REQUIRE(bit_reverse(0x1234567812489BCF) == 0xF3D912481E6A2C48);
	REQUIRE(bit_reverse(0xF3D912481E6A2C48) == 0x1234567812489BCF);
}

TEST_CASE("line_is_too_small") {
	drawvec dv;
	dv.emplace_back(VT_MOVETO, 4243099709, 2683872952);
	dv.emplace_back(VT_LINETO, 4243102487, 2683873977);
	dv.emplace_back(VT_MOVETO, -51867587, 2683872952);
	dv.emplace_back(VT_LINETO, -51864809, 2683873977);
	REQUIRE(line_is_too_small(dv, 0, 10));
}

// ============================================================================
// MLT (MapLibre Tile) encoding tests
// ============================================================================

// Helper to convert string to hex for easier debugging
static std::string to_hex(const std::string &s) {
	std::string result;
	const char hex[] = "0123456789abcdef";
	for (unsigned char c : s) {
		result.push_back(hex[c >> 4]);
		result.push_back(hex[c & 0x0f]);
	}
	return result;
}

TEST_CASE("MLT varint encoding", "[mlt]") {
	SECTION("Single byte values (0-127)") {
		std::string out;
		mlt::encode_varint(out, 0);
		REQUIRE(out == std::string("\x00", 1));

		out.clear();
		mlt::encode_varint(out, 1);
		REQUIRE(out == std::string("\x01", 1));

		out.clear();
		mlt::encode_varint(out, 127);
		REQUIRE(out == std::string("\x7f", 1));
	}

	SECTION("Two byte values (128-16383)") {
		std::string out;
		mlt::encode_varint(out, 128);
		REQUIRE(out == std::string("\x80\x01", 2));

		out.clear();
		mlt::encode_varint(out, 300);
		// 300 = 0b100101100 = 0xAC 0x02
		REQUIRE(out == std::string("\xac\x02", 2));

		out.clear();
		mlt::encode_varint(out, 16383);
		REQUIRE(out == std::string("\xff\x7f", 2));
	}

	SECTION("Larger values") {
		std::string out;
		mlt::encode_varint(out, 16384);
		REQUIRE(out == std::string("\x80\x80\x01", 3));

		out.clear();
		mlt::encode_varint(out, 2097151);
		REQUIRE(out == std::string("\xff\xff\x7f", 3));
	}

	SECTION("Large 64-bit value") {
		std::string out;
		mlt::encode_varint(out, 0xFFFFFFFFFFFFFFFFULL);
		// Maximum varint is 10 bytes for 64-bit
		REQUIRE(out.size() == 10);
	}
}

TEST_CASE("MLT zigzag encoding", "[mlt]") {
	SECTION("32-bit zigzag") {
		REQUIRE(mlt::encode_zigzag32(0) == 0);
		REQUIRE(mlt::encode_zigzag32(-1) == 1);
		REQUIRE(mlt::encode_zigzag32(1) == 2);
		REQUIRE(mlt::encode_zigzag32(-2) == 3);
		REQUIRE(mlt::encode_zigzag32(2) == 4);
		REQUIRE(mlt::encode_zigzag32(2147483647) == 4294967294U);
		REQUIRE(mlt::encode_zigzag32(-2147483648) == 4294967295U);
	}

	SECTION("64-bit zigzag") {
		REQUIRE(mlt::encode_zigzag64(0) == 0);
		REQUIRE(mlt::encode_zigzag64(-1) == 1);
		REQUIRE(mlt::encode_zigzag64(1) == 2);
		REQUIRE(mlt::encode_zigzag64(-2) == 3);
		REQUIRE(mlt::encode_zigzag64(2) == 4);
	}
}

TEST_CASE("MLT signed varint encoding", "[mlt]") {
	SECTION("Zero") {
		std::string out;
		mlt::encode_signed_varint(out, 0);
		REQUIRE(out == std::string("\x00", 1));
	}

	SECTION("Positive values") {
		std::string out;
		mlt::encode_signed_varint(out, 1);
		REQUIRE(out == std::string("\x02", 1));  // zigzag(1) = 2

		out.clear();
		mlt::encode_signed_varint(out, 100);
		REQUIRE(out == std::string("\xc8\x01", 2));  // zigzag(100) = 200 = 0xC8,0x01
	}

	SECTION("Negative values") {
		std::string out;
		mlt::encode_signed_varint(out, -1);
		REQUIRE(out == std::string("\x01", 1));  // zigzag(-1) = 1

		out.clear();
		mlt::encode_signed_varint(out, -100);
		REQUIRE(out == std::string("\xc7\x01", 2));  // zigzag(-100) = 199 = 0xC7,0x01
	}
}

TEST_CASE("MLT tile encoding from MVT", "[mlt]") {
	SECTION("Empty tile") {
		mvt_tile tile;
		mlt::mlt_tile encoder;
		std::string result = encoder.encode(tile);
		// Empty tile should produce minimal output
		REQUIRE(result.empty());
	}

	SECTION("Single layer with single point feature") {
		mvt_tile tile;
		mvt_layer layer;
		layer.name = "test";
		layer.version = 2;
		layer.extent = 4096;

		// Add a key/value
		layer.keys.push_back("name");
		mvt_value val;
		val.type = mvt_string;
		val.set_string_value("point1");
		layer.values.push_back(val);

		// Add a point feature
		mvt_feature feature;
		feature.type = mvt_point;
		feature.id = 1;
		feature.has_id = true;
		feature.tags.push_back(0);  // key index
		feature.tags.push_back(0);  // value index

		feature.geometry.push_back(mvt_geometry(mvt_moveto, 1000, 2000));

		layer.features.push_back(feature);
		tile.layers.push_back(layer);

		mlt::mlt_tile encoder;
		std::string result = encoder.encode(tile);

		// Verify the result is not empty and has expected structure
		REQUIRE(!result.empty());
		// First few bytes should contain layer metadata
		// The exact format depends on implementation but we can verify basic structure
		REQUIRE(result.size() > 10);  // Should have meaningful content
	}

	SECTION("Layer with multiple geometry types") {
		mvt_tile tile;
		mvt_layer layer;
		layer.name = "mixed";
		layer.version = 2;
		layer.extent = 4096;

		// Point feature
		mvt_feature point;
		point.type = mvt_point;
		point.id = 1;
		point.has_id = true;
		point.geometry.push_back(mvt_geometry(mvt_moveto, 100, 200));
		layer.features.push_back(point);

		// LineString feature
		mvt_feature line;
		line.type = mvt_linestring;
		line.id = 2;
		line.has_id = true;
		line.geometry.push_back(mvt_geometry(mvt_moveto, 0, 0));
		line.geometry.push_back(mvt_geometry(mvt_lineto, 100, 100));
		layer.features.push_back(line);

		tile.layers.push_back(layer);

		mlt::mlt_tile encoder;
		std::string result = encoder.encode(tile);

		REQUIRE(!result.empty());
		REQUIRE(result.size() > 20);  // Mixed geometries need more space
	}

	SECTION("Layer with nullable properties") {
		mvt_tile tile;
		mvt_layer layer;
		layer.name = "props";
		layer.version = 2;
		layer.extent = 4096;

		// Add keys
		layer.keys.push_back("always");
		layer.keys.push_back("sometimes");

		// Add values
		mvt_value v1, v2, v3;
		v1.type = mvt_string;
		v1.set_string_value("a");
		v2.type = mvt_string;
		v2.set_string_value("b");
		v3.type = mvt_string;
		v3.set_string_value("c");
		layer.values.push_back(v1);  // 0
		layer.values.push_back(v2);  // 1
		layer.values.push_back(v3);  // 2

		// Feature 1: has both properties
		mvt_feature f1;
		f1.type = mvt_point;
		f1.id = 1;
		f1.has_id = true;
		f1.tags = {0, 0, 1, 1};  // always=a, sometimes=b
		f1.geometry.push_back(mvt_geometry(mvt_moveto, 100, 100));
		layer.features.push_back(f1);

		// Feature 2: has only "always" property
		mvt_feature f2;
		f2.type = mvt_point;
		f2.id = 2;
		f2.has_id = true;
		f2.tags = {0, 2};  // always=c
		f2.geometry.push_back(mvt_geometry(mvt_moveto, 200, 200));
		layer.features.push_back(f2);

		tile.layers.push_back(layer);

		mlt::mlt_tile encoder;
		std::string result = encoder.encode(tile);

		REQUIRE(!result.empty());
		// Should contain present stream for nullable property
		REQUIRE(result.size() > 30);
	}

	SECTION("Layer with integer properties") {
		mvt_tile tile;
		mvt_layer layer;
		layer.name = "ints";
		layer.version = 2;
		layer.extent = 4096;

		layer.keys.push_back("count");

		mvt_value v1, v2;
		v1.type = mvt_sint;
		v1.numeric_value.sint_value = 42;
		v2.type = mvt_sint;
		v2.numeric_value.sint_value = -100;
		layer.values.push_back(v1);
		layer.values.push_back(v2);

		mvt_feature f1, f2;
		f1.type = mvt_point;
		f1.id = 1;
		f1.has_id = true;
		f1.tags = {0, 0};
		f1.geometry.push_back(mvt_geometry(mvt_moveto, 100, 100));

		f2.type = mvt_point;
		f2.id = 2;
		f2.has_id = true;
		f2.tags = {0, 1};
		f2.geometry.push_back(mvt_geometry(mvt_moveto, 200, 200));

		layer.features.push_back(f1);
		layer.features.push_back(f2);
		tile.layers.push_back(layer);

		mlt::mlt_tile encoder;
		std::string result = encoder.encode(tile);

		REQUIRE(!result.empty());
	}

	SECTION("Layer with float properties") {
		mvt_tile tile;
		mvt_layer layer;
		layer.name = "floats";
		layer.version = 2;
		layer.extent = 4096;

		layer.keys.push_back("value");

		mvt_value v1;
		v1.type = mvt_double;
		v1.numeric_value.double_value = 3.14159;
		layer.values.push_back(v1);

		mvt_feature f1;
		f1.type = mvt_point;
		f1.id = 1;
		f1.has_id = true;
		f1.tags = {0, 0};
		f1.geometry.push_back(mvt_geometry(mvt_moveto, 100, 100));

		layer.features.push_back(f1);
		tile.layers.push_back(layer);

		mlt::mlt_tile encoder;
		std::string result = encoder.encode(tile);

		REQUIRE(!result.empty());
	}

	SECTION("Layer with boolean properties") {
		mvt_tile tile;
		mvt_layer layer;
		layer.name = "bools";
		layer.version = 2;
		layer.extent = 4096;

		layer.keys.push_back("flag");

		mvt_value v1, v2;
		v1.type = mvt_bool;
		v1.numeric_value.bool_value = true;
		v2.type = mvt_bool;
		v2.numeric_value.bool_value = false;
		layer.values.push_back(v1);
		layer.values.push_back(v2);

		mvt_feature f1, f2;
		f1.type = mvt_point;
		f1.id = 1;
		f1.has_id = true;
		f1.tags = {0, 0};
		f1.geometry.push_back(mvt_geometry(mvt_moveto, 100, 100));

		f2.type = mvt_point;
		f2.id = 2;
		f2.has_id = true;
		f2.tags = {0, 1};
		f2.geometry.push_back(mvt_geometry(mvt_moveto, 200, 200));

		layer.features.push_back(f1);
		layer.features.push_back(f2);
		tile.layers.push_back(layer);

		mlt::mlt_tile encoder;
		std::string result = encoder.encode(tile);

		REQUIRE(!result.empty());
	}
}

TEST_CASE("MLT polygon encoding", "[mlt]") {
	SECTION("Simple polygon") {
		mvt_tile tile;
		mvt_layer layer;
		layer.name = "polys";
		layer.version = 2;
		layer.extent = 4096;

		mvt_feature poly;
		poly.type = mvt_polygon;
		poly.id = 1;
		poly.has_id = true;

		// Simple square polygon (exterior ring, clockwise)
		poly.geometry.push_back(mvt_geometry(mvt_moveto, 0, 0));
		poly.geometry.push_back(mvt_geometry(mvt_lineto, 100, 0));
		poly.geometry.push_back(mvt_geometry(mvt_lineto, 100, 100));
		poly.geometry.push_back(mvt_geometry(mvt_lineto, 0, 100));
		poly.geometry.push_back(mvt_geometry(mvt_closepath, 0, 0));

		layer.features.push_back(poly);
		tile.layers.push_back(layer);

		mlt::mlt_tile encoder;
		std::string result = encoder.encode(tile);

		REQUIRE(!result.empty());
		REQUIRE(result.size() > 20);
	}
}

TEST_CASE("MLT multi-geometry encoding", "[mlt]") {
	SECTION("MultiPoint") {
		mvt_tile tile;
		mvt_layer layer;
		layer.name = "multipoint";
		layer.version = 2;
		layer.extent = 4096;

		mvt_feature mp;
		mp.type = mvt_point;
		mp.id = 1;
		mp.has_id = true;

		// Multiple points
		mp.geometry.push_back(mvt_geometry(mvt_moveto, 100, 100));
		mp.geometry.push_back(mvt_geometry(mvt_moveto, 200, 200));
		mp.geometry.push_back(mvt_geometry(mvt_moveto, 300, 300));

		layer.features.push_back(mp);
		tile.layers.push_back(layer);

		mlt::mlt_tile encoder;
		std::string result = encoder.encode(tile);

		REQUIRE(!result.empty());
	}

	SECTION("MultiLineString") {
		mvt_tile tile;
		mvt_layer layer;
		layer.name = "multiline";
		layer.version = 2;
		layer.extent = 4096;

		mvt_feature ml;
		ml.type = mvt_linestring;
		ml.id = 1;
		ml.has_id = true;

		// Two line strings
		ml.geometry.push_back(mvt_geometry(mvt_moveto, 0, 0));
		ml.geometry.push_back(mvt_geometry(mvt_lineto, 100, 100));
		ml.geometry.push_back(mvt_geometry(mvt_moveto, 200, 0));
		ml.geometry.push_back(mvt_geometry(mvt_lineto, 300, 100));

		layer.features.push_back(ml);
		tile.layers.push_back(layer);

		mlt::mlt_tile encoder;
		std::string result = encoder.encode(tile);

		REQUIRE(!result.empty());
	}
}
