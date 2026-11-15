// shape_flip - inspect a .shape and flip the winding of one sub-mesh in place.
//
// Loads a NeL .shape (CShapeStream) to find the render passes whose material
// uses a given texture, then flips the vertex order of every triangle of those
// passes directly in the file bytes. The rest of the file is left untouched and
// the size never changes, so the result is the shipped asset with nothing
// modified but the winding of the selected passes.
//
// Usage:
//   shape_flip <in.shape>                       dump the shape structure
//   shape_flip <in.shape> - <texture-substr>    dry run (report, no output)
//   shape_flip <in.shape> <out.shape> <substr>  patch and write <out.shape>
//   shape_flip <in.shape> dump                  write blkNN.bin per selected pass
//
// The texture substring is matched case-insensitively against the file names of
// the material textures (e.g. "g_tower2" selects the material that uses
// G_tower2.tga).
//
// Notes on the file format: CIndexBuffer::serial() writes the indices as
// uint32 regardless of the runtime format (see nel/src/3d/index_buffer.cpp),
// preceded by a version byte, _NbIndexes, _Capacity and the vector count. The
// patcher locates each block by its exact index data and checks that prefix, so
// it does not need to parse the whole shape format.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "nel/misc/types_nl.h"
#include "nel/misc/file.h"
#include "nel/misc/path.h"
#include "nel/misc/vector.h"
#include "nel/misc/common.h"

#include <algorithm>
#include <cstdlib>

#include "nel/3d/shape.h"
#include "nel/3d/register_3d.h"
#include "nel/3d/mesh.h"
#include "nel/3d/mesh_mrm.h"
#include "nel/3d/material.h"
#include "nel/3d/texture.h"
#include "nel/3d/texture_file.h"
#include "nel/3d/index_buffer.h"

using namespace NLMISC;
using namespace NL3D;

typedef std::vector<uint8> TBytes;

static void printTextures(const CMaterial &mat)
{
	for (uint l = 0; l < 8; ++l)
	{
		ITexture *lm = mat.getLightMap(l);
		if (lm == NULL && l > 0)
			break;
		if (lm == NULL)
			continue;
		CTextureFile *ltf = dynamic_cast<CTextureFile *>(lm);
		printf("      lightmap%u: %s\n", l, ltf ? CFile::getFilename(ltf->getFileName()).c_str() : "<non-file>");
	}
	for (uint32 i = 0; i < IDRV_MAT_MAXTEXTURES; ++i)
	{
		ITexture *tex = mat.getTexture(i);
		if (tex == NULL)
			continue;
		CTextureFile *tf = dynamic_cast<CTextureFile *>(tex);
		if (tf != NULL)
			printf("      tex%u: %s\n", i, CFile::getFilename(tf->getFileName()).c_str());
		else
			printf("      tex%u: <non-file texture>\n", i);
	}
}

// True if the material references a texture file whose name contains 'substr'.
static bool materialUsesTexture(const CMaterial &mat, const std::string &substr)
{
	for (uint32 i = 0; i < IDRV_MAT_MAXTEXTURES; ++i)
	{
		ITexture *tex = mat.getTexture(i);
		if (tex == NULL)
			continue;
		CTextureFile *tf = dynamic_cast<CTextureFile *>(tex);
		if (tf == NULL)
			continue;
		if (toLowerAscii(CFile::getFilename(tf->getFileName())).find(substr) != std::string::npos)
			return true;
	}
	return false;
}

static void appendU32(TBytes &out, uint32 v)
{
	out.push_back((uint8)(v & 0xFF));
	out.push_back((uint8)((v >> 8) & 0xFF));
	out.push_back((uint8)((v >> 16) & 0xFF));
	out.push_back((uint8)((v >> 24) & 0xFF));
}

static uint32 readU32(const TBytes &buf, size_t pos)
{
	return (uint32)buf[pos] | ((uint32)buf[pos + 1] << 8) | ((uint32)buf[pos + 2] << 16) | ((uint32)buf[pos + 3] << 24);
}

// Serialize the index buffer the way CIndexBuffer::serial() stores it (uint32).
static void buildIndexPattern(const CIndexBuffer &ib, TBytes &pattern)
{
	pattern.clear();
	pattern.reserve(ib.getNumIndexes() * 4);
	CIndexBufferRead ibr;
	ib.lock(ibr);
	if (ibr.getFormat() == CIndexBuffer::Indices16)
	{
		const uint16 *p = (const uint16 *)ibr.getPtr();
		for (uint i = 0; i < ib.getNumIndexes(); ++i)
			appendU32(pattern, p[i]);
	}
	else
	{
		const uint32 *p = (const uint32 *)ibr.getPtr();
		for (uint i = 0; i < ib.getNumIndexes(); ++i)
			appendU32(pattern, p[i]);
	}
}

// Swap the 2nd and 3rd index (uint32 in file order) of each triangle.
// Replace every triangle by a degenerate one (i0,i0,i0): the faces are never
// rasterised, whatever the winding convention of the driver is.
static void degenerateInBytes(TBytes &data, size_t offset, size_t numBytes)
{
	for (size_t t = 0; t < numBytes / 12; ++t)
	{
		memcpy(&data[offset + t * 12 + 4], &data[offset + t * 12], 4);
		memcpy(&data[offset + t * 12 + 8], &data[offset + t * 12], 4);
	}
}

static void flipWindingInBytes(TBytes &data, size_t offset, size_t numBytes)
{
	for (size_t t = 0; t < numBytes / 12; ++t)
	{
		uint8 tmp[4];
		memcpy(tmp, &data[offset + t * 12 + 4], 4);
		memcpy(&data[offset + t * 12 + 4], &data[offset + t * 12 + 8], 4);
		memcpy(&data[offset + t * 12 + 8], tmp, 4);
	}
}

struct CBlockPatch
{
	TBytes Pattern;
	uint NumIndexes;
};

// Find the block in 'data' at or after 'searchFrom' and check the index buffer
// prefix that precedes it (version byte, _NbIndexes, _Capacity, vector count).
static bool findBlock(const TBytes &data, size_t searchFrom, const CBlockPatch &block, size_t &found)
{
	const size_t len = block.Pattern.size();
	for (size_t pos = searchFrom; pos + len <= data.size(); ++pos)
	{
		if (data[pos] != block.Pattern[0])
			continue;
		if (memcmp(&data[pos], &block.Pattern[0], len) != 0)
			continue;
		// prefix: [ver:1][_NbIndexes:4][_Capacity:4][count:4]
		if (pos < 13)
			continue;
		if (data[pos - 13] < 1 || data[pos - 13] > 3)
			continue;
		if (readU32(data, pos - 12) != block.NumIndexes)
			continue;
		if (readU32(data, pos - 8) < block.NumIndexes)
			continue;
		if (readU32(data, pos - 4) != block.NumIndexes)
			continue;
		found = pos;
		return true;
	}
	return false;
}

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "Usage: %s <in.shape> [out.shape|-|dump] [texture-substring] [--degenerate]\n", argv[0]);
		return 1;
	}

	registerSerial3d();

	const std::string inName = argv[1];
	std::string outName;
	std::string substr;
	// --degenerate: collapse every triangle of the selected passes instead of
	// swapping its 2nd/3rd index (removes the faces whatever the driver's
	// winding convention is).
	bool degenerate = false;
	std::string matList;
	for (int i = 2; i < argc; ++i)
	{
		std::string arg = argv[i];
		if (arg == "--degenerate")
			degenerate = true;
		else if (arg.compare(0, 6, "--mat=") == 0)
			matList = arg.substr(6);
		else if (outName.empty())
			outName = arg;
		else if (substr.empty())
			substr = toLowerAscii(arg);
	}
	const bool writeOutput = !outName.empty() && outName != "-";

	CIFile inf;
	if (!inf.open(inName))
	{
		fprintf(stderr, "ERROR: cannot open %s\n", inName.c_str());
		return 1;
	}

	CShapeStream shapeStream;
	try
	{
		inf.serial(shapeStream);
	}
	catch (const std::exception &e)
	{
		fprintf(stderr, "ERROR: cannot read %s: %s\n", inName.c_str(), e.what());
		return 1;
	}
	inf.close();

	IShape *shape = shapeStream.getShapePointer();
	CMeshBase *meshBase = dynamic_cast<CMeshBase *>(shape);
	if (meshBase == NULL)
	{
		fprintf(stderr, "ERROR: shape is not a CMeshBase\n");
		return 1;
	}

	printf("shape: %s\n", typeid(*shape).name());
	printf("materials: %u\n", meshBase->getNbMaterial());
	std::vector<bool> selected(meshBase->getNbMaterial(), false);
	if (!matList.empty())
	{
		// --mat=1,4,7 or --mat=3-9: select by material index
		std::string cur;
		for (size_t i = 0; i <= matList.size(); ++i)
		{
			char c = (i < matList.size()) ? matList[i] : ',';
			if (c != ',')
			{
				cur += c;
				continue;
			}
			if (cur.empty())
				continue;
			size_t dash = cur.find('-');
			if (dash == std::string::npos)
			{
				uint v = (uint)atoi(cur.c_str());
				if (v < selected.size())
					selected[v] = true;
			}
			else
			{
				uint a = (uint)atoi(cur.substr(0, dash).c_str());
				uint b = (uint)atoi(cur.substr(dash + 1).c_str());
				for (uint v = a; v <= b && v < selected.size(); ++v)
					selected[v] = true;
			}
			cur.clear();
		}
	}
	for (uint m = 0; m < meshBase->getNbMaterial(); ++m)
	{
		const CMaterial &mat = meshBase->getMaterial(m);
		if (matList.empty())
			selected[m] = !substr.empty() && materialUsesTexture(mat, substr);
		printf("  material %u: doubleSided=%d blend=%d lighting=%d alphaTest=%d shader=%d texgen0=%d flags=0x%x%s\n",
			m, (int)mat.getDoubleSided(), (int)mat.getBlend(), (int)mat.isLighted(),
			(int)mat.getAlphaTest(), (int)mat.getShader(), (int)mat.getTexCoordGen(0),
			mat.getFlags(), selected[m] ? "  <-- selected (winding will be flipped)" : "");
		printTextures(mat);
	}

	// Collect the selected index blocks, in file order.
	std::vector<CBlockPatch> patches;
	uint selectedTris = 0;

	CMesh *mesh = dynamic_cast<CMesh *>(shape);
	CMeshMRM *meshMRM = dynamic_cast<CMeshMRM *>(shape);

	if (meshMRM != NULL)
	{
		// The accessors are const only; reading does not modify anything.
		const CMeshMRMGeom &geom = meshMRM->getMeshGeom();
		printf("MRM: %u lods\n", geom.getNbLod());
		for (uint lod = 0; lod < geom.getNbLod(); ++lod)
		{
			printf("  lod %u: %u rdrpasses\n", lod, geom.getNbRdrPass(lod));
			for (uint rp = 0; rp < geom.getNbRdrPass(lod); ++rp)
			{
				uint32 matId = geom.getRdrPassMaterial(lod, rp);
				const CIndexBuffer &ib = geom.getRdrPassPrimitiveBlock(lod, rp);
				bool sel = matId < selected.size() && selected[matId];
				printf("    pass %u: material %u, %u triangles%s\n", rp, matId, ib.getNumIndexes() / 3, sel ? "  <-- flip" : "");
				if (!sel)
					continue;
				CBlockPatch patch;
				patch.NumIndexes = ib.getNumIndexes();
				buildIndexPattern(ib, patch.Pattern);
				patches.push_back(patch);
				selectedTris += ib.getNumIndexes() / 3;
			}
			// NB: the geomorphs of a lod are only vertex ranges (CMRMWedgeGeom
			// Start/End), they hold no index data, so there is nothing to flip.
		}
	}
	else if (mesh != NULL)
	{
		printf("MESH: %u matrix blocks\n", mesh->getNbMatrixBlock());
		for (uint mb = 0; mb < mesh->getNbMatrixBlock(); ++mb)
		{
			for (uint rp = 0; rp < mesh->getNbRdrPass(mb); ++rp)
			{
				uint32 matId = mesh->getRdrPassMaterial(mb, rp);
				const CIndexBuffer &ib = mesh->getRdrPassPrimitiveBlock(mb, rp);
				bool sel = matId < selected.size() && selected[matId];
				printf("  block %u pass %u: material %u, %u triangles%s\n", mb, rp, matId, ib.getNumIndexes() / 3, sel ? "  <-- flip" : "");
				if (!sel)
					continue;
				CBlockPatch patch;
				patch.NumIndexes = ib.getNumIndexes();
				buildIndexPattern(ib, patch.Pattern);
				patches.push_back(patch);
				selectedTris += ib.getNumIndexes() / 3;
			}
		}
	}
	else
	{
		fprintf(stderr, "ERROR: unsupported shape type\n");
		return 1;
	}

	printf("selected triangles: %u in %u blocks\n", selectedTris, (uint)patches.size());

	if (outName == "notwosided")
	{
		// Diagnostic: clear IDRV_MAT_DOUBLE_SIDED on every material and write a
		// full re-serialised copy (notwosided.shape). Used to test whether the
		// Vulkan texture mix-up only affects two-sided materials.
		for (uint m = 0; m < meshBase->getNbMaterial(); ++m)
			meshBase->getMaterial(m).setDoubleSided(false);
		std::string out = "notwosided.shape";
		COFile f;
		if (!f.open(out))
			return 1;
		CShapeStream st(shape);
		f.serial(st);
		f.close();
		printf("written: %s\n", out.c_str());
		return 0;
	}

	if (outName == "stats")
	{
		// Per-material geometry statistics: triangle count, bounding box and
		// largest triangle edge, used to spot huge stretched panels.
		CMesh *m = dynamic_cast<CMesh *>(shape);
		if (m == NULL)
		{
			fprintf(stderr, "ERROR: stats mode needs a plain mesh (CMesh)\n");
			return 1;
		}
		CVertexBuffer &vb = const_cast<CVertexBuffer &>(m->getVertexBuffer());
		CVertexBufferReadWrite vba;
		vb.lock(vba);
		for (uint mb = 0; mb < m->getNbMatrixBlock(); ++mb)
		{
			for (uint rp = 0; rp < m->getNbRdrPass(mb); ++rp)
			{
				uint32 matId = m->getRdrPassMaterial(mb, rp);
				const CIndexBuffer &ib = m->getRdrPassPrimitiveBlock(mb, rp);
				CIndexBufferRead ibr;
				ib.lock(ibr);
				uint ntri = ib.getNumIndexes() / 3;
				CVector bmin(1e30f, 1e30f, 1e30f), bmax(-1e30f, -1e30f, -1e30f);
				float maxEdge = 0.f;
				double area = 0.0;
				for (uint t = 0; t < ntri; ++t)
				{
					uint32 i0, i1, i2;
					if (ibr.getFormat() == CIndexBuffer::Indices16)
					{
						const uint16 *p = (const uint16 *)ibr.getPtr();
						i0 = p[t * 3]; i1 = p[t * 3 + 1]; i2 = p[t * 3 + 2];
					}
					else
					{
						const uint32 *p = (const uint32 *)ibr.getPtr();
						i0 = p[t * 3]; i1 = p[t * 3 + 1]; i2 = p[t * 3 + 2];
					}
					CVector v0 = *vba.getVertexCoordPointer(i0);
					CVector v1 = *vba.getVertexCoordPointer(i1);
					CVector v2 = *vba.getVertexCoordPointer(i2);
					const CVector *vv[3] = { &v0, &v1, &v2 };
					for (int k = 0; k < 3; ++k)
					{
						bmin.x = std::min(bmin.x, vv[k]->x); bmin.y = std::min(bmin.y, vv[k]->y); bmin.z = std::min(bmin.z, vv[k]->z);
						bmax.x = std::max(bmax.x, vv[k]->x); bmax.y = std::max(bmax.y, vv[k]->y); bmax.z = std::max(bmax.z, vv[k]->z);
					}
					maxEdge = std::max(maxEdge, (v1 - v0).norm());
					maxEdge = std::max(maxEdge, (v2 - v0).norm());
					maxEdge = std::max(maxEdge, (v2 - v1).norm());
					area += 0.5 * ((v1 - v0) ^ (v2 - v0)).norm();
				}
				printf("mat %3u tris %5u  bbox (%.2f %.2f %.2f)-(%.2f %.2f %.2f) size (%.2f %.2f %.2f) maxedge %.2f area %.1f\n",
					matId, ntri,
					(double)bmin.x, (double)bmin.y, (double)bmin.z,
					(double)bmax.x, (double)bmax.y, (double)bmax.z,
					(double)(bmax.x - bmin.x), (double)(bmax.y - bmin.y), (double)(bmax.z - bmin.z),
					(double)maxEdge, area);
			}
		}
		return 0;
	}

	if (outName == "dump")
	{
		// Debug helper: write the serialized form of each selected block.
		for (uint i = 0; i < patches.size(); ++i)
		{
			std::string name = toString("blk%02u.bin", i);
			COFile f;
			if (!f.open(name))
				return 1;
			f.serialBuffer(&patches[i].Pattern[0], (uint)patches[i].Pattern.size());
			f.close();
			printf("  dumped %s (%u bytes)\n", name.c_str(), (uint)patches[i].Pattern.size());
		}
		return 0;
	}

	if (!writeOutput)
		return 0;

	// Read the file as bytes, patch the blocks in place.
	TBytes data;
	{
		CIFile f;
		if (!f.open(inName))
		{
			fprintf(stderr, "ERROR: cannot open %s\n", inName.c_str());
			return 1;
		}
		data.resize(f.getFileSize());
		if (!data.empty())
			f.serialBuffer(&data[0], (uint)data.size());
		f.close();
	}

	size_t cursor = 0;
	for (uint i = 0; i < patches.size(); ++i)
	{
		size_t offset = 0;
		if (!findBlock(data, cursor, patches[i], offset))
		{
			fprintf(stderr, "ERROR: index block %u (%u indices, %u bytes) not found after offset %u of %u\n", i, patches[i].NumIndexes, (uint)patches[i].Pattern.size(), (uint)cursor, (uint)data.size());
			return 1;
		}
		if (degenerate)
		{
			degenerateInBytes(data, offset, patches[i].Pattern.size());
		}
		else
		{
			flipWindingInBytes(data, offset, patches[i].Pattern.size());
		}
		printf("  patched block %u: %u triangles at offset %u\n", i, patches[i].NumIndexes / 3, (uint)offset);
		cursor = offset + patches[i].Pattern.size();
	}

	COFile outf;
	if (!outf.open(outName))
	{
		fprintf(stderr, "ERROR: cannot create %s\n", outName.c_str());
		return 1;
	}
	if (!data.empty())
		outf.serialBuffer(&data[0], (uint)data.size());
	outf.close();
	printf("written: %s (%u bytes, same size as input)\n", outName.c_str(), (uint)data.size());

	return 0;
}
