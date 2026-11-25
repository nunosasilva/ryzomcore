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
 * (five authored bands over 0..50m, authored densities/scales, no merged-far).
 *
 * On, the microvegetation takes over the stock layout: every authored species
 * list of the tileset is generated into the farthest of the five distance
 * buckets and reaches NL3D_VEGETABLE_BLOCK_EXTRA_FAR_DIST (100m) instead of the
 * stock 0..50m. The `.vegetset` file format is untouched; only the bucket
 * contents and the bucket edges change.
 *
 * Why one bucket (the farthest):
 * CLandscapeVegetableBlock::update() keeps every bucket i >= _CurDistType
 * alive at the same time ([Cur..4]), so the farthest bucket is the only one
 * that is alive at every distance. Generating a species there means it is
 * created once - when the block enters the far edge - and is never created or
 * deleted again while walking: no ring, no band vanishing, still exactly one
 * copy of each species.
 *
 * The two previous experiments are not kept, because both made plants appear or
 * disappear at full alpha on a bucket crossing:
 * - one authored band per bucket (each band stretched to its bucket) fades each
 *   band in across its whole width and deletes it wholesale at the bucket edge;
 *   at 100m that reads as rings at ~35/55/75m and large areas of grass
 *   disappearing at 20/40/60/80m;
 * - copying the lower bands into the farthest bucket duplicates instances over
 *   the same ground and draws them several times at the same pixels.
 * The rule that any future layout must keep: because [Cur..4] are alive
 * together and the vertex program fades each source to alpha 0 at its own
 * reach, a species must live in exactly one bucket, and its reach must be that
 * bucket's far edge. Here: one bucket, one reach (the far edge).
 *
 * Density/scale: a tile keeps its authored instance count and scale per band
 * (1.0), so the close-up is stock by construction; stretching a band only keeps
 * those instances alive from farther away. The cost of the extension is the
 * larger number of tiles that stay alive, not more instances per tile. The
 * RYZOM_VEG_MERGED_DENSITY / RYZOM_VEG_MERGED_SCALE environment overrides exist
 * for measurement runs only; they are read once per process.
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
		 *	bucket it is generated into and its reach are independent of it (every
		 *	band goes into the farthest bucket), so this is what lets the
		 *	RYZOM_VEG_STATE log attribute instances to a band.
		 */
		uint				Band;
	};

	CVegetableExtra();

	/** Master switch: false = stock layout, true = merged-far layout. Returns true
	 *	only if the layout actually changed, which is how CVegetableManager decides
	 *	whether the caller has to rebuild the instance groups.
	 */
	bool					setEnabled(bool enabled);
	bool					isEnabled() const { return _Enabled; }

	/// Distance the extended layout reaches, in metres (meaningful when enabled).
	float					getFarEdgeM() const { return _FarEdgeM; }
	/// Width of one distance bucket in the extended layout, in metres.
	float					getBandDist() const { return _FarEdgeM / (float)NL3D_VEGETABLE_BLOCK_NUMDIST; }
	/// Squared bucket edges used by the block distance state machine.
	/// Index 0..NUMDIST; the last entry is the far edge.
	const float				*getSqrDistLUT() const { return _SqrDistLUT; }

	/// Distance of the farthest ZSort blend layer needed by the extended layout:
	/// the far edge plus one band of margin, which keeps the farthest sort blocks
	/// out of the last-layer clamp instead of piling them on the last layer.
	float					getZSortDistMax() const { return _FarEdgeM + getBandDist(); }

	/** True if this tileset's bucket has anything to generate. On, every authored
	 *	list lives in the farthest bucket and the other buckets are empty.
	 */
	bool					hasSources(const CTileVegetableDesc &tileVegetDesc, uint distType) const;

	/** True if ANY tileset can put something in this bucket - the same rule
	 *	without the tileset. Only the farthest bucket can ever be filled, so
	 *	CLandscapeVegetableBlock::update() skips creating the instance groups of
	 *	the others instead of allocating one per tile just to delete it again.
	 */
	bool					hasAnySources(uint distType) const;

	/** Density of every source in the extended layout. 1.0 reproduces the stock
	 *	close-up exactly; override with RYZOM_VEG_MERGED_DENSITY for measurement.
	 */
	static float			getMergedDensity();
	/** Scale of every source in the extended layout, 1.0 = authored scale; same
	 *	rationale as getMergedDensity(). RYZOM_VEG_MERGED_SCALE overrides it.
	 */
	static float			getMergedScale();

	/** Fill 'sources' with the lists generated for one tile's bucket in the
	 *	extended layout: in the farthest bucket, every authored list of the
	 *	tileset, each source keeping its own band seed and reaching the far edge.
	 *	The other buckets produce nothing.
	 */
	void					buildSources(const CTileVegetableDesc &tileVegetDesc, uint distType,
								std::vector<CSource> &sources) const;

private:
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
