// NeL - MMORPG Framework <https://wiki.ryzom.dev/>
// Copyright (C) 2026  The Ryzom Core Community
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

/**
 * Occlusion queries over VK_QUERY_TYPE_OCCLUSION.
 *
 * The only user is the flare system (CFlareModel): it draws the object that
 * emits a flare with colour writes off, then checks two frames later whether
 * any sample passed the depth test, and uses the two counts as a visibility
 * ratio. Nothing needs the result in the frame that recorded it, so the CPU
 * must never wait on the GPU here.
 *
 * Layout:
 *  - two query pools, one per frame in flight. beginFrame() resets a pool
 *    inside the frame command buffer, before the render pass starts (the
 *    reset is only legal outside a render pass), at which point the fence for
 *    that slot has been waited and the pool's previous submission is done.
 *    The other pool belongs to the frame still in flight and is untouched.
 *  - a query object takes a fresh slot from the current frame's pool at every
 *    begin(). Slots are therefore never reused while pending, even if the
 *    engine reuses the IOcclusionQuery object, and no per-slot reset is
 *    needed.
 *  - beginFrame() also harvests the results of the queries recorded in that
 *    pool (the fence just waited proves they are available) and caches them
 *    in the query objects. This is what keeps a result valid if the engine
 *    polls it after the pool has been reset.
 *  - getOcclusionType() only reads the pool when the result was not harvested
 *    yet and the issuing frame's fence has passed; it uses
 *    VK_QUERY_RESULT_WITH_AVAILABILITY_BIT and never WAIT_BIT.
 */

#include "stdvulkan.h"
#include "driver_vulkan.h"

#include "nel/3d/occlusion_query.h"

#include <algorithm>

using namespace NLMISC;
using namespace NL3D;

namespace NLVULKAN {

class COcclusionQueryVulkan : public IOcclusionQuery
{
public:
	explicit COcclusionQueryVulkan(CDriverVulkan *driver)
		: _Driver(driver), _Active(false), _Recorded(false), _Pool(0), _Slot(0)
		, _IssuedFrame(0), _Result(NotAvailable), _VisibleCount(0)
	{
	}

	virtual ~COcclusionQueryVulkan() NL_OVERRIDE
	{
	}

	virtual void begin() NL_OVERRIDE;
	virtual void end() NL_OVERRIDE;
	virtual TOcclusionType getOcclusionType() NL_OVERRIDE;
	virtual uint getVisibleCount() NL_OVERRIDE;

private:
	friend class CDriverVulkan;

	/// Read the result once, without blocking. Returns false when the query
	/// has not executed yet (only possible before its frame's fence is
	/// waited, or for a query that never reached a command buffer).
	bool readResult()
	{
		if (!_Recorded)
		{
			// The query never reached a command buffer (no frame open, no
			// pool, pool exhausted). Report visible so the flare system is
			// not stuck waiting forever.
			_Result = NotOccluded;
			_VisibleCount = 1;
			return true;
		}
		if (_Driver->_OcclusionCompletedFrame[_Pool] < _IssuedFrame)
			return false;
		// With WITH_AVAILABILITY the data of one query is its 32-bit result
		// followed by its 32-bit availability flag.
		uint32 data[2] = { 0, 0 };
		const VkResult result = vkGetQueryPoolResults(_Driver->_Device, _Driver->_OcclusionPools[_Pool],
			_Slot, 1, sizeof(data), data, 2 * sizeof(uint32), VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
		if (result != VK_SUCCESS || data[1] == 0)
			return false;
		_VisibleCount = data[0];
		_Result = _VisibleCount != 0 ? NotOccluded : Occluded;
		return true;
	}

	/// Called by the driver when the frame that recorded this query is known
	/// to be finished. Results are cached here so a later pool reset cannot
	/// invalidate a poll that comes late.
	void harvest(uint32 pool)
	{
		if (!_Recorded || _Result != NotAvailable || _Pool != pool)
			return;
		++_Driver->_StatsOcclusionHarvested;
		if (!readResult())
		{
			// The fence already passed, so an unavailable result is a driver
			// bug, not a timing issue: keep the flare visible instead of
			// freezing its fade.
			nlwarning("Vulkan: occlusion query %u (pool %u) has no result after its frame fence",
				_Slot, pool);
			_Result = NotOccluded;
			_VisibleCount = 1;
		}
	}

	CDriverVulkan	*_Driver;
	/// Between begin() and end().
	bool			_Active;
	/// begin() and end() were recorded into a command buffer.
	bool			_Recorded;
	/// Pool and slot of the last issue. The pool is also the frame slot.
	uint32			_Pool;
	uint32			_Slot;
	uint64			_IssuedFrame;
	/// Cached result; NotAvailable until readResult() succeeds.
	TOcclusionType	_Result;
	uint			_VisibleCount;
};

void COcclusionQueryVulkan::begin()
{
	nlassert(!_Active);
	_Active = true;
	_Result = NotAvailable;
	_VisibleCount = 0;
	_Recorded = false;

	// Queries are recorded into the frame command buffer, so they can only be
	// issued while a frame is open. The flare pass always runs there.
	if (!_Driver->_FrameOpen)
		return;
	const uint32 frame = _Driver->_FrameIndex;
	if (!_Driver->_OcclusionPools[frame])
		return;
	if (_Driver->_OcclusionNextSlot[frame] >= CDriverVulkan::NL_VK_OCCLUSION_SLOTS)
	{
		// More queries in one frame than the pool holds. Do not reuse a slot
		// that may still be in flight; the flare is drawn without testing.
		return;
	}

	_Pool = frame;
	_Slot = _Driver->_OcclusionNextSlot[frame]++;
	_IssuedFrame = _Driver->_SwapBufferCounter;
	_Driver->_OcclusionIssuedFrame[frame] = _IssuedFrame;
	vkCmdBeginQuery(_Driver->_CommandBuffers[frame], _Driver->_OcclusionPools[frame], _Slot,
		_Driver->_OcclusionPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
	_Driver->_OcclusionFrameQueries[frame].push_back(this);
	++_Driver->_StatsOcclusionIssued;
	_Recorded = true;
}

void COcclusionQueryVulkan::end()
{
	nlassert(_Active);
	_Active = false;
	if (!_Recorded)
	{
		// begin() could not record: report visible (see readResult()).
		_Result = NotOccluded;
		_VisibleCount = 1;
		return;
	}
	nlassert(_Pool == _Driver->_FrameIndex);
	vkCmdEndQuery(_Driver->_CommandBuffers[_Pool], _Driver->_OcclusionPools[_Pool], _Slot);
}

IOcclusionQuery::TOcclusionType COcclusionQueryVulkan::getOcclusionType()
{
	nlassert(!_Active);
	if (_Result == NotAvailable)
		readResult();
	return _Result;
}

uint COcclusionQueryVulkan::getVisibleCount()
{
	if (getOcclusionType() == NotAvailable)
		return 0;
	return _VisibleCount;
}

void CDriverVulkan::harvestOcclusionQueries(uint32 frame)
{
	std::vector<COcclusionQueryVulkan *> &list = _OcclusionFrameQueries[frame];
	for (size_t i = 0; i < list.size(); ++i)
		list[i]->harvest(frame);
	list.clear();
}

IOcclusionQuery *CDriverVulkan::createOcclusionQuery()
{
	if (!supportOcclusionQuery())
		return NULL;
	++_OcclusionCreatedCount;
	if (_OcclusionCreatedCount == 1)
		nlinfo("Vulkan: first occlusion query created (flare system active)");
	return new COcclusionQueryVulkan(this);
}

void CDriverVulkan::deleteOcclusionQuery(IOcclusionQuery *oq)
{
	if (!oq)
		return;
	COcclusionQueryVulkan *query = NLMISC::safe_cast<COcclusionQueryVulkan *>(oq);
	nlassert(query->_Driver == this);
	// The object may be destroyed during a frame, after it was issued: remove
	// it from the harvest lists so a later beginFrame() never touches it.
	for (uint i = 0; i < 2; ++i)
	{
		std::vector<COcclusionQueryVulkan *> &list = _OcclusionFrameQueries[i];
		list.erase(std::remove(list.begin(), list.end(), query), list.end());
	}
	delete query;
}

} /* namespace NLVULKAN */
