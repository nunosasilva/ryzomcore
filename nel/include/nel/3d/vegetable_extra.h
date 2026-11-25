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
 * (five authored bands over 0..50m, authored densities/scales, no union).
 *
 * On, the microvegetation takes over the stock layout with the union2 layout:
 * the five authored species lists stay in their OWN distance buckets, but the
 * five stock 10m bands are stretched to five 20m bands over 0..100m, so a
 * species keeps its authored band identity but reaches 20/40/60/80/100m
 * instead of 10/20/30/40/50m. The farthest bucket additionally carries a
 * thinned copy (0.26) of every lower band's list, with that lower band's seed
 * and scale, so a species keeps existing across its own band's outer edge and
 * the band boundary never swaps the plants. This is the layout the user
 * validated as union2; the `.vegetset` file format is untouched.
 *
 * Why not all bands in one bucket (the retired merged-far layout): that made
 * the near-authored bands (the tall grass) reach 100m from every tile, so a
 * 6m walk across a tile whose near bands are sparse made the tall grass at the
 * feet disappear. The near bands must keep their own (short) reaches. The
 * merged-far mapping is kept only behind RYZOM_VEG_EXTRA_LAYOUT=merged-far as a
 * measurement knob; MicroVegetExtra=1 always selects union2.
 *
 * The constraint every layout keeps: CLandscapeVegetableBlock::update() keeps
 * every bucket i >= _CurDistType alive at the same time, and the vertex program
 * fades a source to alpha 0 at its own reach. A species may therefore live in
 * one bucket (its own), and the far bucket copies of the lower bands only
 * extend the same instances a thinned amount, never introduce a new position or
 * a 0->1 alpha step. No fade/crossfade/gated carrier is used: union3/4/5 were
 * tested and rejected.
 *
 * Density/scale: union2 uses the authored band thinning chosen by union2
 * (0.28/1.40 for bands 0..3, 0.22/1.45 for the far band, 1.0 for far-only
 * tilesets, and 0.26 for the far-bucket copies). The RYZOM_VEG_MERGED_DENSITY /
 * _SCALE overrides exist only for the merged-far measurement layout.
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
		 *	far bucket carries copies of the lower bands), so this is what lets
		 *	the RYZOM_VEG_STATE log attribute instances to a band.
		 */
		uint				Band;
	};

	CVegetableExtra();

	/** Master switch: false = stock layout, true = union2 layout. Returns true
	 *	only if the layout actually changed, which is how CVegetableManager decides
	 *	whether the caller has to rebuild the instance groups.
	 */
	bool					setEnabled(bool enabled);
	bool					isEnabled() const { return _Enabled; }

	/// Distance the extended layout reaches, in metres (meaningful when enabled).
	float					getFarEdgeM() const { return _FarEdgeM; }
	/// Width of one distance bucket in the extended layout, in metres.
	float					getBandDist() const { return _FarEdgeM / (float)NL3D_VEGETABLE_BLOCK_NUMDIST; }
	/// Far edge of one bucket in the extended layout (20/40/60/80/100m).
	float					getBandFarDist(uint distType) const;

	/// Squared bucket edges used by the block distance state machine.
	/// Index 0..NUMDIST; the last entry is the far edge.
	const float				*getSqrDistLUT() const { return _SqrDistLUT; }

	/// Distance of the farthest ZSort blend layer needed by the extended layout:
	/// the far edge plus one band of margin, which keeps the farthest sort blocks
	/// out of the last-layer clamp instead of piling them on the last layer.
	float					getZSortDistMax() const { return _FarEdgeM + getBandDist(); }

	/** True if this tileset's bucket has anything to generate. Union2: the
	 *	bucket's own authored list; the farthest bucket also counts when only
	 *	lower bands have species (far-only or mixed tilesets).
	 */
	bool					hasSources(const CTileVegetableDesc &tileVegetDesc, uint distType) const;

	/** True if ANY tileset can put something in this bucket - the same rule
	 *	without the tileset. Union2 fills every bucket (near: own band; far: own
	 *	band plus lower-band copies), so no bucket can be skipped.
	 */
	bool					hasAnySources(uint distType) const;

	/// Density/scale of a bucket's own authored list (union2).
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

	/** True only when RYZOM_VEG_EXTRA_LAYOUT=merged-far selected the old
	 *	all-bands-in-the-farthest-bucket mapping for measurement. Never selected
	 *	by MicroVegetExtra itself.
	 */
	bool					isMergedFar() const { return _Layout == MergedFarLayout; }

	/** Fill 'sources' with the lists generated for one tile's bucket in the
	 *	extended layout.
	 *	- union2 (default): the bucket's own authored list at its band density/
	 *	  scale, reach = that band's far edge; in the farthest bucket, the lower
	 *	  bands' species are appended at the union density with their own seed and
	 *	  scale, reaching the far edge.
	 *	- merged-far (measurement): every authored list in the farthest bucket,
	 *	  at merged density/scale, reaching the far edge.
	 */
	void					buildSources(const CTileVegetableDesc &tileVegetDesc, uint distType,
								std::vector<CSource> &sources) const;

private:
	enum TLayout
	{
		Union2Layout = 0,
		MergedFarLayout,
	};

	TLayout					_Layout;
	bool					_Enabled;
	/// Distance the extended layout reaches (NL3D_VEGETABLE_BLOCK_EXTRA_FAR_DIST,
	/// or RYZOM_VEG_FAR_EDGE_M for measurement runs). Always built, so the LUT is
	/// valid even while the switch is off; the stock LUT is used then.
	float					_FarEdgeM;
	/// (band width * i)^2, monotonically increasing: the bucket edges.
	float					_SqrDistLUT[NL3D_VEGETABLE_BLOCK_NUMDIST + 1];

	void					buildLUT();
};


} // NL3D


#endif // NL_VEGETABLE_EXTRA_H

/* End of vegetable_extra.h */
