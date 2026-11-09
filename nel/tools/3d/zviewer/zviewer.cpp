// NeL - MMORPG Framework <http://dev.ryzom.com/projects/nel/>
// Copyright (C) 2010  Winch Gate Property Limited
//
// This source file has been modified by the following contributors:
// Copyright (C) 2014-2022  Jan BOON (Kaetemi) <jan.boon@kaetemi.be>
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

#include <string>
#include <deque>

#include <nel/misc/types_nl.h>
#include <nel/misc/config_file.h>
#include <nel/misc/debug.h>
#include <nel/misc/common.h>
#include <nel/misc/path.h>
#include <nel/misc/i18n.h>

#include <nel/3d/driver.h>
#include <nel/3d/camera.h>
#include <nel/3d/landscape_model.h>
#include <nel/3d/landscape.h>
#include <nel/3d/decal.h>
#include <nel/3d/transformable.h>
#include <nel/3d/visual_collision_manager.h>
#include <nel/misc/file.h>
#include <nel/3d/text_context.h>
#include <nel/3d/mini_col.h>
#include <nel/3d/nelu.h>
#include <nel/3d/scene_group.h>
#include <nel/3d/texture_file.h>

//#include "nel/net/local_entity.h"

#include "move_listener.h"

// Tempyoyo.
#include <nel/3d/height_map.h>

#ifdef NL_OS_WINDOWS
#	ifndef NL_COMP_MINGW
#		define NOMINMAX
#	endif
#	include <windows.h>
#endif // NL_OS_WINDOWS

using namespace std;
using namespace NLMISC;
using namespace NL3D;


#define BANK_PAH_RELATIVE

#ifndef NL_ZVIEWER_CFG
#define NL_ZVIEWER_CFG "."
#endif // NL_ZVIEWER_CFG

/**
 * CViewerConfig
 */
struct CViewerConfig
{
	bool			Windowed;
	uint			Width;
	uint			Height;
	uint			Depth;
	CVector			Position;
	CVector			Heading;
	CVector			EyesHeight;
	CRGBA			Background;

	// Landscape
	bool			AutoLight;
	CVector			LightDir;
	string			ZonesPath;
	string			BanksPath;
	string			TilesPath;
	bool			UseDDS;
	bool			AllPathRelative;

	string			IgPath;
	string			ShapePath;
	string			MapsPath;
	string			Bank;
	string			FarBank;
	string			FontPath;
	CTextContext	TextContext;
	CFontManager	FontManager;
	float			ZFar;
	float			LandscapeTileNear;
	float			LandscapeThreshold;
	// Camera pin. When CameraLocked is set, the view angles come from these two
	// fields instead of drifting with mouse-move events, which is what makes a
	// headless capture reproducible enough to diff pixel by pixel.
	bool			CameraLocked;
	float			CameraRotZ;
	float			CameraRotX;
	bool			LandscapeNoise;
	// Micro-vegetation (grass/plants): off by default so the established capture
	// baseline stays byte-identical. Enabling it is what exercises the vegetation
	// vertex program (D.2 in AGENTS_trackD.md), which nothing else in this tool
	// reached before.
	bool			Vegetable;
	float			VegetableDensity;
	// The micro-vegetation atlas. The client reads it from the continent data
	// ("MicroVeget", continent.cpp:440) and loads it per season; without it the grass
	// sprites have no texture and the material's alpha test discards them, which looks
	// exactly like "vegetation does not render".
	std::string		VegetableTexture;
	// A projected decal on the landscape. This is the only place in the tree that drives
	// CShadowPolyReceiver::receiveDecal() - the terrain half of the decal system, which is
	// what the client uses for ring decals. nl_sample_decal covers the collision-mesh half.
	bool			Decal;
	float			DecalRadius;
	float			DecalForward; // metres ahead of the camera; the splat lands on the ground
	// which driver NELU should create, and whether it may draw without a window.
	// Gl3 asks for the OpenGL3 (shader) driver; CNELU::initDriver falls back to the
	// legacy one when it is not compiled in, so this stays valid on every platform.
	// Offscreen is what makes the tool drivable from a headless harness (Xvfb/CI).
	bool			Gl3;
	bool			Offscreen;
	// Fog, mirroring the client's own contract at ryzom/client/src/fog_map.cpp:50-51
	// (enableFog(true) then setupFog(start, end, colour), start/end in WORLD UNITS).
	// Off by default so the tool keeps drawing exactly what it drew before.
	bool			Fog;
	float			FogStart;
	float			FogEnd;
	CRGBA			FogColor;
	vector<string>	Zones;
	vector<string>	Igs;

	// HeightField.
	string			HeightFieldName;
	float			HeightFieldMaxZ;
	float			HeightFieldOriginX;
	float			HeightFieldOriginY;
	float			HeightFieldSizeX;
	float			HeightFieldSizeY;

	// StaticLight
	CRGBA			LandAmbient;
	CRGBA			LandDiffuse;

	CViewerConfig()
	{
		Windowed = true;
		Width = 800;
		Height = 600;
		Depth = 32;
		Position = CVector( 1088.987793f, -925.732178f, 0.0f );
		Heading = CVector(0,1,0);
		EyesHeight = CVector(0,0,1.8f);
		Background = CRGBA(100,100,255);
		AutoLight = false;
		LightDir = CVector (1, 0, 0);
		ZonesPath = "./";
		BanksPath = "./";
		TilesPath = "./";
		UseDDS = false;
		AllPathRelative = false;
		IgPath = "./";
		ShapePath = "./";
		MapsPath = "./";
		Bank = "bank.bank";
		FontPath = "\\\\server\\code\\fonts\\arialuni.ttf";
		ZFar = 1000;
		LandscapeTileNear = 50.0f;
		LandscapeThreshold = 0.001f;
		CameraLocked = false;
		CameraRotZ = 0.0f;
		CameraRotX = 0.0f;
		LandscapeNoise = true;
		Vegetable = false;
		VegetableDensity = 1.0f;
		VegetableTexture = "microveget_fyros_wi.dds";
		Decal = false;
		DecalRadius = 25.0f;
		DecalForward = 40.0f;
		Gl3 = false;
		Offscreen = false;
		Fog = false;
		FogStart = 0.0f;
		FogEnd = 100.0f;
		FogColor = CRGBA(154, 172, 224, 255);

		HeightFieldMaxZ= 100;
		HeightFieldOriginX= 16000;
		HeightFieldOriginY= -24000;
		HeightFieldSizeX= 160;
		HeightFieldSizeY= 160;

		CRGBA diffuse (241, 226, 244);
		CRGBA ambiant  (17, 54, 100);
		LandDiffuse= diffuse;
		LandAmbient= ambiant;

	}
};

CViewerConfig			ViewerCfg;



CLandscapeModel			*Landscape = nullptr;
CMoveListener			MoveListener;
CMiniCol				CollisionManager;











/*******************************************************************\
						getZoneNameByCoord()
\*******************************************************************/
string getZoneNameByCoord(float x, float y)
{
	const float zoneDim = 160.0f;

	float xcount = x/zoneDim;
	float ycount = -y/zoneDim + 1;

	string zoneName;
	char ych[32];
	sprintf(ych,"%d",(sint)ycount);
	sint sz = (sint)strlen(ych);
	for(sint i = 0; i<sz; i++)
	{
		zoneName += ych[i];
	}
	zoneName += '_';
	zoneName += 'A' + (sint)xcount/26;
	zoneName += 'A' + (sint)xcount%26;

	return zoneName;
}




/*********************************************************\
					displayOrientation()
\*********************************************************/
void displayOrientation()
{
	float x = 0.9f*4.f/3.f;
	float y = 0.1f;
	float radius = 0.015f;

	// Triangle
	CMaterial mat;
	mat.initUnlit();
	mat.setSrcBlend(CMaterial::srcalpha);
	mat.setDstBlend(CMaterial::invsrcalpha);
	mat.setBlend(true);
	
	CVertexBuffer vb;
	vb.setVertexFormat (CVertexBuffer::PositionFlag);
	vb.setNumVertices (7);
	{
		CVertexBufferReadWrite vba;
		vb.lock(vba);
		
		// tri
		vba.setVertexCoord (0, CVector (-radius, 0, 0));
		vba.setVertexCoord (1, CVector (radius, 0, 0));
		vba.setVertexCoord (2, CVector (0, 0, 3*radius));

		// quad
		vba.setVertexCoord (3, CVector (-radius, 0, -radius));
		vba.setVertexCoord (4, CVector (radius, 0, -radius));
		vba.setVertexCoord (5, CVector (radius, 0, radius));
		vba.setVertexCoord (6, CVector (-radius, 0, radius));
	}
	
	CNELU::Driver->activeVertexBuffer(vb);

	CIndexBuffer pbTri;
	pbTri.setNumIndexes (3);
	{
		CIndexBufferReadWrite iba;
		pbTri.lock (iba);
		iba.setTri (0, 0, 1, 2);
	}
	
	CIndexBuffer pbQuad;
	pbQuad.setNumIndexes (6);
	{
		CIndexBufferReadWrite iba;
		pbQuad.lock(iba);
		iba.setTri (0, 3, 4, 5);
		iba.setTri (3, 5, 6, 3);
	}
	
	CNELU::Driver->setFrustum (0.f, 4.f/3.f, 0.f, 1.f, -1.f, 1.f, false);
	CMatrix mtx;
	mtx.identity();
	CNELU::Driver->setupViewMatrix (mtx);

	mat.setColor(CRGBA(50,255,255,150));

	// up
	mtx.identity();
	mtx.translate(CVector(x,0,y));
	mtx.rotateY(MoveListener.getRotZ() );
	mtx.translate(CVector(0,0,radius));
	CNELU::Driver->setupModelMatrix (mtx);
	CNELU::Driver->activeVertexBuffer(vb);
	CNELU::Driver->activeIndexBuffer(pbTri);
	CNELU::Driver->renderTriangles(mat, 0, pbTri.getNumIndexes()/3);

	mat.setColor(CRGBA(50,50,255,150));

	// down
	mtx.identity();
	mtx.translate(CVector(x,0,y));
	mtx.rotateY(MoveListener.getRotZ() + (float)Pi);
	mtx.translate(CVector(0,0,radius));
	CNELU::Driver->setupModelMatrix (mtx);
	CNELU::Driver->renderTriangles(mat, 0, pbTri.getNumIndexes()/3);

	// left
	mtx.identity();
	mtx.translate(CVector(x,0,y));
	mtx.rotateY(MoveListener.getRotZ() - (float)Pi/2);
	mtx.translate(CVector(0,0,radius));
	CNELU::Driver->setupModelMatrix (mtx);
	CNELU::Driver->renderTriangles(mat, 0, pbTri.getNumIndexes()/3);

	// right
	mtx.identity();
	mtx.translate(CVector(x,0,y));
	mtx.rotateY(MoveListener.getRotZ() + (float)Pi/2);
	mtx.translate(CVector(0,0,radius));
	CNELU::Driver->setupModelMatrix (mtx);
	CNELU::Driver->renderTriangles(mat, 0, pbTri.getNumIndexes()/3);
	
	// center
	mtx.identity();
	mtx.translate(CVector(x,0,y));
	mtx.rotateY(MoveListener.getRotZ());
	CNELU::Driver->setupModelMatrix (mtx);
	CNELU::Driver->activeIndexBuffer(pbQuad);
	CNELU::Driver->renderTriangles(mat, 0, pbQuad.getNumIndexes()/3);	
}





/*********************************************************\
					displayZone()
\*********************************************************/
// Set the first time the vegetation has been applied in the render loop (see the call
// site: it has to happen after the landscape has a driver, not at setup time).
static bool s_VegetableApplied = false;

// Ditto for the decal: the landscape must have tessellated at least once, because the decal
// collects its receiver triangles from the faces that are already registered in the
// landscape's CShadowPolyReceiver (patch.cpp:906, appendToShadowPolyReceiver).
static bool s_DecalApplied = false;
static CDecal *s_Decal = NULL;
static uint32 s_DecalFrames = 0;

// Generated once, like the sample's: a 128x128 RGBA ring, written next to the viewer and
// removed on exit. Kept out of the repo and out of the data tree.
static const char *ZVIEWER_DECAL_TEXTURE = "zviewer_decal.tga";

static void buildViewerDecalTexture(NLMISC::CBitmap &bmp)
{
	const uint32 size = 128;
	bmp.resize(size, size, NLMISC::CBitmap::RGBA);
	uint8 *px = bmp.getPixels().getPtr();
	const float center = (float)size * 0.5f;
	const float radius = (float)size * 0.44f;
	for (uint32 y = 0; y < size; ++y)
	{
		for (uint32 x = 0; x < size; ++x)
		{
			const float dx = (float)x + 0.5f - center;
			const float dy = (float)y + 0.5f - center;
			const float d = sqrtf(dx * dx + dy * dy);
			uint8 *p = px + ((size_t)y * size + x) * 4;
			if (d > radius)
			{
				p[0] = p[1] = p[2] = p[3] = 0;
				continue;
			}
			float alpha = 1.f;
			if (d > radius - 2.f)
				alpha = (radius - d) * 0.5f;
			uint8 r = 240, g = 200, b = 40; // yellow ring, so it reads against green terrain
			if (d < (float)size * 0.34f)
			{
				r = 200; g = 120; b = 20;
				if (d < (float)size * 0.10f)
				{
					r = 255; g = 255; b = 240;
				}
			}
			p[0] = r; p[1] = g; p[2] = b;
			p[3] = (uint8)(alpha * 255.f);
		}
	}
}

void displayZones()
{
	const float zFarStep = 5.0f;
	const float	tileNearStep = 10.0f;
	const float thresholdStep = 0.005f;

	ViewerCfg.TextContext.setHotSpot(CComputedString::MiddleMiddle);
	ViewerCfg.TextContext.setColor(CRGBA(255,255,255));
	ViewerCfg.TextContext.setFontSize(20);
	
	CNELU::clearBuffers(CRGBA(0,0,0));
	CNELU::swapBuffers();



	// Create landscape
	CNELU::clearBuffers(CRGBA(0,0,0));
	ViewerCfg.TextContext.printfAt(0.5f,0.5f,"Creating landscape...");
	CNELU::swapBuffers();
	
	Landscape = (CLandscapeModel*)CNELU::Scene->createModel(LandscapeModelId);
	Landscape->Landscape.setNoiseMode (ViewerCfg.LandscapeNoise);
	Landscape->Landscape.setTileNear(ViewerCfg.LandscapeTileNear);
	Landscape->Landscape.setThreshold(ViewerCfg.LandscapeThreshold);

	Landscape->Landscape.enableAutomaticLighting (ViewerCfg.AutoLight);
	Landscape->Landscape.setupAutomaticLightDir (ViewerCfg.LightDir);

	// Enable Additive Tiles.
	Landscape->enableAdditive(true);

	// HeightField.
	CBitmap		heightBitmap;
	CIFile file(ViewerCfg.HeightFieldName);

	if( !ViewerCfg.HeightFieldName.empty() && heightBitmap.load(file) )
	{
		CHeightMap	heightMap;
		heightMap.buildFromBitmap(heightBitmap);
		heightMap.MaxZ= ViewerCfg.HeightFieldMaxZ;
		heightMap.OriginX= ViewerCfg.HeightFieldOriginX;
		heightMap.OriginY= ViewerCfg.HeightFieldOriginY;
		heightMap.SizeX = ViewerCfg.HeightFieldSizeX;
		heightMap.SizeY = ViewerCfg.HeightFieldSizeY;
		Landscape->Landscape.setHeightField(heightMap);
	}

	
	// Init TileBank.
	CNELU::clearBuffers(CRGBA(0,0,0));
	ViewerCfg.TextContext.printfAt(0.5f,0.5f,"Initializing TileBanks...");
	CNELU::swapBuffers();

	try 
	{
		CIFile bankFile (ViewerCfg.BanksPath + "/" + ViewerCfg.Bank);
		Landscape->Landscape.TileBank.serial(bankFile);
	}
	catch(const Exception &)
	{
		string tmp = string("Cant load bankfile ")+ViewerCfg.BanksPath + "/" + ViewerCfg.Bank;
		nlerror ("%s", tmp.c_str());
	}

	if (!ViewerCfg.TilesPath.empty())
		Landscape->Landscape.TileBank.setAbsPath (ViewerCfg.TilesPath + "/");

	if (ViewerCfg.UseDDS)
	{
		Landscape->Landscape.TileBank.makeAllExtensionDDS();
	}

	if (ViewerCfg.AllPathRelative)
		Landscape->Landscape.TileBank.makeAllPathRelative();

	string::size_type idx = ViewerCfg.Bank.find(".");
	string farBank = ViewerCfg.Bank.substr(0,idx);
	farBank += "_su.farbank";
	if (!ViewerCfg.FarBank.empty())
	{
		farBank = ViewerCfg.FarBank;
	}

	try
	{
		CIFile farbankFile(ViewerCfg.BanksPath + "/" + farBank);
		Landscape->Landscape.TileFarBank.serial(farbankFile);
	}
	catch(const Exception &)
	{
		string tmp = string("Cant load bankfile ")+ViewerCfg.BanksPath + "/" + farBank;
		nlerror ("%s", tmp.c_str());
	}
	
	if ( ! Landscape->Landscape.initTileBanks() )
	{
		nlwarning( "You need to recompute bank.farbank for the far textures" );
	}
	string seasonPostfix = farBank.substr(idx,3);
	Landscape->Landscape.TileBank.postfixTileFilename (seasonPostfix.c_str());
	Landscape->Landscape.TileBank.postfixTileVegetableDesc (seasonPostfix.c_str());

	// The vegetable descriptors are NOT inside the bank: each tile set stores the *name* of a
	// .vegetset file (CTileSet::_TileVegetableDescFileName) and the lists are loaded from it
	// separately. The client does this from CLandscapeUser::flushTiles()
	// (landscape_user.cpp:101, "must load the vegetables descriptor" before initTileBanks),
	// and zviewer never did - which is why enabling vegetables here instantiated nothing:
	// active was 1 but every tile reported an empty vegetable list and no vegetation vertex
	// program was ever compiled.
	Landscape->Landscape.TileBank.loadTileVegetableDescs();

	// Init light color
	CNELU::clearBuffers(CRGBA(0,0,0));
	ViewerCfg.TextContext.printfAt(0.5f,0.5f,"Initializing Light...");
	CNELU::swapBuffers();
	
	Landscape->Landscape.setupStaticLight (ViewerCfg.LandDiffuse, ViewerCfg.LandAmbient, 1.1f);

	// Init collision manager
	CollisionManager.init( &(Landscape->Landscape), 200);
	

	// Preload of TileBank
	CNELU::clearBuffers(CRGBA(0,0,0));
	ViewerCfg.TextContext.printfAt(0.5f,0.5f,"Loading TileBank...");
	CNELU::swapBuffers();
	
	for (int ts=0; ts<Landscape->Landscape.TileBank.getTileSetCount (); ts++)
	{
		CTileSet *tileSet=Landscape->Landscape.TileBank.getTileSet (ts);
		sint tl;
		for (tl=0; tl<tileSet->getNumTile128(); tl++)
			Landscape->Landscape.flushTiles (CNELU::Scene->getDriver(), (uint16)tileSet->getTile128(tl), 1);
		for (tl=0; tl<tileSet->getNumTile256(); tl++)
			Landscape->Landscape.flushTiles (CNELU::Scene->getDriver(), (uint16)tileSet->getTile256(tl), 1);
		for (tl=0; tl<CTileSet::count; tl++)
			Landscape->Landscape.flushTiles (CNELU::Scene->getDriver(), (uint16)tileSet->getTransition(tl)->getTile (), 1);
	}
		

	// Build zones.
	CNELU::clearBuffers(CRGBA(0,0,0));
	ViewerCfg.TextContext.printfAt(0.5f,0.5f,"Loading zones...");
	CNELU::swapBuffers();
	uint32 i;
	for(i =0; i<ViewerCfg.Zones.size(); i++)
	{
		CZone zone;
		try
		{
			CIFile file(CPath::lookup(ViewerCfg.ZonesPath + "/" + ViewerCfg.Zones[i]));
			zone.serial(file);
			file.close();

			// Add it to landscape.
			Landscape->Landscape.addZone(zone);

			// Add it to collision manager.
			CollisionManager.addZone(zone.getZoneId());
		}
		catch(const Exception &e)
		{
			printf("%s\n", e.what ());
		}		
	}

	// Load instance group.
	CNELU::clearBuffers(CRGBA(0,0,0));
	ViewerCfg.TextContext.printfAt(0.5f,0.5f,"Loading objects...");
	CNELU::swapBuffers();
	for(i =0; i<ViewerCfg.Igs.size(); i++)
	{
		CInstanceGroup *group = new CInstanceGroup;
		try
		{
			CIFile file(CPath::lookup(ViewerCfg.Igs[i]));
			group->serial(file);
			file.close();

			// Add it to the scene.
			group->addToScene (*CNELU::Scene);
		}
		catch(const Exception &e)
		{
			printf("%s\n", e.what ());
		}		
	}
	
	// Init collision Manager.
	CNELU::clearBuffers(CRGBA(0,0,0));
	ViewerCfg.TextContext.printfAt(0.5f,0.5f,"Initializing collision manager...");
	CNELU::swapBuffers();
	
	
	CollisionManager.setCenter(ViewerCfg.Position);
	ViewerCfg.Position.z = 0.0f;
	CollisionManager.snapToGround( ViewerCfg.Position, 1000.0f );

	

	// hide mouse cursor
	CNELU::Driver->showCursor(false);
#ifdef NL_RELEASE
	CNELU::Driver->setCapture(true);
#endif

	
		
	// Events management
	CNELU::EventServer.addEmitter(CNELU::Driver->getEventEmitter());
	CNELU::AsyncListener.addToServer(CNELU::EventServer);
	
	MoveListener.init(CNELU::Scene, ViewerCfg.Width, ViewerCfg.Height, *CNELU::Camera);
	MoveListener.addToServer(CNELU::EventServer);
	MoveListener.setPos( ViewerCfg.Position );	

	// The pin has to be applied after init(): operator() would otherwise rotate the
	// view again from the next mouse-move event.
	if (ViewerCfg.CameraLocked)
	{
		MoveListener.setRotZ(ViewerCfg.CameraRotZ * Pi / 180.0f);
		MoveListener.setRotX(ViewerCfg.CameraRotX * Pi / 180.0f);
		MoveListener.lockRotation(true);
	}

	CNELU::Camera->setPerspective (float(80.0*Pi/180.0), 1.33f, 0.1f, 1000.0f);
	
	bool showInfos = true;


	// initializing Z-Clip Far
	float left;
	float right;
	float bottom;
	float top; 
	float znear;
	float zfar;
	CNELU::Camera->getFrustum(left, right, bottom, top, znear, zfar);
	zfar = ViewerCfg.ZFar;
	CNELU::Camera->setFrustum(left, right, bottom, top, znear, zfar);
	
	
	// Time mgt.
	//==========
	sint64 ts(0);
	do
	{

		MoveListener.setLocalTime(CTime::getLocalTime());
		sint64 dt64 = MoveListener.deltaTimeInMilliseconds();
		ts += dt64;
		float	dt(MoveListener.deltaTimeInSeconds());

	
		CNELU::EventServer.pump();

		
		// Manage movement and collision
		CVector oldpos = MoveListener.getPos();
		MoveListener.changeViewMatrix();
		if(MoveListener.getMode()==CMoveListener::WALK)
		{
			CVector pos = MoveListener.getPos();
			CollisionManager.snapToGround( pos , 1000.0f );
			MoveListener.setPos( pos );
		}
		CollisionManager.setCenter(MoveListener.getPos()); 

				
		// Change move mode
		if(CNELU::AsyncListener.isKeyPushed(KeySPACE))
		{
			MoveListener.swapMode();	
		}
		
		
		// Change displaying infos state
		if(CNELU::AsyncListener.isKeyPushed(KeyF1))
		{
			showInfos = !showInfos;
		}


		// Change eyes height
		float eh = MoveListener.getEyesHeight();
		if(CNELU::AsyncListener.isKeyPushed(KeyADD))
		{
			ViewerCfg.EyesHeight.z += 0.1f;
			eh += 0.1f;
		}
		if(CNELU::AsyncListener.isKeyPushed(KeySUBTRACT))
		{
			ViewerCfg.EyesHeight.z -= 0.1f;
			eh -= 0.1f;
		}
		if(ViewerCfg.EyesHeight.z<0.1f) ViewerCfg.EyesHeight.z = 0.1f;
		if(eh<0.1f) eh = 0.1f;
		MoveListener.setEyesHeight(eh);


		// Change TileNear
		float tileNear = Landscape->Landscape.getTileNear();
		if(CNELU::AsyncListener.isKeyPushed(KeyHOME))
			tileNear += tileNearStep;
		if(CNELU::AsyncListener.isKeyPushed(KeyEND))
			tileNear -= tileNearStep;
		if(tileNear<0) tileNear = 0;
		Landscape->Landscape.setTileNear(tileNear);


		// Change Z-Far
		CNELU::Camera->getFrustum(left, right, bottom, top, znear, zfar);
		if(CNELU::AsyncListener.isKeyDown(KeyPRIOR))
			zfar += zFarStep;
		if(CNELU::AsyncListener.isKeyDown(KeyNEXT))
			zfar -= zFarStep;
		if(zfar<0) zfar = 0;
		CNELU::Camera->setFrustum(left, right, bottom, top, znear, zfar);


		// Change Threshold
		float threshold = Landscape->Landscape.getThreshold();
		if(CNELU::AsyncListener.isKeyPushed(KeyINSERT))
			threshold += thresholdStep;
		if(CNELU::AsyncListener.isKeyPushed(KeyDELETE))
			threshold -= thresholdStep;
		if(threshold<0.001f) threshold = 0.001f;
		if(threshold>0.1f) threshold = 0.1f;
		Landscape->Landscape.setThreshold(threshold);


		// Switch between wired and filled scene display
		if(CNELU::AsyncListener.isKeyPushed(KeyF3))
		{
			if (CNELU::Driver->getPolygonMode ()==IDriver::Filled)
				CNELU::Driver->setPolygonMode (IDriver::Line);
			else
				CNELU::Driver->setPolygonMode (IDriver::Filled);
		}

		
		// Switch between mouse move and keyboard-only move
		if(CNELU::AsyncListener.isKeyPushed(KeyRETURN))
		{
			MoveListener.changeControlMode();
		}
		

		
		// Render
		//=======
		CNELU::clearBuffers(ViewerCfg.Background);
		CNELU::Driver->clearZBuffer();
		CNELU::Scene->render();

		// Micro-vegetation, applied after the FIRST render on purpose.
		// CLandscape::setDriver() only runs from CLandscapeModel::clip() during the first
		// traversal (landscape_model.cpp:179), and CLandscape::isVegetableActive() is
		// _VegetableManagerEnabled && _DriverOkForVegetable. Calling enableVegetable()
		// before that point leaves _DriverOkForVegetable false, so it computes no
		// vegetable IG, never retries, and the capture silently shows nothing. Same four
		// calls the client makes (ryzom/client/src/continent_manager.cpp:238-245).
		// One projected decal on the terrain, created after the first render so that the
		// landscape has tessellated and its faces are registered in the shadow poly receiver.
		if (ViewerCfg.Decal && !s_DecalApplied)
		{
			s_DecalApplied = true;

			// No CVisualCollisionManager: this decal's receiver is the terrain, which the
			// landscape's shadow poly receiver supplies. computeDecal() used to demand a
			// collision manager anyway (and with an empty one the result was identical), so
			// this is the landscape-only configuration the check used to forbid.

			NLMISC::CBitmap bmp;
			buildViewerDecalTexture(bmp);
			std::string texPath = NLMISC::CPath::getCurrentPath();
			if (texPath.empty() || texPath[texPath.size() - 1] != '/')
				texPath += '/';
			texPath += ZVIEWER_DECAL_TEXTURE;
			{
				NLMISC::COFile out;
				const bool wrote = out.open(texPath) && bmp.writeTGA(out, 32);
				out.close();
				if (!wrote)
					nlwarning("zviewer: could not write '%s'; no decal", texPath.c_str());
			}

			CDecal *decal = dynamic_cast<CDecal *>(CNELU::Scene->createModel(DecalId));
			if (decal != NULL)
			{
				const CVector pos = MoveListener.getPos();
				decal->setTransformMode(ITransformable::DirectMatrix);
				decal->setTexture(texPath);

				// UDecal::setWorldMatrixForSpot() hardcodes z = 0 (decal.cpp: setPos(x-r, y-r, 0)),
				// and the world bounding box it derives is what selects receiver triangles from
				// the landscape's grid. On terrain at z = 420 that box covers nothing, so the
				// decal silently collects 0 vertices. Build the same matrix at the ground
				// height instead: the four side clip planes are vertical, so only the bbox
				// needs the right z.
				const float groundZ = pos.z - ViewerCfg.EyesHeight.z;

				// Ahead of the camera, not under it: a decal centred on the camera's own XY sits
				// at the bottom edge of the view (or below it) and looks like "nothing drawn".
				// Forward comes from the same rotation the view matrix uses
				// (move_listener.cpp:163-167: rotZ then rotX, camera looks along +Y).
				CMatrix camRot;
				camRot.identity();
				camRot.rotateZ(MoveListener.getRotZ());
				camRot.rotateX(MoveListener.getRotX());
				CVector forward = camRot * CVector(0.f, 1.f, 0.f);
				forward.normalize();

				const CVector spot = pos + forward * ViewerCfg.DecalForward;
				CMatrix decalMat;
				decalMat.setScale(2.f * ViewerCfg.DecalRadius);
				decalMat.setPos(CVector(spot.x - ViewerCfg.DecalRadius, spot.y - ViewerCfg.DecalRadius, groundZ));
				decal->setMatrix(decalMat);
				// NOT static: the terrain LOD changes as the camera moves, so the receiver
				// triangles change and the clipped geometry has to be recomputed.
				decal->setStatic(false);
				decal->setDiffuse(CRGBA(255, 255, 255, 255));
				s_Decal = decal;
				nlinfo("ZV: decal at (%.1f, %.1f) %.0f m ahead, radius %.1f m",
					spot.x, spot.y, ViewerCfg.DecalForward, ViewerCfg.DecalRadius);
			}
			else
			{
				nlwarning("zviewer: createModel(DecalId) failed");
			}
		}

		// Report the clipped geometry a few frames in. getVertices() returns an empty set on
		// its first call on purpose (CDecal's "first-frame skip", decal.cpp:220), so probing
		// it at creation time always reads 0 and says nothing about whether it works.
		if (s_Decal != NULL && s_DecalFrames < 6)
		{
			++s_DecalFrames;
			if (s_DecalFrames == 6)
			{
				const std::vector<CVector> &verts = s_Decal->getVertices(false);
				nlinfo("ZV: decal receiver geometry: %u vertices, %u triangles",
					(uint)verts.size(), (uint)(verts.size() / 3));
			}
		}

		if (ViewerCfg.Vegetable && !s_VegetableApplied)
		{
			s_VegetableApplied = true;
			Landscape->Landscape.setVegetableWind(CVector(0.5f, 0.5f, 0.0f).normed(), 0.5f, 1.0f, 0.0f);
			Landscape->Landscape.setVegetableUpdateLightingFrequency(1.0f / 20.0f);
			Landscape->Landscape.setVegetableDensity(ViewerCfg.VegetableDensity);
			// This is NOT optional in practice. Without the atlas the grass sprites have no
			// texture, the material's alpha test discards every fragment, and the result looks
			// exactly like "vegetation never renders" - the CLandscapeModel gate says active,
			// the nelvp vegetation programs compile and get drawn, and no pixel changes.
			if (!ViewerCfg.VegetableTexture.empty())
				Landscape->Landscape.loadVegetableTexture(ViewerCfg.VegetableTexture);
			Landscape->Landscape.enableVegetable(true);
			nlinfo("ZV: vegetable enabled after first render: active=%d density=%.2f faces=%u",
				(int)Landscape->Landscape.isVegetableActive(), ViewerCfg.VegetableDensity,
				Landscape->Landscape.getNumVegetableFaceRendered());
		}

				
		if(showInfos)
		{
			
			// black top quad
			CDRU::drawQuad(0,0.97f,1.0f,1.0f,*CNELU::Driver,CRGBA(0,0,0),CNELU::Scene->getViewport());

			// black bottom quad
			CDRU::drawQuad(0,0,1.0f,0.03f,*CNELU::Driver,CRGBA(0,0,0),CNELU::Scene->getViewport());

			
			ViewerCfg.TextContext.setFontSize(12);
			ViewerCfg.TextContext.setColor(CRGBA(255,255,255));

			// Display fps.
			ViewerCfg.TextContext.printfAt(0.05f,0.98f,"%.1f fps",1/dt);

			// Display ms
			ViewerCfg.TextContext.printfAt(0.12f,0.98f,"%d ms",dt64);

			// Display Tile Near
			ViewerCfg.TextContext.printfAt(0.75f,0.98f,"Tile Near : %.1f",tileNear);

			//Display moving mode
			ViewerCfg.TextContext.setColor(CRGBA(255,0,0));
			switch(MoveListener.getMode())
			{
				case CMoveListener::WALK :
					ViewerCfg.TextContext.printfAt(0.5f,0.98f,"Walk Mode");
					break;
				case CMoveListener::FREE :
					ViewerCfg.TextContext.printfAt(0.5f,0.98f,"Free-Look Mode");
					break;
				default:
					break;
			}
			ViewerCfg.TextContext.setColor(CRGBA(255,255,255));

			// Display Threshold
			ViewerCfg.TextContext.printfAt(0.3f,0.98f,"Threshold : %.3f",threshold);

			// Display Clip Far
			ViewerCfg.TextContext.printfAt(0.92f,0.98f,"Clip Far : %.1f",zfar);

			
			ViewerCfg.TextContext.setHotSpot(CComputedString::MiddleBottom);
			
			// Display current zone name
			CVector pos = MoveListener.getPos();
			string zoneName = getZoneNameByCoord(pos.x, pos.y);
			ViewerCfg.TextContext.printfAt(0.3f,0.01f,"Zone : %s",zoneName.c_str());

			// Position
			ViewerCfg.TextContext.printfAt(0.1f,0.01f,"Position : %.2f %.2f %.2f", pos.x, pos.y, pos.z);

			// Eyes height
			ViewerCfg.TextContext.printfAt(0.7f,0.01f,"Eyes : %.2f m",ViewerCfg.EyesHeight.z);

			// Display speed in km/h
			ViewerCfg.TextContext.setColor(CRGBA(255,0,0));
			ViewerCfg.TextContext.printfAt(0.5f,0.01f,"Speed : %d km/h",(sint)(MoveListener.getSpeed()*3.6f));
			ViewerCfg.TextContext.setColor(CRGBA(255,255,255));

			// Heading
			sint heading = -(sint)(MoveListener.getRotZ()*180/Pi)%360;
			if(heading<0) heading += 360;
			ViewerCfg.TextContext.printfAt(0.9f,0.01f,"Heading : %d degrees",heading);

			// Display the cool compass.
			displayOrientation();
		}

		
		CNELU::swapBuffers();
		CNELU::screenshot();
	}
	while(!CNELU::AsyncListener.isKeyPushed(KeyESCAPE));





	ViewerCfg.Position = MoveListener.getPos();
	
	CNELU::AsyncListener.removeFromServer(CNELU::EventServer);
	MoveListener.removeFromServer(CNELU::EventServer);

	CNELU::Driver->showCursor(true);
#ifdef NL_RELEASE
	CNELU::Driver->setCapture(false);
#endif
}




/****************************************************************\
							writeConfigFile
\****************************************************************/
void writeConfigFile(const char * configFileName)
{
	FILE * f = nlfopen(configFileName, "wt");

	if(f == nullptr)
	{
		nlerror("can't open file '%s'\n",configFileName);
	}

	fprintf(f,"FullScreen = %d;\n",ViewerCfg.Windowed?0:1);
	fprintf(f,"Width = %d;\n",ViewerCfg.Width);
	fprintf(f,"Height = %d;\n",ViewerCfg.Height);
	fprintf(f,"Depth = %d;\n",ViewerCfg.Depth);
	fprintf(f,"Position = { %f, %f, %f };\n", ViewerCfg.Position.x,ViewerCfg.Position.y,ViewerCfg.Position.z);
	fprintf(f,"EyesHeight = %f;\n", ViewerCfg.EyesHeight.z);
	fprintf(f,"Background = { %d, %d, %d };\n", ViewerCfg.Background.R,ViewerCfg.Background.G,ViewerCfg.Background.B);
	fprintf(f,"ZFar = %f;\n", ViewerCfg.ZFar);

	fprintf(f,"Gl3 = %d;\n", ViewerCfg.Gl3?1:0);
	fprintf(f,"Offscreen = %d;\n", ViewerCfg.Offscreen?1:0);
	fprintf(f,"Fog = %d;\n", ViewerCfg.Fog?1:0);
	fprintf(f,"FogStart = %f;\n", ViewerCfg.FogStart);
	fprintf(f,"FogEnd = %f;\n", ViewerCfg.FogEnd);
	fprintf(f,"FogColor = \"%d,%d,%d\";\n", (int)ViewerCfg.FogColor.R, (int)ViewerCfg.FogColor.G, (int)ViewerCfg.FogColor.B);
	fprintf(f,"AutoLight = %d;\n", ViewerCfg.AutoLight?1:0);
	fprintf(f,"LightDir = { %f, %f, %f };\n", ViewerCfg.LightDir.x, ViewerCfg.LightDir.y, ViewerCfg.LightDir.z);
	fprintf(f,"LandscapeTileNear = %f;\n", ViewerCfg.LandscapeTileNear);
	fprintf(f,"LandscapeThreshold = %f;\n", ViewerCfg.LandscapeThreshold);
	fprintf(f,"LandscapeNoise = %d;\n", (int)ViewerCfg.LandscapeNoise);
	fprintf(f,"Vegetable = %d;\n", ViewerCfg.Vegetable ? 1 : 0);
	fprintf(f,"VegetableDensity = %f;\n", ViewerCfg.VegetableDensity);
	fprintf(f,"VegetableTexture = \"%s\";\n", ViewerCfg.VegetableTexture.c_str());
	fprintf(f,"Decal = %d;\n", ViewerCfg.Decal ? 1 : 0);
	fprintf(f,"DecalRadius = %f;\n", ViewerCfg.DecalRadius);
	fprintf(f,"DecalForward = %f;\n", ViewerCfg.DecalForward);
	fprintf(f,"CameraLocked = %d;\n", ViewerCfg.CameraLocked ? 1 : 0);
	fprintf(f,"CameraRotZ = %f;\n", ViewerCfg.CameraRotZ);
	fprintf(f,"CameraRotX = %f;\n", ViewerCfg.CameraRotX);
	fprintf(f,"BanksPath = \"%s\";\n",ViewerCfg.BanksPath.c_str());
	fprintf(f,"TilesPath = \"%s\";\n",ViewerCfg.TilesPath.c_str());
	fprintf(f,"UseDDS = \"%d\";\n",ViewerCfg.UseDDS?1:0);
	fprintf(f,"AllPathRelative = \"%d\";\n",ViewerCfg.AllPathRelative?1:0);
	fprintf(f,"Bank = \"%s\";\n",ViewerCfg.Bank.c_str());
	// FarBank was read back with getVar() but never written, so a zviewer run with no
	// cfg on disk wrote a cfg that the next run rejected (EUnknownVar -> nlerror -> abort).
	fprintf(f,"FarBank = \"%s\";\n",ViewerCfg.FarBank.c_str());
	fprintf(f,"ZonesPath = \"%s\";\n",ViewerCfg.ZonesPath.c_str());
	fprintf(f,"IgPath = \"%s\";\n",ViewerCfg.IgPath.c_str());
	fprintf(f,"ShapePath = \"%s\";\n",ViewerCfg.ShapePath.c_str());
	fprintf(f,"MapsPath = \"%s\";\n",ViewerCfg.MapsPath.c_str());
	fprintf(f,"FontPath = \"%s\";\n",ViewerCfg.FontPath.c_str());

	fprintf(f,"HeightFieldName = \"%s\";\n", ViewerCfg.HeightFieldName.c_str());
	fprintf(f,"HeightFieldMaxZ = %f;\n", ViewerCfg.HeightFieldMaxZ);
	fprintf(f,"HeightFieldOriginX = %f;\n", ViewerCfg.HeightFieldOriginX);
	fprintf(f,"HeightFieldOriginY = %f;\n", ViewerCfg.HeightFieldOriginY);
	fprintf(f,"HeightFieldSizeX = %f;\n", ViewerCfg.HeightFieldSizeX);
	fprintf(f,"HeightFieldSizeY = %f;\n", ViewerCfg.HeightFieldSizeY);

	fprintf(f,"LandAmbient = { %d, %d, %d };\n", ViewerCfg.LandAmbient.R,ViewerCfg.LandAmbient.G,ViewerCfg.LandAmbient.B);
	fprintf(f,"LandDiffuse = { %d, %d, %d };\n", ViewerCfg.LandDiffuse.R,ViewerCfg.LandDiffuse.G,ViewerCfg.LandDiffuse.B);

	fprintf(f,"Zones = {\n");
	fprintf(f,"};\n");

	fprintf(f,"Ig = {\n");
	fprintf(f,"};\n");

	fclose(f);
}



/****************************************************************\
						init()
\****************************************************************/
void initViewerConfig(const char * configFileName)
{
	FILE *f = nlfopen(configFileName, "rt");
	if(f == nullptr)
	{
		nlwarning("'%s' not found, default values used", configFileName);
		writeConfigFile(configFileName);
	}
	else fclose (f);
	
	try
	{
		CConfigFile cf;
	
		cf.load(configFileName);
	
		CConfigFile::CVar &cvFullScreen = cf.getVar("FullScreen");
		ViewerCfg.Windowed = cvFullScreen.asInt() ? false : true;
		
		CConfigFile::CVar &cvWidth = cf.getVar("Width");
		ViewerCfg.Width = cvWidth.asInt();

		CConfigFile::CVar &cvHeight = cf.getVar("Height");
		ViewerCfg.Height = cvHeight.asInt();

		CConfigFile::CVar &cvDepth = cf.getVar("Depth");
		ViewerCfg.Depth = cvDepth.asInt();

		CConfigFile::CVar &cvPosition = cf.getVar("Position");
		nlassert(cvPosition.size()==3);
		ViewerCfg.Position.x = cvPosition.asFloat(0);
		ViewerCfg.Position.y = cvPosition.asFloat(1);
		ViewerCfg.Position.z = cvPosition.asFloat(2);

		CConfigFile::CVar &cvEyesHeight = cf.getVar("EyesHeight");
		ViewerCfg.EyesHeight = CVector(0,0,cvEyesHeight.asFloat());

		// Optional, unlike the entries above: getVar() throws EUnknownVar (config_file.cpp:562),
		// so a zviewer.cfg written before these knobs existed - including the one shipped in this
		// directory, and every artist's copy on disk - must keep loading. Absent means the legacy
		// driver and a real window, i.e. exactly the previous behaviour.
		CConfigFile::CVar *cvGl3 = cf.getVarPtr("Gl3");
		if (cvGl3 != nullptr)
			ViewerCfg.Gl3 = cvGl3->asInt() ? true : false;

		CConfigFile::CVar *cvOffscreen = cf.getVarPtr("Offscreen");
		if (cvOffscreen != nullptr)
			ViewerCfg.Offscreen = cvOffscreen->asInt() ? true : false;

		// Optional for the same reason as Gl3/Offscreen. FogStart/FogEnd are WORLD-UNIT
		// eye distances (what setupFog feeds to the fog UBO), not 0..1 fractions.
		CConfigFile::CVar *cvFog = cf.getVarPtr("Fog");
		if (cvFog != nullptr)
			ViewerCfg.Fog = cvFog->asInt() ? true : false;
		CConfigFile::CVar *cvFogStart = cf.getVarPtr("FogStart");
		if (cvFogStart != nullptr)
			ViewerCfg.FogStart = cvFogStart->asFloat();
		CConfigFile::CVar *cvFogEnd = cf.getVarPtr("FogEnd");
		if (cvFogEnd != nullptr)
			ViewerCfg.FogEnd = cvFogEnd->asFloat();
		CConfigFile::CVar *cvFogColor = cf.getVarPtr("FogColor");
		if (cvFogColor != nullptr)
		{
			// "r,g,b". sscanf, not strtof/stof: MSVC 2008's CRT has no strtof (vs2008.yml
			// builds the NeL tools with -DWITH_NEL_TOOLS=ON) and stof is C++11.
			int fr = 0, fg = 0, fb = 0;
			std::string fogCol = cvFogColor->asString();
			if (sscanf(fogCol.c_str(), "%d,%d,%d", &fr, &fg, &fb) == 3)
				ViewerCfg.FogColor = CRGBA((uint8)fr, (uint8)fg, (uint8)fb, 255);
			else
				nlwarning("ZV: FogColor \"%s\" is not \"r,g,b\", keeping the default", fogCol.c_str());
		}

		CConfigFile::CVar &cvBackColor = cf.getVar("Background");
		nlassert(cvBackColor.size()==3);
		ViewerCfg.Background.R = cvBackColor.asInt(0);
		ViewerCfg.Background.G = cvBackColor.asInt(1);
		ViewerCfg.Background.B = cvBackColor.asInt(2);

		CConfigFile::CVar &cvZFar = cf.getVar("ZFar");
		ViewerCfg.ZFar = cvZFar.asFloat();

		CConfigFile::CVar &cvAutoLight = cf.getVar("AutoLight");
		ViewerCfg.AutoLight = cvAutoLight.asInt() ? true : false;

		CConfigFile::CVar &cvLightDir = cf.getVar("LightDir");
		nlassert(cvLightDir.size()==3);
		ViewerCfg.LightDir.x = cvLightDir.asFloat(0);
		ViewerCfg.LightDir.y = cvLightDir.asFloat(1);
		ViewerCfg.LightDir.z = cvLightDir.asFloat(2);

		CConfigFile::CVar &cvLandscapeTileNear = cf.getVar("LandscapeTileNear");
		ViewerCfg.LandscapeTileNear = cvLandscapeTileNear.asFloat();

		CConfigFile::CVar &cvLandscapeThreshold = cf.getVar("LandscapeThreshold");
		ViewerCfg.LandscapeThreshold = cvLandscapeThreshold.asFloat();

		CConfigFile::CVar &cvLandscapeNoise = cf.getVar("LandscapeNoise");
		ViewerCfg.LandscapeNoise = cvLandscapeNoise.asInt() != 0;

		// Optional: a zviewer.cfg written before vegetation support lacks these.
		CConfigFile::CVar *cvVegetable = cf.getVarPtr("Vegetable");
		if (cvVegetable != nullptr)
			ViewerCfg.Vegetable = cvVegetable->asInt() != 0;
		CConfigFile::CVar *cvVegetableDensity = cf.getVarPtr("VegetableDensity");
		if (cvVegetableDensity != nullptr)
			ViewerCfg.VegetableDensity = cvVegetableDensity->asFloat();
		CConfigFile::CVar *cvVegetableTexture = cf.getVarPtr("VegetableTexture");
		if (cvVegetableTexture != nullptr)
			ViewerCfg.VegetableTexture = cvVegetableTexture->asString();
		CConfigFile::CVar *cvDecal = cf.getVarPtr("Decal");
		if (cvDecal != nullptr)
			ViewerCfg.Decal = cvDecal->asInt() != 0;
		CConfigFile::CVar *cvDecalRadius = cf.getVarPtr("DecalRadius");
		if (cvDecalRadius != nullptr)
			ViewerCfg.DecalRadius = cvDecalRadius->asFloat();
		CConfigFile::CVar *cvDecalForward = cf.getVarPtr("DecalForward");
		if (cvDecalForward != nullptr)
			ViewerCfg.DecalForward = cvDecalForward->asFloat();

		// optional: absent in every cfg written before the pin existed, and the
		// default keeps the old mouse-driven behaviour
		CConfigFile::CVar *cvCameraLocked = cf.getVarPtr("CameraLocked");
		if (cvCameraLocked)
			ViewerCfg.CameraLocked = cvCameraLocked->asInt() != 0;
		CConfigFile::CVar *cvCameraRotZ = cf.getVarPtr("CameraRotZ");
		if (cvCameraRotZ)
			ViewerCfg.CameraRotZ = cvCameraRotZ->asFloat();
		CConfigFile::CVar *cvCameraRotX = cf.getVarPtr("CameraRotX");
		if (cvCameraRotX)
			ViewerCfg.CameraRotX = cvCameraRotX->asFloat();

		CConfigFile::CVar &cvBanksPath = cf.getVar("BanksPath");
		ViewerCfg.BanksPath = cvBanksPath.asString();

		CConfigFile::CVar &cvTilesPath = cf.getVar("TilesPath");
		ViewerCfg.TilesPath = cvTilesPath.asString();

		CConfigFile::CVar &cvUseDDS = cf.getVar("UseDDS");
		ViewerCfg.UseDDS = cvUseDDS.asInt() ? true : false;

		CConfigFile::CVar &cvAllPathRelative = cf.getVar("AllPathRelative");
		ViewerCfg.AllPathRelative = cvAllPathRelative.asInt() ? true : false;

		CConfigFile::CVar &cvBank = cf.getVar("Bank");
		ViewerCfg.Bank = cvBank.asString();
		
		// Optional on purpose: writeConfigFile() did not emit FarBank (see below), so every
		// zviewer.cfg on disk predating this fix lacks it, and getVar() would throw EUnknownVar
		// and abort the tool. Absent means the historical behaviour - derive
		// "<bank base>_su.farbank" from Bank - see the "_su.farbank" derivation in
		// displayZones(), which is also the reader below. Named rather than cited by
		// line number: a line number rots on the next unrelated edit in this file.
		CConfigFile::CVar *cvFarBank = cf.getVarPtr("FarBank");
		if (cvFarBank != nullptr)
			ViewerCfg.FarBank = cvFarBank->asString();

		CConfigFile::CVar &cvZonesPath = cf.getVar("ZonesPath");
		ViewerCfg.ZonesPath = cvZonesPath.asString();
		CPath::addSearchPath(cvZonesPath.asString());

		CConfigFile::CVar &cvIgPath = cf.getVar("IgPath");
		ViewerCfg.IgPath = cvIgPath.asString();
		CPath::addSearchPath(cvIgPath.asString());

		CConfigFile::CVar &cvShapePath = cf.getVar("ShapePath");
		ViewerCfg.ShapePath = cvShapePath.asString();
		CPath::addSearchPath(cvShapePath.asString());

		CConfigFile::CVar &cvMapsPath = cf.getVar("MapsPath");
		ViewerCfg.MapsPath = cvMapsPath.asString();
		CPath::addSearchPath(cvMapsPath.asString());

		CConfigFile::CVar &cvFontPath = cf.getVar("FontPath");
		ViewerCfg.FontPath = cvFontPath.asString();

		CConfigFile::CVar &cvHeightFieldName = cf.getVar("HeightFieldName");
		ViewerCfg.HeightFieldName = cvHeightFieldName.asString();
		
		CConfigFile::CVar &cvHeightFieldMaxZ = cf.getVar("HeightFieldMaxZ");
		ViewerCfg.HeightFieldMaxZ = cvHeightFieldMaxZ.asFloat();

		CConfigFile::CVar &cvHeightFieldOriginX = cf.getVar("HeightFieldOriginX");
		ViewerCfg.HeightFieldOriginX = cvHeightFieldOriginX.asFloat();

		CConfigFile::CVar &cvHeightFieldOriginY = cf.getVar("HeightFieldOriginY");
		ViewerCfg.HeightFieldOriginY = cvHeightFieldOriginY.asFloat();

		CConfigFile::CVar &cvHeightFieldSizeX = cf.getVar("HeightFieldSizeX");
		ViewerCfg.HeightFieldSizeX = cvHeightFieldSizeX.asFloat();

		CConfigFile::CVar &cvHeightFieldSizeY = cf.getVar("HeightFieldSizeY");
		ViewerCfg.HeightFieldSizeY = cvHeightFieldSizeY.asFloat();


		CConfigFile::CVar &cvLandAmb = cf.getVar("LandAmbient");
		nlassert(cvLandAmb.size()==3);
		ViewerCfg.LandAmbient.R = cvLandAmb.asInt(0);
		ViewerCfg.LandAmbient.G = cvLandAmb.asInt(1);
		ViewerCfg.LandAmbient.B = cvLandAmb.asInt(2);

		CConfigFile::CVar &cvLandDiff = cf.getVar("LandDiffuse");
		nlassert(cvLandDiff.size()==3);
		ViewerCfg.LandDiffuse.R = cvLandDiff.asInt(0);
		ViewerCfg.LandDiffuse.G = cvLandDiff.asInt(1);
		ViewerCfg.LandDiffuse.B = cvLandDiff.asInt(2);


		CConfigFile::CVar &cvZones = cf.getVar("Zones");
		for(uint i=0; i<cvZones.size(); i++)
		{
			ViewerCfg.Zones.push_back(cvZones.asString(i));
		}

		CConfigFile::CVar &cvIgs = cf.getVar("Ig");
		for(uint i=0; i<cvIgs.size(); i++)
		{
			ViewerCfg.Igs.push_back(cvIgs.asString(i));
		}

	}
	catch (const EConfigFile &e)
	{
		nlerror("Problem in config file : %s\n", e.what ());
	}

}

/****************************************************************\
							MAIN
\****************************************************************/
#ifdef NL_OS_WINDOWS
int APIENTRY nltWinMain(HINSTANCE /* hInstance */, HINSTANCE /* hPrevInstance */, LPTSTR /* lpCmdLine */, int /* nCmdShow */)
#else // NL_OS_WINDOWS
int nltmain(int /* argc */, NLMISC::tchar ** /* argv */)
#endif // NL_OS_WINDOWS
{
	try
	{
		NLMISC::CApplicationContext myApplicationContext;

#ifdef NL_OS_UNIX
		NLMISC::CPath::addSearchPath(NLMISC::CPath::getApplicationDirectory("NeL"));
#endif // NL_OS_UNIX

		NLMISC::CPath::addSearchPath(NL_ZVIEWER_CFG);

		initViewerConfig("zviewer.cfg");

		// Init NELU
		NL3D::CNELU::init(ViewerCfg.Width, ViewerCfg.Height, CViewport(), ViewerCfg.Depth, ViewerCfg.Windowed, EmptyWindow, ViewerCfg.Offscreen, false, ViewerCfg.Gl3);
		NL3D::CNELU::Driver->setWindowTitle(ucstring("NeL ZViewer"));

		// Fog is driver state, set once. Same call pair the client uses per fog map
		// (ryzom/client/src/fog_map.cpp:50-51).
		if (ViewerCfg.Fog)
		{
			NL3D::CNELU::Driver->enableFog(true);
			NL3D::CNELU::Driver->setupFog(ViewerCfg.FogStart, ViewerCfg.FogEnd, ViewerCfg.FogColor);
		}
		NL3D::CNELU::Camera->setTransformMode(ITransformable::DirectMatrix);

		// Init the font manager
		ViewerCfg.TextContext.init (CNELU::Driver, &ViewerCfg.FontManager);
		ViewerCfg.TextContext.setFontGenerator(ViewerCfg.FontPath);
		ViewerCfg.TextContext.setFontSize(12);
		ViewerCfg.FontManager.setMaxMemory(2000000);

		displayZones();

		// release nelu
		NL3D::CNELU::release();
	}
	catch (const Exception &e)
	{
		nlerror("main trapped an exception: '%s'", e.what ());
	}

	return 0;
}
