// NeL - MMORPG Framework <https://wiki.ryzom.dev/>
// Copyright (C) 2026  Jan BOON (Kaetemi) <jan.boon@kaetemi.be>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

// KTX1 container support (CBitmap::readKTX). The files are built in memory from the
// specification, so the test needs no binary asset and covers the two things that matter
// for a container format: the pixel data has to come out exactly as it went in (including
// the 4-byte row alignment KTX mandates for uncompressed images), and a hostile or broken
// file has to be refused instead of allocating on its say-so.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include <nel/misc/bitmap.h>
#include <nel/misc/common.h>
#include <nel/misc/mem_stream.h>

using NLMISC::CBitmap;
using NLMISC::CMemStream;

namespace {

typedef std::vector<uint8> TBytes;

// GL enums, spelled out so the test does not depend on a GL header.
const uint32 GL_UNSIGNED_BYTE__ = 0x1401;
const uint32 GL_RGB__ = 0x1907;
const uint32 GL_RGBA__ = 0x1908;
const uint32 GL_LUMINANCE__ = 0x1909;
const uint32 GL_RGBA8__ = 0x8058;
const uint32 GL_COMPRESSED_RGBA_S3TC_DXT5_EXT__ = 0x83F3;

void put32(TBytes &out, uint32 v)
{
	out.push_back((uint8)(v & 0xff));
	out.push_back((uint8)((v >> 8) & 0xff));
	out.push_back((uint8)((v >> 16) & 0xff));
	out.push_back((uint8)((v >> 24) & 0xff));
}

// A KTX1 file: 12-byte identifier, 13 header uint32s, then one imageSize + the level data
// padded to 4 bytes. 'pixels' must already be in the layout the reader expects
// (rowStride * height, with rowStride = align4(width * bytesPerPixel) for uncompressed).
TBytes buildKTX(uint32 width, uint32 height,
	uint32 glType, uint32 glTypeSize, uint32 glFormat, uint32 glInternalFormat,
	uint32 glBaseInternalFormat, const TBytes &pixels,
	uint32 faces = 1, uint32 depth = 0, uint32 mipLevels = 1,
	bool corruptIdentifier = false)
{
	TBytes f;
	// Identifier: 0xAB 'K' 'T' 'X' ' ' '1' '1' 0xBB '\r' '\n' 0x1A '\n'
	const uint8 id[12] = { 0xAB, 'K', 'T', 'X', ' ', '1', '1', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A };
	f.insert(f.end(), id, id + 12);
	if (corruptIdentifier)
		f[1] = 'Z';

	put32(f, 0x04030201);   // endianness: little endian
	put32(f, glType);
	put32(f, glTypeSize);
	put32(f, glFormat);
	put32(f, glInternalFormat);
	put32(f, glBaseInternalFormat);
	put32(f, width);
	put32(f, height);
	put32(f, depth);
	put32(f, 0);            // numberOfArrayElements
	put32(f, faces);
	put32(f, mipLevels);
	put32(f, 0);            // bytesOfKeyValueData

	put32(f, (uint32)pixels.size());
	f.insert(f.end(), pixels.begin(), pixels.end());
	while (f.size() % 4 != 0)
		f.push_back(0);
	return f;
}

uint8 levelDepth(const TBytes &ktx)
{
	CMemStream stream(true);
	stream.fill(&ktx[0], (uint32)ktx.size());
	CBitmap bitmap;
	return bitmap.load(stream);
}

} // namespace

// A plain RGBA8 2x2 image: the bytes must round-trip untouched.
TEST(CBitmapKTX, LoadRgba8PixelsRoundTrip)
{
	TBytes pixels;
	for (uint32 i = 0; i < 2 * 2 * 4; ++i)
		pixels.push_back((uint8)(0x10 + i)); // 0x10..0x1f

	const TBytes ktx = buildKTX(2, 2, GL_UNSIGNED_BYTE__, 1, GL_RGBA__, GL_RGBA8__, GL_RGBA__, pixels);

	CMemStream stream(true);
	stream.fill(const_cast<uint8 *>(&ktx[0]), (uint32)ktx.size());

	CBitmap bitmap;
	const uint8 depth = bitmap.load(stream);

	ASSERT_EQ(32, (int)depth);
	ASSERT_EQ(2, (int)bitmap.getWidth());
	ASSERT_EQ(2, (int)bitmap.getHeight());
	ASSERT_EQ(CBitmap::RGBA, (int)bitmap.getPixelFormat());
	// CBitmap::getSize() counts pixels; 2x2 RGBA therefore holds 16 bytes.
	ASSERT_EQ(4u, bitmap.getSize(0));
	ASSERT_EQ(pixels.size(), (size_t)bitmap.getSize(0) * 4);
	EXPECT_EQ(0, memcmp(bitmap.getPixels(0).getPtr(), &pixels[0], pixels.size()));
	ASSERT_EQ(1, (int)bitmap.getMipMapCount());
}

// getSizeFromBitmap (loadSize) must answer from the header alone, for the same files.
TEST(CBitmapKTX, LoadSizeFromHeader)
{
	TBytes pixels(3 * 2 * 4, 0x42); // 3x2 RGBA, rowStride == rowBytes == 12
	const TBytes ktx = buildKTX(3, 2, GL_UNSIGNED_BYTE__, 1, GL_RGBA__, GL_RGBA8__, GL_RGBA__, pixels);

	CMemStream stream(true);
	stream.fill(const_cast<uint8 *>(&ktx[0]), (uint32)ktx.size());

	uint32 w = 0, h = 0;
	CBitmap::loadSize(stream, w, h);
	EXPECT_EQ(3u, w);
	EXPECT_EQ(2u, h);
}

// RGB data is widened to RGBA, and the mandatory 4-byte row padding is stripped on the way
// in: a 3x2 RGB image has 9 bytes per row but 12 in the file.
TEST(CBitmapKTX, RgbRowsAreUnpaddedAndWidened)
{
	const uint32 w = 3, h = 2;
	const uint32 rowBytes = w * 3;              // 9
	const uint32 rowStride = (rowBytes + 3) & ~3u; // 12
	TBytes pixels(rowStride * h, 0);
	for (uint32 y = 0; y < h; ++y)
		for (uint32 x = 0; x < w; ++x)
		{
			const uint8 *src = &pixels[0] + y * rowStride + x * 3;
			EXPECT_EQ(0, src[0]); // padding region untouched below
		}
	// Fill with a recognisable pattern: R = x + 10y, G = 100 + x, B = 200 + y, padding = 0xEE
	for (uint32 y = 0; y < h; ++y)
	{
		for (uint32 i = 0; i < rowStride; ++i)
			pixels[y * rowStride + i] = 0xEE;
		for (uint32 x = 0; x < w; ++x)
		{
			pixels[y * rowStride + x * 3 + 0] = (uint8)(x + 10 * y);
			pixels[y * rowStride + x * 3 + 1] = (uint8)(100 + x);
			pixels[y * rowStride + x * 3 + 2] = (uint8)(200 + y);
		}
	}

	const TBytes ktx = buildKTX(w, h, GL_UNSIGNED_BYTE__, 1, GL_RGB__, GL_RGBA8__, GL_RGB__, pixels);

	CMemStream stream(true);
	stream.fill(const_cast<uint8 *>(&ktx[0]), (uint32)ktx.size());

	CBitmap bitmap;
	ASSERT_NE(0, (int)bitmap.load(stream));
	ASSERT_EQ(CBitmap::RGBA, (int)bitmap.getPixelFormat());
	const uint8 *out = bitmap.getPixels(0).getPtr();
	ASSERT_NE((const uint8 *)NULL, out);
	for (uint32 y = 0; y < h; ++y)
		for (uint32 x = 0; x < w; ++x)
		{
			const uint32 o = (y * w + x) * 4;
			EXPECT_EQ((int)(x + 10 * y), (int)out[o + 0]) << "R at " << x << "," << y;
			EXPECT_EQ((int)(100 + x), (int)out[o + 1]) << "G at " << x << "," << y;
			EXPECT_EQ((int)(200 + y), (int)out[o + 2]) << "B at " << x << "," << y;
			EXPECT_EQ(255, (int)out[o + 3]) << "A at " << x << "," << y;
		}
}

// DXTC5 blocks are stored verbatim: the reader must not try to expand them.
TEST(CBitmapKTX, CompressedBlocksPassThrough)
{
	TBytes blocks(16 * 2, 0x5A); // two 16-byte DXT5 blocks == a 8x4 image
	const TBytes ktx = buildKTX(8, 4, 0, 0, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT__,
		GL_COMPRESSED_RGBA_S3TC_DXT5_EXT__, blocks);

	CMemStream stream(true);
	stream.fill(const_cast<uint8 *>(&ktx[0]), (uint32)ktx.size());

	CBitmap bitmap;
	ASSERT_NE(0, (int)bitmap.load(stream));
	EXPECT_EQ(CBitmap::DXTC5, (int)bitmap.getPixelFormat());
	// 8x4 pixels, but the payload is 2 blocks of 16 bytes = 32 bytes (not 8*4*4).
	ASSERT_EQ(32u, bitmap.getSize(0));
	EXPECT_EQ(0, memcmp(bitmap.getPixels(0).getPtr(), &blocks[0], blocks.size()));
}

// A broken identifier is not a KTX at all.
TEST(CBitmapKTX, RejectCorruptIdentifier)
{
	TBytes pixels(2 * 2 * 4, 0);
	const TBytes ktx = buildKTX(2, 2, GL_UNSIGNED_BYTE__, 1, GL_RGBA__, GL_RGBA8__, GL_RGBA__, pixels,
		1, 0, 1, /*corruptIdentifier=*/true);
	EXPECT_EQ(0, (int)levelDepth(ktx));
}

// The reader supports 2D faces only, so cubemaps and 3D textures are refused rather than
// silently read as one face/one slice.
TEST(CBitmapKTX, RejectCubemapAnd3D)
{
	TBytes pixels(2 * 2 * 4, 0);
	EXPECT_EQ(0, (int)levelDepth(buildKTX(2, 2, GL_UNSIGNED_BYTE__, 1, GL_RGBA__, GL_RGBA8__, GL_RGBA__, pixels, 6)));
	EXPECT_EQ(0, (int)levelDepth(buildKTX(2, 2, GL_UNSIGNED_BYTE__, 1, GL_RGBA__, GL_RGBA8__, GL_RGBA__, pixels, 1, 2)));
	EXPECT_EQ(0, (int)levelDepth(buildKTX(0, 2, GL_UNSIGNED_BYTE__, 1, GL_RGBA__, GL_RGBA8__, GL_RGBA__, pixels)));
}

// A level whose declared size cannot hold the pixels must be refused, not read past.
TEST(CBitmapKTX, RejectUndersizedLevel)
{
	TBytes pixels(4, 0); // too small for 2x2 RGBA (needs 16)
	TBytes ktx = buildKTX(2, 2, GL_UNSIGNED_BYTE__, 1, GL_RGBA__, GL_RGBA8__, GL_RGBA__, pixels);
	EXPECT_EQ(0, (int)levelDepth(ktx));

	// Same thing, truncated mid-data: the declared size is right but the stream ends early.
	// load() is allowed to throw EStreamOverflow there; the path that must never throw is
	// loadUntrusted(), which is what a download goes through.
	pixels.assign(16, 0x11);
	ktx = buildKTX(2, 2, GL_UNSIGNED_BYTE__, 1, GL_RGBA__, GL_RGBA8__, GL_RGBA__, pixels);
	ktx.resize(ktx.size() - 8);
	CMemStream stream(true);
	stream.fill(const_cast<uint8 *>(&ktx[0]), (uint32)ktx.size());
	CBitmap bitmap;
	EXPECT_FALSE(bitmap.loadUntrusted(stream, 4096));
}

// The reason readKTX() takes maxSize: CBitmap::loadUntrusted() must be able to refuse a
// KTX that declares a huge size, without attempting the allocation first. This is the one
// container reader where the sizes come straight from the file header.
TEST(CBitmapKTX, UntrustedHugeSizeRefused)
{
	// 1x1 of real data, but claim 100000 x 100000 in the header.
	TBytes pixels(4, 0);
	TBytes ktx = buildKTX(1, 1, GL_UNSIGNED_BYTE__, 1, GL_RGBA__, GL_RGBA8__, GL_RGBA__, pixels);
	// pixelWidth is the 7th uint32 of the header: 12 (id) + 6 * 4
	const size_t widthOffset = 12 + 6 * 4;
	const size_t heightOffset = 12 + 7 * 4;
	const uint32 huge = 100000;
	for (uint32 i = 0; i < 4; ++i)
	{
		ktx[widthOffset + i] = (uint8)((huge >> (8 * i)) & 0xff);
		ktx[heightOffset + i] = (uint8)((huge >> (8 * i)) & 0xff);
	}

	CMemStream stream(true);
	stream.fill(const_cast<uint8 *>(&ktx[0]), (uint32)ktx.size());

	CBitmap bitmap;
	EXPECT_FALSE(bitmap.loadUntrusted(stream, 4096));
	EXPECT_EQ(0u, (uint32)bitmap.getWidth());
	EXPECT_EQ(0u, (uint32)bitmap.getHeight());
}

// Same property, but through plain load(): it caps at MAX_BITMAP_SIZE, and a header that
// exceeds the cap must be REFUSED, not attempted. Without the check in readKTX() this call
// reaches resize() with the declared size and dies on std::bad_alloc (or worse, on a machine
// without overcommit), which is exactly what the test would report as a failure.
TEST(CBitmapKTX, LoadRefusesSizeAboveMaxInsteadOfAllocating)
{
	const uint32 tooBig = NLMISC::MAX_BITMAP_SIZE * 2;
	TBytes pixels(4, 0);
	TBytes ktx = buildKTX(1, 1, GL_UNSIGNED_BYTE__, 1, GL_RGBA__, GL_RGBA8__, GL_RGBA__, pixels);
	const size_t widthOffset = 12 + 6 * 4;
	const size_t heightOffset = 12 + 7 * 4;
	for (uint32 i = 0; i < 4; ++i)
	{
		ktx[widthOffset + i] = (uint8)((tooBig >> (8 * i)) & 0xff);
		ktx[heightOffset + i] = (uint8)((tooBig >> (8 * i)) & 0xff);
	}

	CMemStream stream(true);
	stream.fill(const_cast<uint8 *>(&ktx[0]), (uint32)ktx.size());
	CBitmap bitmap;
	uint8 depth = 0;
	EXPECT_NO_THROW(depth = bitmap.load(stream)) << "an oversized header must be refused, not allocated";
	EXPECT_EQ(0, (int)depth);
}

// The declared size of a LEVEL is as untrusted as the header: a small image could otherwise
// ask for a megabyte-sized level buffer before anything looks at it. Both layouts are
// exercised - the compressed block layout and the uncompressed row layout. load() is used on
// purpose: loadUntrusted() would swallow the resulting exception and pass either way, so the
// test would not bite. Only the bound makes these calls return 0 instead of throwing.
TEST(CBitmapKTX, RejectOversizedLevel)
{
	const size_t levelSizeOffset = 12 + 13 * 4;
	const uint32 oversize = 0x00100000u; // 1 MiB claimed for a 64-byte / 16-byte level

	// 4x4 RGBA8: legal header, level claims 1 MiB.
	TBytes pixels(64, 0x33);
	TBytes ktx = buildKTX(4, 4, GL_UNSIGNED_BYTE__, 1, GL_RGBA__, GL_RGBA8__, GL_RGBA__, pixels);
	for (uint32 i = 0; i < 4; ++i)
		ktx[levelSizeOffset + i] = (uint8)((oversize >> (8 * i)) & 0xff);
	{
		CMemStream stream(true);
		stream.fill(const_cast<uint8 *>(&ktx[0]), (uint32)ktx.size());
		CBitmap bitmap;
		uint8 depth = 1;
		EXPECT_NO_THROW(depth = bitmap.load(stream)) << "uncompressed level size must be bounded";
		EXPECT_EQ(0, (int)depth);
	}

	// 4x4 DXT5: one 16-byte block, level claims 1 MiB.
	TBytes blocks(16, 0x44);
	TBytes ktx2 = buildKTX(4, 4, 0, 0, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT__,
		GL_COMPRESSED_RGBA_S3TC_DXT5_EXT__, blocks);
	for (uint32 i = 0; i < 4; ++i)
		ktx2[levelSizeOffset + i] = (uint8)((oversize >> (8 * i)) & 0xff);
	{
		CMemStream stream(true);
		stream.fill(const_cast<uint8 *>(&ktx2[0]), (uint32)ktx2.size());
		CBitmap bitmap;
		uint8 depth = 1;
		EXPECT_NO_THROW(depth = bitmap.load(stream)) << "compressed level size must be bounded";
		EXPECT_EQ(0, (int)depth);
	}
}

// The level COUNT is untrusted too. It used to be cast to uint8 before any limit was applied,
// so a file claiming 256 levels became 0, no level was read at all, and the reader still
// returned a valid-looking depth with _Width/_Height set and _Data[0] empty - a bitmap the
// texture uploader would happily try to upload. DDS refuses this in its header check
// (bitmap.cpp:610, "header[6] > MAX_MIPMAP"); KTX now does the same.
TEST(CBitmapKTX, RejectTooManyMipmapLevels)
{
	TBytes pixels(2 * 2 * 4, 0x21);
	// 256 is chosen because (uint8)256 == 0, which is exactly what made the old code report
	// success with an empty level 0. 17 (one over the limit) is checked too.
	const uint32 claimed[] = { 17, 256 };
	for (uint32 c = 0; c < sizeof(claimed) / sizeof(claimed[0]); ++c)
	{
		TBytes ktx = buildKTX(2, 2, GL_UNSIGNED_BYTE__, 1, GL_RGBA__, GL_RGBA8__, GL_RGBA__, pixels,
			1, 0, claimed[c]);
		CMemStream stream(true);
		stream.fill(const_cast<uint8 *>(&ktx[0]), (uint32)ktx.size());
		CBitmap bitmap;
		uint8 depth = 1;
		EXPECT_NO_THROW(depth = bitmap.load(stream)) << "mipmap count " << claimed[c];
		EXPECT_EQ(0, (int)depth) << "mipmap count " << claimed[c] << " must be refused";
		EXPECT_EQ(0u, (uint32)bitmap.getWidth());
	}
}
