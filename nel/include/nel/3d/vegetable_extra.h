// NeL - MMORPG Framework <http://dev.ryzom.com/projects/nel/>
// Copyright (C) 2010  Winch Gate Property Limited
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

#ifndef NL_VEGETABLE_EXTRA_H
#define NL_VEGETABLE_EXTRA_H

#include "nel/misc/types_nl.h"
#include "nel/3d/vegetable_def.h"

#include <vector>


namespace NL3D
{


class CTileVegetableDesc;
class CVegetable;


// ***************************************************************************
/**
 * Opt-in extended microvegetation (client.cfg MicroVegetExtra).
 *
 * MicroVegetExtra is a switch: 0 = off, 1 = on. Off, this class is not used at
 * all and CPatch::generateTileVegetable() runs the stock path byte for byte
 * (five authored bands over 0..50m, authored densities/scales, no copy).
 *
 * On, the microvegetation uses the far-only extension:
 * - bands 0..2 keep their STOCK reach (10/20/30m) and their authored density
 *   and scale (1.0). The near tall grass is exactly stock, no thinning and no
 *   stretch, so a 6m walk cannot move it farther than stock does.
 * - bands 3 and 4 are generated into the farthest bucket and reach 100m, at
 *   the authored density/scale (1.0). They are the only bands that are
 *   extended; the near field is not copied into the far field.
 * - no fade, crossfade or gated carrier is used (union3/4/5 were rejected).
 *
 * The block distance layout is therefore non-uniform: edges 0/10/20/30m for
 * the three near bands, then the farthest bucket covers 30..100m and holds
 * both far bands. Bucket 3 (the 30..60m slot) is never filled; the farthest
 * bucket (index 4) is created when a block enters 30m and stays alive to
 * 100m, so bands 3/4 reach the full far edge.
 *
 * The union2 and merged-far layouts are kept only as measurement layouts,
 * selected with RYZOM_VEG_EXTRA_LAYOUT=union2|merged-far; the switch and
 * client.cfg always select far-only. Stock (MicroVegetExtra=0) is unaffected.
 *
 * Constraint: CLandscapeVegetableBlock::update() keeps every bucket
 * i >= _CurDistType alive at once, and the vertex program fades a source to
 * alpha 0 at its own reach. Near bands are in their own buckets with their
 * stock reach; the far bands live only in the farthest bucket with the far
 * edge reach, so no species is drawn twice for the same purpose and no bucket
 * crossing introduces an alpha step.
 */
class CVegetableExtra
{
public:
	/// One species list to generate into a tile's instance group.
	struct CSource
	{
		const CVegetable	*Veget;
		uint32				Seed;
		/// Multiplier on the authored instance count (1 = authored).
		float				Density;
		/// Multiplier on the authored instance scale (1 = authored).
		float				Scale;
		/** Distance (m) at which this source's instances reach alpha 0. It is
			baked per instance (vegetable_manager.cpp: v[9].w) and the vertex
			program computes alpha = (reach - dist) / blend transition, so a
			source whose reach is shorter than its bucket's far edge is simply
			invisible in the outer part of that bucket.
		*/
		float				Reach;
		/** Which of the five authored species lists this source came from. The
		 *	bucket it is generated into and its reach are independent of it (the
			far bucket carries both far bands), so this is what lets the
			RYZOM_VEG_STATE log attribute instances to a band.
		 */
		uint				Band;
	};

	CVegetableExtra();

	/** Master switch: false = stock layout, true = far-only layout. Returns true
	 *	only if the layout actually changed, which is how CVegetableManager decides
	 *	whether the caller has to rebuild the instance groups.
	 */
	bool					setEnabled(bool enabled);
	bool					isEnabled() const { return _Enabled; }

	/// Distance the extended layout reaches, in metres (meaningful when enabled).
	float					getFarEdgeM() const { return _FarEdgeM; }
	/// Width of one distance bucket in the classic union2 layout, in metres.
	float					getBandDist() const { return _FarEdgeM / (float)NL3D_VEGETABLE_BLOCK_NUMDIST; }
	/// Far edge of one bucket in the active layout (far-only: 10/20/30/60/100m).
	float					getBandFarDist(uint distType) const;

	/// Squared bucket edges used by the block distance state machine.
	/// Index 0..NUMDIST; the last entry is the far edge.
	const float				*getSqrDistLUT() const { return _SqrDistLUT; }

	/// Distance of the farthest ZSort blend layer needed by the extended layout:
	/// the far edge plus one band of margin, which keeps the farthest sort blocks
	/// out of the last-layer clamp instead of piling them on the last layer.
	float					getZSortDistMax() const { return _FarEdgeM + getBandDist(); }

	/// Layout name for the logs: "far-only", "union2" or "merged-far".
	const char				*getLayoutName() const;

	/** True if this tileset's bucket has anything to generate.
	 *	far-only: own list in buckets 0..2; bucket 3 is empty; bucket 4 holds
	 *	bands 3+4 (either one). union2: own list, plus lower bands in bucket 4.
	 *	merged-far: all bands in bucket 4.
	 */
	bool					hasSources(const CTileVegetableDesc &tileVegetDesc, uint distType) const;

	/** True if ANY tileset can put something in this bucket. far-only fills
	 *	buckets 0..2 and 4 (bucket 3 is skipped); union2/stock fill every bucket;
	 *	merged-far fills only bucket 4.
	 */
	bool					hasAnySources(uint distType) const;

	/// Density/scale of a bucket's own authored list (union2 measurement layout).
	float					getOwnDensity(const CTileVegetableDesc &tileVegetDesc, uint distType) const;
	float					getOwnScale(uint distType) const;
	/// Union2 band factors for the own authored list (bands 0..3 / far band).
	static float			getBandDensity() { return 0.28f; }
	static float			getBandScale() { return 1.40f; }
	static float			getFarDensity() { return 0.22f; }
	static float			getFarScale() { return 1.45f; }
	/// Density of the lower-band copies in the farthest union2 bucket.
	static float			getUnionDensity() { return 0.26f; }
	/// Far-bucket density when every lower band is empty (far-only tilesets).
	static float			getFarOnlyDensity() { return 1.f; }

	/** Density/scale of every source in the merged-far measurement layout.
	 *	1.0 reproduces the stock close-up by construction; override with
	 *	RYZOM_VEG_MERGED_DENSITY / RYZOM_VEG_MERGED_SCALE.
	 */
	static float			getMergedDensity();
	static float			getMergedScale();

	/// True only for the RYZOM_VEG_EXTRA_LAYOUT=merged-far measurement layout.
	bool					isMergedFar() const { return _Layout == MergedFarLayout; }
	/// True for the RYZOM_VEG_EXTRA_LAYOUT=union2 measurement layout.
	bool					isUnion2() const { return _Layout == Union2Layout; }

	/** Fill 'sources' with the lists generated for one tile's bucket.
	 *	- far-only (default): buckets 0..2 generate their own authored band at
	 *	  density/scale 1.0, reach 10/20/30m; bucket 4 generates bands 3 and 4
	 *	  at density/scale 1.0, reach 100m. Bucket 3 is empty.
	 *	- union2/merged-far: measurement layouts, see the class comment.
	 */
	void					buildSources(const CTileVegetableDesc &tileVegetDesc, uint distType,
								std::vector<CSource> &sources) const;

private:
	enum TLayout
	{
		FarOnlyLayout = 0,
		Union2Layout,
		MergedFarLayout,
	};

	TLayout					_Layout;
	bool					_Enabled;
	/// Distance the extended layout reaches (NL3D_VEGETABLE_BLOCK_EXTRA_FAR_DIST,
	/// or RYZOM_VEG_FAR_EDGE_M for measurement runs). Always built, so the LUT is
	/// valid even while the switch is off; the stock LUT is used then.
	float					_FarEdgeM;
	/// (band edge * i)^2, monotonically increasing: the bucket edges.
	float					_SqrDistLUT[NL3D_VEGETABLE_BLOCK_NUMDIST + 1];

	void					buildLUT();
};


} // NL3D


#endif // NL_VEGETABLE_EXTRA_H

/* End of vegetable_extra.h */
