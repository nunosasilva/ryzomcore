/** \file pixel_program.cpp
 * Pixel program definition
 */

/* Copyright, 2000, 2001 Nevrax Ltd.
 *
 * This file is part of NEVRAX NEL.
 * NEVRAX NEL is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.

 * NEVRAX NEL is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with NEVRAX NEL; see the file COPYING. If not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA.
 */

#include "std3d.h"

#include <nel/3d/pixel_program.h>

#include <nel/3d/driver.h>

namespace NL3D 
{

// ***************************************************************************

CPixelProgram::CPixelProgram(CGPUProgramSourceCont *programSource) : _Info(NULL), IGPUProgram(programSource)
{
	
}

// ***************************************************************************

CPixelProgram::~CPixelProgram ()
{
	delete _Info;
	_Info = NULL;
}

// ***************************************************************************

void CPixelProgram::buildInfo(const char *displayName, uint features)
{
	nlassert(_Info == NULL);
	_Info = new CPixelProgramInfo();
	CPixelProgramInfo *info = _Info;
	info->DisplayName = displayName;
	info->Features = features;
	if (features & CPixelProgramInfo::Fog)
	{
		if (features & CPixelProgramInfo::DynamicFog)
		{
			info->FogEnabledIdx = getParamIdx("nlFogEnabled");
			if (info->FogEnabledIdx == ~0)
			{
				nlwarning("Missing 'nlFogEnabled' in gpu program '%s', DynamicFog disabled", displayName);
				info->Features &= ~CPixelProgramInfo::DynamicFog;
			}
		}
		info->FogStartEndIdx = getParamIdx("nlFogStartEnd");
		info->FogColorIdx = getParamIdx("nlFogColor");
		if (info->FogStartEndIdx == ~0
			|| info->FogStartEndIdx == ~0)
		{
			nlwarning("Missing 'nlFogStartEnd/nlFogColor' in gpu program '%s', Fog disabled", displayName);
			info->Features &= ~CPixelProgramInfo::Fog;
		}
	}
}

} // NL3D
