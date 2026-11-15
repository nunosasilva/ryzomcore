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
		fprintf(stderr, "Usage: %s <in.shape> [out.shape|-] [texture-substring]\n", argv[0]);
		return 1;
	}

	registerSerial3d();

	const std::string inName = argv[1];
	const std::string outName = (argc >= 3) ? argv[2] : "";
	const std::string substr = (argc >= 4) ? toLowerAscii(std::string(argv[3])) : std::string();
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
	for (uint m = 0; m < meshBase->getNbMaterial(); ++m)
	{
		const CMaterial &mat = meshBase->getMaterial(m);
		selected[m] = !substr.empty() && materialUsesTexture(mat, substr);
		printf("  material %u: doubleSided=%d blend=%d lighting=%d alphaTest=%d%s\n",
			m, (int)mat.getDoubleSided(), (int)mat.getBlend(), (int)mat.isLighted(),
			(int)mat.getAlphaTest(), selected[m] ? "  <-- selected (winding will be flipped)" : "");
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
		flipWindingInBytes(data, offset, patches[i].Pattern.size());
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
