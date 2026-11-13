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
 * Hardware cursor handling for the Vulkan driver.
 *
 * This is the X11 part of the GL3 driver's driver_opengl3_inputs.cpp. The
 * engine turns textures listed in client.cfg's HardwareCursors into driver
 * cursors with addCursor(), then selects one from the interface every frame
 * (CViewPointer::drawCursor -> IDriver::setCursor). Without this, the X
 * pointer keeps the root arrow: the WM's resize cursor still appears over the
 * window border, but Ryzom's own cursors (curs_default, curs_resize_*, the
 * contextual attack/take/talk shapes) never do.
 */

#include "stdvulkan.h"
#include "driver_vulkan.h"

#if defined(NL_OS_UNIX) && !defined(NL_OS_MAC)
# include <X11/Xatom.h>
# ifdef HAVE_XRENDER
#  include <X11/extensions/Xrender.h>
# endif // HAVE_XRENDER
# ifdef HAVE_XCURSOR
#  include <X11/Xcursor/Xcursor.h>
# endif // HAVE_XCURSOR
#endif // defined(NL_OS_UNIX) && !defined(NL_OS_MAC)

#include "nel/misc/common.h"

#include <algorithm>

using namespace std;
using namespace NLMISC;

namespace NLVULKAN {

// ---------------------------------------------------------------------------
// Cursor storage
// ---------------------------------------------------------------------------

CDriverVulkan::CCursor::CCursor()
	: OrigHeight(32)
	, HotspotScale(1.f)
	, HotspotOffsetX(0)
	, HotspotOffsetY(0)
	, HotSpotX(0)
	, HotSpotY(0)
	, Cursor(EmptyCursor)
	, Col(CRGBA::White)
	, Rot(0)
	, Dpy(nullptr)
{
}

CDriverVulkan::CCursor::~CCursor()
{
	reset();
}

void CDriverVulkan::CCursor::reset()
{
	if (Cursor != EmptyCursor && Dpy)
	{
		XFreeCursor(Dpy, Cursor);
		XSync(Dpy, False);
	}
	Cursor = EmptyCursor;
}

CDriverVulkan::CCursor& CDriverVulkan::CCursor::operator=(const CDriverVulkan::CCursor &from)
{
	if (&from == this)
		return *this;
	// Drop the handle this entry may still own; addCursor() reassigns an
	// existing cursor when a texture is reloaded.
	reset();
	Src = from.Src;
	OrigHeight = from.OrigHeight;
	HotspotScale = from.HotspotScale;
	HotspotOffsetX = from.HotspotOffsetX;
	HotspotOffsetY = from.HotspotOffsetY;
	HotSpotX = from.HotSpotX;
	HotSpotY = from.HotSpotY;
	Cursor = from.Cursor;
	Col = from.Col;
	Rot = from.Rot;
	Dpy = from.Dpy;
	return *this;
}

// ---------------------------------------------------------------------------
// Capability and window cursors
// ---------------------------------------------------------------------------

bool CDriverVulkan::isAlphaBlendedCursorSupported()
{
	if (!_AlphaBlendedCursorSupportRetrieved)
	{
		_AlphaBlendedCursorSupported = false;

		if (_Display)
		{
#ifdef HAVE_XCURSOR
			if (XcursorSupportsARGB(_Display))
				_AlphaBlendedCursorSupported = true;
#endif // HAVE_XCURSOR

#ifdef HAVE_XRENDER
			// Same probe as the GL3 driver: ask for RENDER before using it,
			// and keep a version for the cursor creation path. Query it even
			// when XCursor works, so the XRender path stays a usable fallback.
			sint xrender_opcode = 0, xrender_event = 0, xrender_error = 0;
			if (XQueryExtension(_Display, "RENDER", &xrender_opcode, &xrender_event, &xrender_error) &&
				XRenderQueryExtension(_Display, &xrender_event, &xrender_error))
			{
				sint xrender_major = 0, xrender_minor = 0;
				XRenderQueryVersion(_Display, &xrender_major, &xrender_minor);
				_XRenderVersion = (uint32)(xrender_major * 100 + xrender_minor);
				if (_XRenderVersion > 0)
					_AlphaBlendedCursorSupported = true;
			}
#endif // HAVE_XRENDER
		}

		_AlphaBlendedCursorSupportRetrieved = true;
	}

	return _AlphaBlendedCursorSupported;
}

void CDriverVulkan::createCursors()
{
	if (!_Display || !_XWindow)
		return;

	if (_BlankCursor == EmptyCursor)
	{
		// A fully transparent 8x8 cursor. Defining it on the window is how the
		// driver hides the pointer while the interface draws its own.
		char bm_no_data[] = { 0, 0, 0, 0, 0, 0, 0, 0 };
		Pixmap pixmap_no_data = XCreateBitmapFromData(_Display, _XWindow, bm_no_data, 8, 8);
		XColor black;
		memset(&black, 0, sizeof(XColor));
		black.flags = DoRed | DoGreen | DoBlue;
		_BlankCursor = XCreatePixmapCursor(_Display, pixmap_no_data, pixmap_no_data, &black, &black, 0, 0);
		XFreePixmap(_Display, pixmap_no_data);
	}
}

void CDriverVulkan::releaseCursors()
{
	if (_Display && _XWindow)
		XUndefineCursor(_Display, _XWindow);
	_AppliedCursor = EmptyCursor;

	if (_Display && _BlankCursor != EmptyCursor)
		XFreeCursor(_Display, _BlankCursor);
	_BlankCursor = EmptyCursor;

	_Cursors.clear();

	// A frame may still try to set a cursor during shutdown; make it a no-op
	// rather than sending requests for a window that is about to be destroyed.
	_CurrName = "none";
}

// ---------------------------------------------------------------------------
// Cursor selection
// ---------------------------------------------------------------------------

void CDriverVulkan::updateCursor(bool forceRebuild)
{
	setCursor(_CurrName, _CurrCol, _CurrRot, _CurrHotSpotX, _CurrHotSpotY, forceRebuild);
}

void CDriverVulkan::setCursor(const std::string &name, NLMISC::CRGBA col, uint8 rot, sint hotSpotX, sint hotSpotY, bool forceRebuild)
{
	// The GL3 driver's behaviour: no RGBA cursor support means no shape change,
	// and "none" means the pointer is hidden until showCursor(true).
	if (!isAlphaBlendedCursorSupported() || _CurrName == "none") return;

	_CurrName = name;
	_CurrCol = col;
	_CurrRot = rot;
	_CurrHotSpotX = hotSpotX;
	_CurrHotSpotY = hotSpotY;

	// The name is cleared by showCursor(true), which lets the next frame pick
	// the shape again; an empty name does not reset the window cursor here.
	if (_CurrName.empty()) return;

	if (rot > 3) rot = 3; // same as CViewRenderer::drawRotFlipBitmapTiled

	TCursorMap::iterator it = _Cursors.find(name);
	nlCursor cursorHandle = EmptyCursor;

	if (it != _Cursors.end())
	{
		// Update the cursor if it was modified or has not been built yet.
		CCursor &curs = it->second;
		hotSpotX = hotSpotX - (sint)curs.HotspotOffsetX;
		hotSpotY = (sint)curs.OrigHeight - hotSpotY - (sint)curs.HotspotOffsetY;
		if (curs.Cursor == EmptyCursor ||
			curs.HotspotScale != _CursorScale ||
			curs.HotSpotX != hotSpotX ||
			curs.HotSpotY != hotSpotY ||
			curs.Col != col ||
			curs.Rot != rot ||
			forceRebuild)
		{
			curs.reset();
			curs.Cursor = buildCursor(curs.Src, col, rot, hotSpotX, hotSpotY);
			curs.Col = col;
			curs.Rot = rot;
			curs.HotspotScale = _CursorScale;
			curs.HotSpotX = hotSpotX;
			curs.HotSpotY = hotSpotY;
			curs.Dpy = _Display;
		}
		cursorHandle = curs.Cursor;
	}

	// The X11 fallback of isSystemCursorInClientArea() is always true, so the
	// GL3 guard (in client area or captured or forced) is unnecessary here.
	if (cursorHandle == _AppliedCursor)
		return; // the interface selects the same shape every frame
	if (!_Display || !_XWindow)
		return;
	if (cursorHandle == EmptyCursor)
		XUndefineCursor(_Display, _XWindow);
	else
		XDefineCursor(_Display, _XWindow, cursorHandle);
	_AppliedCursor = cursorHandle;
}

void CDriverVulkan::setCursorScale(float scale)
{
	_CursorScale = scale;
}

// ---------------------------------------------------------------------------
// Bitmap to X cursor
// ---------------------------------------------------------------------------

nlCursor CDriverVulkan::buildCursor(const CBitmap &src, NLMISC::CRGBA col, uint8 rot, sint hotSpotX, sint hotSpotY)
{
	nlassert(isAlphaBlendedCursorSupported());

	CBitmap rotSrc = src;
	if (rot > 3) rot = 3;
	switch (rot)
	{
		case 0: break;
		case 1: rotSrc.rot90CW(); break;
		case 2: rotSrc.rot90CW(); rotSrc.rot90CW(); break;
		case 3: rotSrc.rot90CCW(); break;
	}

	// The bitmap need not be square, and rotation changes which side is which.
	uint mouseW = rotSrc.getWidth();
	uint mouseH = rotSrc.getHeight();

	if (_CursorScale != 1.f)
	{
		// A cursor keeps at least one pixel, whatever scale it is given.
		mouseW = (uint)std::max(mouseW * _CursorScale, 1.f);
		mouseH = (uint)std::max(mouseH * _CursorScale, 1.f);
		hotSpotX = (sint)(hotSpotX * _CursorScale);
		hotSpotY = (sint)(hotSpotY * _CursorScale);
	}

	// The hotspot has to be inside the cursor, X11 refuses it otherwise (it is
	// not rotated with the bitmap).
	clamp(hotSpotX, 0, (sint)mouseW - 1);
	clamp(hotSpotY, 0, (sint)mouseH - 1);

	nlCursor result = EmptyCursor;
	convertBitmapToCursor(rotSrc, result, mouseW, mouseH, col, hotSpotX, hotSpotY);
	return result;
}

bool CDriverVulkan::convertBitmapToCursor(const NLMISC::CBitmap &bitmap, nlCursor &cursor, uint iconWidth, uint iconHeight, const NLMISC::CRGBA &col, sint hotSpotX, sint hotSpotY)
{
	CBitmap src = bitmap;

	// Resample if necessary.
	if (src.getWidth() != iconWidth || src.getHeight() != iconHeight)
		src.resample((sint32)iconWidth, (sint32)iconHeight);

	CBitmap colorBm;
	colorBm.resize(iconWidth, iconHeight, CBitmap::RGBA);
	const CRGBA *srcColorPtr = (CRGBA *)&(src.getPixels()[0]);
	const CRGBA *srcColorPtrLast = srcColorPtr + (iconWidth * iconHeight);
	CRGBA *destColorPtr = (CRGBA *)&(colorBm.getPixels()[0]);

	do
	{
		// Colorize the icon.
		destColorPtr->modulateFromColor(*srcColorPtr, col);

		// X11 wants BGRA pixels: swap red and blue channels.
		std::swap(destColorPtr->R, destColorPtr->B);

		// Premultiplied alpha.
		if (destColorPtr->A < 255)
		{
			destColorPtr->R = (destColorPtr->R * destColorPtr->A) / 255;
			destColorPtr->G = (destColorPtr->G * destColorPtr->A) / 255;
			destColorPtr->B = (destColorPtr->B * destColorPtr->A) / 255;
		}

		++srcColorPtr;
		++destColorPtr;
	}
	while (srcColorPtr != srcColorPtrLast);

#ifdef HAVE_XCURSOR

	if (XcursorSupportsARGB(_Display))
	{
		XcursorImage *image = XcursorImageCreate(iconWidth, iconHeight);

		if (!image)
		{
			nlwarning("Failed to create a XcursorImage with size %ux%u", iconWidth, iconHeight);
		}
		else
		{
			image->xhot = (uint)hotSpotX;
			image->yhot = (uint)hotSpotY;

			memcpy(image->pixels, &colorBm.getPixels(0)[0], colorBm.getSize() * 4);

			cursor = XcursorImageLoadCursor(_Display, image);

			XcursorImageDestroy(image);

			if (cursor != EmptyCursor)
				return true;
		}
	}

#endif // HAVE_XCURSOR

#ifdef HAVE_XRENDER

	if (_XRenderVersion > 0)
	{
		// use malloc() because X will free() the data itself
		CRGBA *src32 = (CRGBA *)malloc(colorBm.getSize() * 4);
		if (!src32)
			return false;
		memcpy(src32, &colorBm.getPixels(0)[0], colorBm.getSize() * 4);

		sint screen = DefaultScreen(_Display);
		Visual *visual = DefaultVisual(_Display, screen);

		if (!visual)
		{
			nlwarning("Failed to get a default visual for screen %d", screen);
			free(src32);
			return false;
		}

		XImage *image = XCreateImage(_Display, visual, 32, ZPixmap, 0, (char *)src32, iconWidth, iconHeight, 32, 0);
		if (!image)
		{
			nlwarning("Failed to create a cursor image");
			free(src32);
			return false;
		}

		Pixmap pixmap = XCreatePixmap(_Display, _XWindow, iconWidth, iconHeight, 32);
		if (!pixmap)
		{
			nlwarning("Failed to create a cursor pixmap %ux%ux%d", iconWidth, iconHeight, 32);
			XDestroyImage(image); // also frees src32
			return false;
		}

		GC gc = XCreateGC(_Display, pixmap, 0, nullptr);
		if (!gc)
		{
			nlwarning("Failed to create a cursor GC");
			XFreePixmap(_Display, pixmap);
			XDestroyImage(image); // also frees src32
			return false;
		}

		sint res = XPutImage(_Display, pixmap, gc, image, 0, 0, 0, 0, iconWidth, iconHeight);
		if (res)
			nlwarning("XPutImage failed with code %d", res);

		XFreeGC(_Display, gc);
		XDestroyImage(image); // also frees src32

		XRenderPictFormat *format = XRenderFindStandardFormat(_Display, PictStandardARGB32);
		if (!format)
		{
			nlwarning("Failed to find a standard ARGB32 format");
			XFreePixmap(_Display, pixmap);
			return false;
		}

		Picture picture = XRenderCreatePicture(_Display, pixmap, format, 0, nullptr);
		if (!picture)
		{
			nlwarning("Failed to create a cursor picture");
			XFreePixmap(_Display, pixmap);
			return false;
		}

		cursor = XRenderCreateCursor(_Display, picture, (uint)hotSpotX, (uint)hotSpotY);

		XRenderFreePicture(_Display, picture);
		XFreePixmap(_Display, pixmap);

		if (cursor == EmptyCursor)
		{
			nlwarning("Failed to create a cursor");
			return false;
		}
		return true;
	}

#endif // HAVE_XRENDER

	return false;
}

// ---------------------------------------------------------------------------
// Pointer visibility, warping and capture
// ---------------------------------------------------------------------------

void CDriverVulkan::showCursor(bool b)
{
	if (!_Display || !_XWindow)
		return;

	if (!b)
	{
		if (_BlankCursor != EmptyCursor && _AppliedCursor != _BlankCursor)
		{
			XDefineCursor(_Display, _XWindow, _BlankCursor);
			_AppliedCursor = _BlankCursor;
		}
		_CurrName = "none";
	}
	else
	{
		_CurrName = "";
	}

	// Repaint the current shape. On show this is a no-op until the interface
	// selects a cursor again, exactly like the GL3 driver.
	updateCursor(true);
}

void CDriverVulkan::setMousePos(float x, float y)
{
	if (!_Display || !_XWindow || !_WindowWidth || !_WindowHeight)
		return;

	sint x1 = (sint)((float)_WindowWidth * x);
	sint y1 = (sint)((float)_WindowHeight * (1.0f - y));
	XWarpPointer(_Display, None, _XWindow, None, None, None, None, x1, y1);
}

void CDriverVulkan::setCapture(bool b)
{
	if (!_Display || !_XWindow)
		return;

	if (b)
	{
		XGrabPointer(_Display, _XWindow, True, 0, GrabModeAsync, GrabModeAsync, _XWindow, None, CurrentTime);
		_MouseCaptured = true;
	}
	else
	{
		XUngrabPointer(_Display, CurrentTime);
		_MouseCaptured = false;
	}
}

bool CDriverVulkan::isSystemCursorCaptured()
{
	return _MouseCaptured;
}

// ---------------------------------------------------------------------------
// Cursor registration
// ---------------------------------------------------------------------------

void CDriverVulkan::addCursor(const std::string &name, const NLMISC::CBitmap &cursorBitmap)
{
	if (!isAlphaBlendedCursorSupported()) return;

	nlassert(cursorBitmap.getWidth() != 0);
	nlassert(cursorBitmap.getHeight() != 0);

	// Find the used part from the alpha channel, to avoid shrinking away
	// transparent margins that the hotspot coordinates refer to.
	const CRGBA *pixels = (const CRGBA *)&cursorBitmap.getPixels()[0];
	uint minX, maxX, minY, maxY;
	uint width = cursorBitmap.getWidth();
	uint height = cursorBitmap.getHeight();
	//
	minX = 0;
	for (uint x = 0; x < width; ++x)
	{
		bool stop = false;
		minX = x;
		for (uint y = 0; y < height; ++y)
		{
			if (pixels[x + y * width].A != 0)
			{
				stop = true;
				break;
			}
		}
		if (stop) break;
	}
	//
	maxX = width - 1;
	for (sint x = (sint)width - 1; x >= 0; --x)
	{
		bool stop = false;
		maxX = (uint)x;
		for (uint y = 0; y < height; ++y)
		{
			if (pixels[x + y * width].A != 0)
			{
				stop = true;
				break;
			}
		}
		if (stop) break;
	}
	//
	minY = 0;
	for (uint y = 0; y < height; ++y)
	{
		bool stop = false;
		minY = y;
		for (uint x = 0; x < width; ++x)
		{
			if (pixels[x + y * width].A != 0)
			{
				stop = true;
				break;
			}
		}
		if (stop) break;
	}
	//
	maxY = height - 1;
	for (sint y = (sint)height - 1; y >= 0; --y)
	{
		bool stop = false;
		maxY = (uint)y;
		for (uint x = 0; x < width; ++x)
		{
			if (pixels[x + y * width].A != 0)
			{
				stop = true;
				break;
			}
		}
		if (stop) break;
	}

	CCursor &curs = _Cursors[name];
	curs = CCursor(); // erase a possible previous cursor

	// Blit at the top left corner.
	curs.Src.resize(cursorBitmap.getWidth(), cursorBitmap.getHeight());
	curs.Src.blit(cursorBitmap, minX, minY, maxX - minX + 1, maxY - minY + 1, 0, 0);

	curs.OrigHeight = cursorBitmap.getHeight();
	curs.HotspotOffsetX = minX;
	curs.HotspotOffsetY = minY;
	curs.Dpy = _Display;

	if (name == _CurrName)
		updateCursor();
}

} // NLVULKAN
