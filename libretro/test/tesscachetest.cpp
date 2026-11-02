// The tessellation cache hands back exactly what tessellating would give: Bezier and spline
// patches drawn over several frames, through an engine that caches and one that can't (it doesn't
// flush per curve), with every input the tessellation reads changed in turn - a control point,
// the index buffer, the division, the primitive, the facing, lighting - and control points that
// change every frame, which must stop the lookups after a while.
#include <cstdio>
#include <cstring>
#include <vector>

#include "Core/Config.h"
#include "GPU/GPU.h"
#include "GPU/GPUState.h"
#include "GPU/ge_constants.h"
#include "GPU/Common/DrawEngineCommon.h"
#include "GPU/Common/SplineCommon.h"
#include "GPU/Common/TransformCommon.h"

struct Drawn {
	std::vector<u16> inds;
	std::vector<SimpleVertex> verts;
	bool operator==(const Drawn &o) const {
		return inds == o.inds && verts.size() == o.verts.size() &&
			memcmp(verts.data(), o.verts.data(), verts.size() * sizeof(SimpleVertex)) == 0;
	}
};

class CaptureEngine : public DrawEngineCommon {
public:
	explicit CaptureEngine(bool flushPerCurve) {
		flushOnParams_ = flushPerCurve;
	}
	void DeviceLost() override {}
	void DeviceRestore(Draw::DrawContext *) override {}
	void Flush() override {}
	void DispatchSubmitPrim(const void *verts, const void *inds, GEPrimitiveType prim, int vertexCount, u32 vertTypeID, bool clockwise, int *bytesRead, ClipInfoFlags) override {
		Drawn d;
		d.inds.assign((const u16 *)inds, (const u16 *)inds + vertexCount);
		u16 hi = 0;
		for (u16 i : d.inds)
			hi = i > hi ? i : hi;
		d.verts.assign((const SimpleVertex *)verts, (const SimpleVertex *)verts + hi + 1);
		last = d;
		*bytesRead = 0;
	}
	Drawn last;
};

struct InVertex {
	float u, v;
	u32 color;
	float x, y, z;
};
static const u32 VTYPE = GE_VTYPE_TC_FLOAT | GE_VTYPE_COL_8888 | GE_VTYPE_POS_FLOAT;

struct Curve {
	bool spline = false;
	int pointsU = 4, pointsV = 4;
	int tessU = 6, tessV = 5;
	int typeU = 0, typeV = 3;
	GEPatchPrimType prim = GE_PATCHPRIM_TRIANGLES;
	bool facing = false;
	bool lighting = false;
	bool indexed = false;
	std::vector<InVertex> points;
	std::vector<u8> indices;
};

static Curve MakeCurve(bool spline, int pu, int pv, float seed) {
	Curve c;
	c.spline = spline;
	c.pointsU = pu;
	c.pointsV = pv;
	for (int v = 0; v < pv; v++) {
		for (int u = 0; u < pu; u++) {
			InVertex p;
			p.u = u * 0.25f;
			p.v = v * 0.25f;
			p.color = 0xFF000000u | (u * 40) << 16 | (v * 50) << 8 | (u ^ v) * 30;
			p.x = u * 10.0f + seed;
			p.y = v * 7.0f - seed * 0.5f;
			p.z = (float)((u * 3 + v * 5) % 7) + seed * 0.25f;
			c.points.push_back(p);
		}
	}
	return c;
}

static int failures;

static Drawn DrawCurve(CaptureEngine &engine, const Curve &c) {
	gstate.lightingEnable = (GE_CMD_LIGHTINGENABLE << 24) | (c.lighting ? 1 : 0);
	u32 vtype = VTYPE | (c.indexed ? GE_VTYPE_IDX_8BIT : 0);
	int bytesRead = 0;
	engine.last = Drawn();
	// The decoder loads positions sixteen bytes at a time, as guest memory always allows.
	std::vector<u8> mem(c.points.size() * sizeof(InVertex) + 64);
	memcpy(mem.data(), c.points.data(), c.points.size() * sizeof(InVertex));
	const void *points = mem.data();
	if (c.spline) {
		Spline::SplineSurface s{};
		s.tess_u = c.tessU;
		s.tess_v = c.tessV;
		s.type_u = c.typeU;
		s.type_v = c.typeV;
		s.num_points_u = c.pointsU;
		s.num_points_v = c.pointsV;
		s.num_patches_u = c.pointsU - 3;
		s.num_patches_v = c.pointsV - 3;
		s.primType = c.prim;
		s.patchFacing = c.facing;
		engine.SubmitCurve(points, c.indexed ? c.indices.data() : nullptr, s, vtype, &bytesRead, "spline");
	} else {
		Spline::BezierSurface s{};
		s.tess_u = c.tessU;
		s.tess_v = c.tessV;
		s.num_points_u = c.pointsU;
		s.num_points_v = c.pointsV;
		s.num_patches_u = (c.pointsU - 1) / 3;
		s.num_patches_v = (c.pointsV - 1) / 3;
		s.primType = c.prim;
		s.patchFacing = c.facing;
		engine.SubmitCurve(points, c.indexed ? c.indices.data() : nullptr, s, vtype, &bytesRead, "bezier");
	}
	return engine.last;
}

// Draws the curve on both engines, checks they agree, and returns how many draws were cached.
static int Check(CaptureEngine &cached, CaptureEngine &plain, const Curve &c, const char *what) {
	const int before = gpuStats.perFrame.numCachedCurves;
	const Drawn a = DrawCurve(cached, c);
	const int hits = gpuStats.perFrame.numCachedCurves - before;
	const Drawn b = DrawCurve(plain, c);
	if (b.inds.empty() || !(a == b)) {
		if (failures++ < 8)
			printf("FAIL %s (frame %d): %zu/%zu indices, %zu/%zu vertices%s\n", what, gpuStats.totals.numFlips,
				a.inds.size(), b.inds.size(), a.verts.size(), b.verts.size(), a.verts.size() == b.verts.size() ? ", contents differ" : "");
	}
	return hits;
}

static void NextFrame() {
	gpuStats.totals.numFlips++;
}

int main() {
	g_Config.iSplineBezierQuality = 2;
	g_Config.bVertexDecoderJit = false;
	gpuStats.Reset();
	gstate.vertType = 0;
	CaptureEngine cached(true), plain(false);

	struct Case {
		const char *name;
		Curve curve;
	};
	std::vector<Case> cases;
	cases.push_back({ "bezier 4x4", MakeCurve(false, 4, 4, 0.0f) });
	cases.push_back({ "bezier 7x4", MakeCurve(false, 7, 4, 1.0f) });
	cases.push_back({ "spline 5x6", MakeCurve(true, 5, 6, 2.0f) });

	// Each case: nothing cached in the first two frames, every draw cached after.
	for (Case &cs : cases) {
		int hits = 0;
		for (int frame = 0; frame < 4; frame++) {
			hits += Check(cached, plain, cs.curve, cs.name);
			NextFrame();
		}
		if (hits != 2) {
			printf("FAIL %s: %d of the four draws cached, want the last two\n", cs.name, hits);
			failures++;
		}
	}

	// Change one input at a time: what's drawn must follow it, never an entry for the old curve.
	for (Case &cs : cases) {
		std::vector<Curve> variants;
		Curve c = cs.curve;
		c.points[5].z += 0.001f;
		variants.push_back(c);
		c = cs.curve;
		c.points[0].u += 0.5f;
		variants.push_back(c);
		c = cs.curve;
		c.points[3].color ^= 0x00010000;
		variants.push_back(c);
		c = cs.curve;
		c.tessU++;
		variants.push_back(c);
		c = cs.curve;
		c.tessV = 2;
		variants.push_back(c);
		c = cs.curve;
		c.prim = GE_PATCHPRIM_LINES;
		variants.push_back(c);
		c = cs.curve;
		c.facing = true;
		variants.push_back(c);
		c = cs.curve;
		c.lighting = true;
		variants.push_back(c);
		// Facing only flips normals, which only lighting asks for.
		c.facing = true;
		variants.push_back(c);
		if (cs.curve.spline) {
			c = cs.curve;
			c.typeU = 1;
			variants.push_back(c);
		}
		// The same points through an index buffer, the first two swapped.
		c = cs.curve;
		c.indexed = true;
		for (int i = 0; i < (int)c.points.size(); i++)
			c.indices.push_back((u8)i);
		std::swap(c.points[0], c.points[1]);
		std::swap(c.indices[0], c.indices[1]);
		variants.push_back(c);
		for (int frame = 0; frame < 4; frame++) {
			for (const Curve &v : variants)
				Check(cached, plain, v, cs.name);
			Check(cached, plain, cs.curve, cs.name);
			NextFrame();
		}
	}

	// Control points that move every frame are never found, and after a while not looked for.
	Curve moving = MakeCurve(false, 4, 4, 0.0f);
	for (int frame = 0; frame < 300; frame++) {
		moving.points[6].y = frame * 0.125f;
		for (int i = 0; i < 20; i++) {
			moving.points[9].x = i * 0.5f;
			if (Check(cached, plain, moving, "moving") != 0) {
				printf("FAIL moving: a draw with new control points was cached\n");
				failures++;
			}
		}
		NextFrame();
	}
	// The cache now stands aside, so even a still curve isn't kept at first - until it tries again.
	int earlyHits = 0, stillHits = 0;
	for (int frame = 0; frame < 300; frame++) {
		const int hits = Check(cached, plain, cases[0].curve, "still");
		if (frame < 8)
			earlyHits += hits;
		stillHits += hits;
		NextFrame();
	}
	if (earlyHits != 0) {
		printf("FAIL moving: still looking curves up right after a long run of misses\n");
		failures++;
	}
	if (stillHits == 0) {
		printf("FAIL still: never cached once the moving curves stopped\n");
		failures++;
	}

	if (failures) {
		printf("tesscachetest: %d failed\n", failures);
		return 1;
	}
	printf("tesscachetest: cached curves match tessellation, inputs that change miss, moving curves bypass\n");
	return 0;
}
