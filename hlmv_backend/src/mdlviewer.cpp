//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//
//=============================================================================//
//
//                 Half-Life Model Viewer (c) 1999 by Mete Ciragan
//
// file:           mdlviewer.cpp
// last modified:  Jun 03 1999, Mete Ciragan
// copyright:      The programs and associated files contained in this
//                 distribution were developed by Mete Ciragan. The programs
//                 are not in the public domain, but they are freely
//                 distributable without licensing fees. These programs are
//                 provided without guarantee or warrantee expressed or
//                 implied.
//
// version:        1.2
//
// email:          mete@swissquake.ch
// web:            http://www.swissquake.ch/chumbalum-soft/
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <mx/mx.h>
#include <mx/mxTga.h>
#include <mx/mxEvent.h>
#include "mdlviewer.h"
#include "ViewerSettings.h"
#include "MatSysWin.h"
#include "ControlPanel.h"
#include "StudioModel.h"
#include "FileAssociation.h"
#include "tier1/strtools.h"
#include "tier0/icommandline.h"
#include "filesystem.h"
#include "ifilesystemopendialog.h"
#include "filesystem_init.h"
#include "appframework/appframework.h"
#include "icvar.h"
#include "vstdlib/cvar.h"
#include "istudiorender.h"
#include "materialsystem/imaterialsystem.h"
#include "bitmap/imageformat.h"
#include "vphysics_interface.h"
#include "Datacache/imdlcache.h"
#include "datacache/idatacache.h"
#include "filesystem_init.h"
#include "materialsystem/imaterialsystemhardwareconfig.h"
#include "SoundEmitterSystem/isoundemittersystembase.h"
#include "soundsystem/isoundsystem.h"
#include "tier1/tier1.h"
#include "tier2/tier2.h"
#include "camera.h"
#include "hlmv_compat.h"
#include "backend_ipc.h"

bool g_bOldFileDialogs = false;
void HLMV_Log(const char* fmt, ...);

MDLViewer *g_MDLViewer = 0;
char g_appTitle[] = "Half-Life Model Viewer v1.22 - Tweaked";
static char recentFiles[8][256] = { "", "", "", "", "", "", "", "" };
extern int g_dxlevel;
bool g_bInError = false;
static int g_thumbExitCode = -1;


//-----------------------------------------------------------------------------
// Singleton interfaces
//-----------------------------------------------------------------------------
IStudioRender *g_pStudioRender;
IMDLCache *g_pMDLCache;
IPhysicsSurfaceProps *physprop;
IPhysicsCollision *physcollision;
IFileSystem *g_pFileSystem;
IStudioDataCache *g_pStudioDataCache;
IDataCache *g_pDataCache;
ISoundEmitterSystemBase *g_pSoundEmitterBase;
ISoundSystem *g_pSoundSystem;
CreateInterfaceFn g_Factory;

// Filesystem dialog module wrappers.
CSysModule *g_pFSDialogModule = 0;
CreateInterfaceFn g_FSDialogFactory = 0;


void LoadFileSystemDialogModule()
{
	Assert( !g_pFSDialogModule );

	// Load the module with the file system open dialog.
	const char *pDLLName = "FileSystemOpenDialog.dll";
	g_pFSDialogModule = g_pFileSystem->LoadModule( pDLLName );
	if ( !g_pFSDialogModule || 
		 !(g_FSDialogFactory = Sys_GetFactory( g_pFSDialogModule ))
		 )
	{
		if ( g_pFSDialogModule )
		{
			g_pFileSystem->UnloadModule( g_pFSDialogModule );
			g_pFSDialogModule = NULL;
		}
	}
}

void UnloadFileSystemDialogModule()
{
	if ( g_pFSDialogModule )
	{
		g_pFileSystem->UnloadModule( g_pFSDialogModule );
		g_pFSDialogModule = 0;
	}
}	



void
MDLViewer::initRecentFiles ()
{
	// In VMT-Studio backend, Win32 menus are unused and updating them causes crashes.
	return;
}



void
MDLViewer::loadRecentFiles ()
{
	char path[256];
	strcpy (path, mx::getApplicationPath ());
	strcat (path, "/hlmv.rf");
	FILE *file = fopen (path, "rb");
	if (file)
	{
		fread (recentFiles, sizeof recentFiles, 1, file);
		fclose (file);
	}
}



void
MDLViewer::saveRecentFiles ()
{
	char path[256];

	strcpy (path, mx::getApplicationPath ());
	strcat (path, "/hlmv.rf");

	FILE *file = fopen (path, "wb");
	if (file)
	{
		fwrite (recentFiles, sizeof recentFiles, 1, file);
		fclose (file);
	}
}

struct AccelTableEntry_t
{
	unsigned short key;
	unsigned short command;
	unsigned char  flags;
};

namespace
{

// A 256 px snapshot is rendered at 512 px and box-filtered down: the D3D9
// backend has a quirk where a 256 px window only paints the top half, and the
// extra resolution also gives a cleaner preview. Larger requests are capped.
int ThumbRenderSize( int outputSize )
{
	if ( outputSize < 64 )
		outputSize = 64;
	if ( outputSize > 1024 )
		outputSize = 1024;
	int render = ( outputSize <= 512 ) ? outputSize * 2 : outputSize;
	if ( render > 1024 )
		render = 1024;
	return render;
}

// Reads the client area of a rendered window into top-down BGRA. The fast path
// is BitBlt + GetDIBits; some D3D9 drivers return a black bitmap for a window
// DC, so it falls back to the GetPixel route that dumpViewport() already uses.
bool CaptureClientBgra( HWND hwnd, int width, int height, std::vector<unsigned char> &out )
{
	if ( !hwnd || !IsWindow( hwnd ) || width <= 0 || height <= 0 || width > 4096 || height > 4096 )
		return false;

	out.assign( (size_t)width * height * 4, 0 );

	bool ok = false;
	HDC src = GetDC( hwnd );
	if ( !src )
		return false;

	HDC mem = CreateCompatibleDC( src );
	HBITMAP bmp = mem ? CreateCompatibleBitmap( src, width, height ) : NULL;
	if ( mem && bmp )
	{
		HGDIOBJ old = SelectObject( mem, bmp );
		const BOOL blt = BitBlt( mem, 0, 0, width, height, src, 0, 0, SRCCOPY );
		SelectObject( mem, old );
		if ( blt )
		{
			BITMAPINFO bi{};
			bi.bmiHeader.biSize = sizeof( BITMAPINFOHEADER );
			bi.bmiHeader.biWidth = width;
			bi.bmiHeader.biHeight = -height; // top-down rows
			bi.bmiHeader.biPlanes = 1;
			bi.bmiHeader.biBitCount = 32;
			bi.bmiHeader.biCompression = BI_RGB;
			const int got = GetDIBits( mem, bmp, 0, height, out.data(), &bi, DIB_RGB_COLORS );
			ok = ( got == height );
		}
	}
	if ( bmp )
		DeleteObject( bmp );
	if ( mem )
		DeleteDC( mem );
	ReleaseDC( hwnd, src );

	if ( ok )
	{
		// A black capture means BitBlt did not see the D3D surface.
		bool nonBlack = false;
		for ( size_t i = 0; i + 3 < out.size(); i += 4 )
		{
			if ( out[i] || out[i + 1] || out[i + 2] )
			{
				nonBlack = true;
				break;
			}
		}
		ok = nonBlack;
	}

	if ( !ok )
	{
		HDC hdc = GetDC( hwnd );
		if ( !hdc )
			return false;
		const size_t rowBytes = (size_t)width * 4;
		for ( int y = 0; y < height; ++y )
		{
			unsigned char *row = out.data() + (size_t)y * rowBytes;
			for ( int x = 0; x < width; ++x )
			{
				const COLORREF c = GetPixel( hdc, x, y );
				unsigned char *p = row + (size_t)x * 4;
				p[0] = GetBValue( c );
				p[1] = GetGValue( c );
				p[2] = GetRValue( c );
				p[3] = 255;
			}
		}
		ReleaseDC( hwnd, hdc );
		ok = true;
	}

	return ok;
}

void DownscaleBgra( const std::vector<unsigned char> &src, int srcSize, int dstSize,
	std::vector<unsigned char> &dst )
{
	if ( dstSize <= 0 || srcSize <= 0 || dstSize > 4096 || srcSize > 4096 )
	{
		dst.clear();
		return;
	}
	if ( src.size() < (size_t)srcSize * srcSize * 4 )
	{
		dst.clear();
		return;
	}

	dst.resize( (size_t)dstSize * dstSize * 4 );
	if ( srcSize == dstSize )
	{
		dst = src;
		return;
	}

	const int factor = ( dstSize > 0 ) ? srcSize / dstSize : 0;
	if ( factor > 0 && srcSize % dstSize == 0 )
	{
		const int samples = factor * factor;
		for ( int y = 0; y < dstSize; ++y )
		{
			for ( int x = 0; x < dstSize; ++x )
			{
				unsigned sum[4] = { 0, 0, 0, 0 };
				for ( int dy = 0; dy < factor; ++dy )
				{
					const unsigned char *row =
						src.data() + (size_t)( y * factor + dy ) * srcSize * 4;
					for ( int dx = 0; dx < factor; ++dx )
					{
						const unsigned char *p = row + (size_t)( x * factor + dx ) * 4;
						sum[0] += p[0];
						sum[1] += p[1];
						sum[2] += p[2];
						sum[3] += p[3];
					}
				}
				unsigned char *q = dst.data() + ( (size_t)y * dstSize + x ) * 4;
				q[0] = (unsigned char)( sum[0] / samples );
				q[1] = (unsigned char)( sum[1] / samples );
				q[2] = (unsigned char)( sum[2] / samples );
				q[3] = 255;
			}
		}
		return;
	}

	for ( int y = 0; y < dstSize; ++y )
	{
		int sy = y * srcSize / dstSize;
		if ( sy >= srcSize ) sy = srcSize - 1;
		if ( sy < 0 ) sy = 0;
		for ( int x = 0; x < dstSize; ++x )
		{
			int sx = x * srcSize / dstSize;
			if ( sx >= srcSize ) sx = srcSize - 1;
			if ( sx < 0 ) sx = 0;
			const unsigned char *p = src.data() + ( (size_t)sy * srcSize + sx ) * 4;
			unsigned char *q = dst.data() + ( (size_t)y * dstSize + x ) * 4;
			q[0] = p[0];
			q[1] = p[1];
			q[2] = p[2];
			q[3] = 255;
		}
	}
}

} // namespace

#define NUM_ACCELERATORS 21
AccelTableEntry_t accelTable[NUM_ACCELERATORS] = {	{VK_F5, IDC_FILE_REFRESH,		mx::ACCEL_VIRTKEY},
													{'w', IDC_ACCEL_WIREFRAME,		mx::ACCEL_CONTROL | mx::ACCEL_VIRTKEY},
													{'W', IDC_ACCEL_WIREFRAME,		mx::ACCEL_CONTROL | mx::ACCEL_VIRTKEY},
													{'a', IDC_ACCEL_ATTACHMENTS,	mx::ACCEL_CONTROL | mx::ACCEL_VIRTKEY},
													{'A', IDC_ACCEL_ATTACHMENTS,	mx::ACCEL_CONTROL | mx::ACCEL_VIRTKEY},
													{'g', IDC_ACCEL_GROUND,			mx::ACCEL_CONTROL | mx::ACCEL_VIRTKEY},
													{'G', IDC_ACCEL_GROUND,			mx::ACCEL_CONTROL | mx::ACCEL_VIRTKEY},
													{'h', IDC_ACCEL_HITBOXES,		mx::ACCEL_CONTROL | mx::ACCEL_VIRTKEY},
													{'H', IDC_ACCEL_HITBOXES,		mx::ACCEL_CONTROL | mx::ACCEL_VIRTKEY},
													{'o', IDC_ACCEL_BONES,			mx::ACCEL_CONTROL | mx::ACCEL_VIRTKEY},
													{'O', IDC_ACCEL_BONES,			mx::ACCEL_CONTROL | mx::ACCEL_VIRTKEY},
													{'b', IDC_ACCEL_BACKGROUND,		mx::ACCEL_CONTROL | mx::ACCEL_VIRTKEY},
													{'B', IDC_ACCEL_BACKGROUND,		mx::ACCEL_CONTROL | mx::ACCEL_VIRTKEY},
													{'m', IDC_ACCEL_MOVEMENT,		mx::ACCEL_CONTROL | mx::ACCEL_VIRTKEY},
													{'M', IDC_ACCEL_MOVEMENT,		mx::ACCEL_CONTROL | mx::ACCEL_VIRTKEY},
													{'n', IDC_ACCEL_NORMALS,		mx::ACCEL_CONTROL | mx::ACCEL_VIRTKEY},
													{'N', IDC_ACCEL_NORMALS,		mx::ACCEL_CONTROL | mx::ACCEL_VIRTKEY},
													{'t', IDC_ACCEL_TANGENTS,		mx::ACCEL_CONTROL | mx::ACCEL_VIRTKEY},
													{'T', IDC_ACCEL_TANGENTS,		mx::ACCEL_CONTROL | mx::ACCEL_VIRTKEY},
													{'s', IDC_ACCEL_SHADOW,			mx::ACCEL_CONTROL | mx::ACCEL_VIRTKEY},
													{'S', IDC_ACCEL_SHADOW,			mx::ACCEL_CONTROL | mx::ACCEL_VIRTKEY}};


MDLViewer::MDLViewer ()
: mxWindow (0, 0, 0, 0, 0, g_appTitle, mxWindow::Normal)
{
	HLMV_Log("[HLMV] MDLViewer constructor start\n");
	d_MatSysWindow = 0;
	d_cpl = 0;
	m_controlPanelHidden = false;

	// create menu stuff
	mb = new mxMenuBar (this);
	mxMenu *menuFile = new mxMenu ();
	mxMenu *menuOptions = new mxMenu ();
	menuView = new mxMenu ();
	mxMenu *menuHelp = new mxMenu ();

	mb->addMenu ("File", menuFile);
	mb->addMenu ("Options", menuOptions);
	mb->addMenu ("View", menuView);
	mb->addMenu ("Help", menuHelp);

	mxMenu *menuRecentModels = new mxMenu ();
	menuRecentModels->add ("(empty)", IDC_FILE_RECENTMODELS1);
	menuRecentModels->add ("(empty)", IDC_FILE_RECENTMODELS2);
	menuRecentModels->add ("(empty)", IDC_FILE_RECENTMODELS3);
	menuRecentModels->add ("(empty)", IDC_FILE_RECENTMODELS4);
	menuRecentModels->add ("(empty)", IDC_FILE_RECENTMODELS5);
	menuRecentModels->add ("(empty)", IDC_FILE_RECENTMODELS6);
	menuRecentModels->add ("(empty)", IDC_FILE_RECENTMODELS7);
	menuRecentModels->add ("(empty)", IDC_FILE_RECENTMODELS8);

	if ( g_bOldFileDialogs )
	{
		menuFile->add ("Load Model...", IDC_FILE_LOADMODEL);
		menuFile->add ("(Steam) Load Model...", IDC_FILE_LOADMODEL_STEAM);
	}
	else
	{
		menuFile->add ("Load Model...", IDC_FILE_LOADMODEL_STEAM);
	}

	menuFile->add( "Refresh (F5)", IDC_FILE_REFRESH );
	menuFile->addSeparator ();
	
	if ( g_bOldFileDialogs )
	{
		menuFile->add ("Load Weapon...", IDC_FILE_LOADMERGEDMODEL);
		menuFile->add ("(Steam) Load Weapon...", IDC_FILE_LOADMERGEDMODEL_STEAM);
	}
	else
	{
		menuFile->add ("Load Weapon...", IDC_FILE_LOADMERGEDMODEL_STEAM);
	}

	menuFile->add ("Unload Weapon", IDC_FILE_UNLOADMERGEDMODEL);
	
	menuFile->addSeparator ();
	menuFile->add ("Load Background Texture...", IDC_FILE_LOADBACKGROUNDTEX);
	menuFile->add ("Load Ground Texture...", IDC_FILE_LOADGROUNDTEX);
	menuFile->addSeparator ();
	menuFile->add ("Unload Ground Texture", IDC_FILE_UNLOADGROUNDTEX);
	menuFile->addSeparator ();
	menuFile->addMenu ("Recent Models", menuRecentModels);
	menuFile->addSeparator ();
	menuFile->add ("Exit", IDC_FILE_EXIT);

	menuFile->setEnabled(IDC_FILE_LOADBACKGROUNDTEX, false);
	menuFile->setEnabled(IDC_FILE_LOADGROUNDTEX, false);
	menuFile->setEnabled(IDC_FILE_UNLOADGROUNDTEX, false);

	menuOptions->add ("Background Color...", IDC_OPTIONS_COLORBACKGROUND);
	menuOptions->add ("Ground Color...", IDC_OPTIONS_COLORGROUND);
	menuOptions->add ("Light Color...", IDC_OPTIONS_COLORLIGHT);
	menuOptions->add ("Ambient Color...", IDC_OPTIONS_COLORAMBIENT);
	menuOptions->addSeparator ();
	menuOptions->add ("Center View", IDC_OPTIONS_CENTERVIEW);
	menuOptions->add ("Viewmodel Mode", IDC_OPTIONS_VIEWMODEL);
	menuOptions->add ("Toggle Control Panel Hiding", IDC_OPTIONS_HIDEMENU);

#ifdef WIN32
	menuOptions->addSeparator ();
	menuOptions->add ("Make Screenshot...", IDC_OPTIONS_MAKESCREENSHOT);
	//menuOptions->add ("Dump Model Info", IDC_OPTIONS_DUMP);
#endif

	menuView->add ("File Associations...", IDC_VIEW_FILEASSOCIATIONS);
	menuView->setEnabled( IDC_VIEW_FILEASSOCIATIONS, false );

	menuView->addSeparator ();
	menuView->add ("Show Activities", IDC_VIEW_ACTIVITIES);
	menuView->add ("Show hidden", IDC_VIEW_HIDDEN );

#ifdef WIN32
	menuHelp->add ("Goto Homepage...", IDC_HELP_GOTOHOMEPAGE);
	menuHelp->addSeparator ();
#endif
	menuHelp->add ("About...", IDC_HELP_ABOUT);

	HLMV_Log("[HLMV] creating MatSysWindow...\n");
	d_MatSysWindow = new MatSysWindow (this, 0, 0, 100, 100, "", mxWindow::Normal);
	HLMV_Log("[HLMV] MatSysWindow created: %p\n", d_MatSysWindow);
#ifdef WIN32
	// SetWindowLong ((HWND) d_MatSysWindow->getHandle (), GWL_EXSTYLE, WS_EX_CLIENTEDGE);
#endif

	HLMV_Log("[HLMV] creating ControlPanel...\n");
	d_cpl = new ControlPanel (this);
	HLMV_Log("[HLMV] ControlPanel created: %p\n", d_cpl);
	d_cpl->setMatSysWindow (d_MatSysWindow);
	g_MatSysWindow = d_MatSysWindow;

	HLMV_Log("[HLMV] creating FileAssociation...\n");
	g_FileAssociation = new FileAssociation ();

	HLMV_Log("[HLMV] loadRecentFiles...\n");
	loadRecentFiles ();
	HLMV_Log("[HLMV] initRecentFiles...\n");
	initRecentFiles ();
	HLMV_Log("[HLMV] checking -embed...\n");

	// Hidden model thumbnail mode (Asset Browser). The window is never seen:
	// it is a fully transparent layered window that exists only to own the
	// back buffer for one frame.
	const bool thumbMode = CommandLine()->FindParm( "-thumb" ) != 0;
	int thumbSize = ThumbRenderSize( CommandLine()->ParmValue( "-size", 256 ) );
	if ( thumbSize < 64 )
		thumbSize = 64;
	if ( thumbSize > 1024 )
		thumbSize = 1024;

	const char *pEmbedStr = CommandLine()->ParmValue( "-embed", (const char*)0 );
	if ( pEmbedStr )
	{
		HWND hParent = (HWND)(uintptr_t)strtoull( pEmbedStr, nullptr, 0 );
		if ( hParent && IsWindow( hParent ) )
		{
			m_controlPanelHidden = true;
			mb->setVisible( false );
			d_cpl->setVisible( false );

			HWND myHwnd = (HWND)getHandle();
			SetMenu( myHwnd, NULL );
			SetParent( myHwnd, hParent );
			LONG style = GetWindowLong( myHwnd, GWL_STYLE );
			style &= ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
			style |= (WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
			SetWindowLong( myHwnd, GWL_STYLE, style );

			LONG exStyle = GetWindowLong( myHwnd, GWL_EXSTYLE );
			exStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
			SetWindowLong( myHwnd, GWL_EXSTYLE, exStyle );

			RECT rc{};
			GetClientRect( hParent, &rc );
			int w = rc.right - rc.left;
			int h = rc.bottom - rc.top;
			setBounds( 0, 0, w, h );
			if ( d_MatSysWindow )
			{
				d_MatSysWindow->setBounds( 0, 0, w, h );
			}
		}
		else
		{
			setBounds (20, 20, 640, 700);
		}
	}
	else if ( thumbMode )
	{
		m_controlPanelHidden = true;
		mb->setVisible( false );
		d_cpl->setVisible( false );

		// Borderless + tool window: the client area is exactly the capture
		// size, and even if Windows ever shows it there is no taskbar entry.
		HWND myHwnd = (HWND)getHandle();
		SetMenu( myHwnd, NULL );
		LONG style = GetWindowLong( myHwnd, GWL_STYLE );
		style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
		SetWindowLong( myHwnd, GWL_STYLE, style );
		LONG exStyle = GetWindowLong( myHwnd, GWL_EXSTYLE );
		exStyle &= ~(WS_EX_APPWINDOW | WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
		exStyle |= WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT;
		SetWindowLong( myHwnd, GWL_EXSTYLE, exStyle );
		// The window must be mapped for the D3D9 swap chain to be presented,
		// but the user must never see it: a fully transparent layered window is
		// still rendered and composited, and its client DC keeps the frame that
		// CaptureModelThumbnail() reads with GDI.
		SetLayeredWindowAttributes( myHwnd, 0, 0, LWA_ALPHA );
		SetWindowPos( myHwnd, NULL, 0, 0, thumbSize, thumbSize,
			SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW );
		setBounds( 0, 0, thumbSize, thumbSize );
		if ( d_MatSysWindow )
		{
			d_MatSysWindow->setBounds( 0, 0, thumbSize, thumbSize );
		}
	}
	else
	{
		int w = 640;
		setBounds (20, 20, w, 700);
	}
	if ( !thumbMode )
		setVisible (true);
	else
		setVisible (true); // transparent layered window; the user sees nothing

	CUtlVector< mx::Accel_t > accelerators;
	mx::Accel_t accel;

	for (int i=0; i < NUM_ACCELERATORS; i++)
	{
		accel.flags	  = accelTable[i].flags ;
		accel.key	  = accelTable[i].key;
		accel.command = accelTable[i].command;
		accelerators.AddToTail( accel );
	}

	mx::createAccleratorTable( accelerators.Count(), accelerators.Base() );
}



MDLViewer::~MDLViewer ()
{
	saveRecentFiles ();
	SaveViewerSettings( g_pStudioModel->GetFileName(), g_pStudioModel );

#ifdef WIN32
	DeleteFile ("hlmv.cfg");
	DeleteFile ("midump.txt");
#endif
}


//-----------------------------------------------------------------------------
// Purpose: Reloads the currently loaded model file.
//-----------------------------------------------------------------------------
void MDLViewer::Refresh( void )
{
	SaveViewerSettings( g_pStudioModel->GetFileName(), g_pStudioModel );
	g_pStudioModel->ReleaseStudioModel( );
	g_pMDLCache->Flush( );
	if ( recentFiles[0][0] != '\0' )
	{
		char szFile[MAX_PATH];
		strcpy( szFile, recentFiles[0] ); 
		g_pMaterialSystem->ReloadMaterials( );
		d_cpl->loadModel( szFile );
	}
}


//-----------------------------------------------------------------------------
// Purpose: Loads the file and updates the MRU list.
// Input  : pszFile - File to load.
//-----------------------------------------------------------------------------
void MDLViewer::LoadModelFile( const char *pszFile, int slot )
{
	// copy off name, pszFile may be point into recentFiles array
	char filename[1024];
	strcpy( filename, pszFile );

	LoadModelResult_t eLoaded = d_cpl->loadModel( filename, slot );

	if ( eLoaded != LoadModel_Success )
	{
		HLMV_Log("[MDL] LoadModel failed for '%s' with code %d\n", filename, eLoaded);
		extern bool g_bEmbedded;
		if ( !g_bEmbedded )
		{
			switch (eLoaded)
			{
				case LoadModel_LoadFail:
				{
					mxMessageBox (this, "Error loading model.", g_appTitle, MX_MB_ERROR | MX_MB_OK);
					break;
				}
				
				case LoadModel_PostLoadFail:
				{
					mxMessageBox (this, "Error post-loading model.", g_appTitle, MX_MB_ERROR | MX_MB_OK);
					break;
				}

				case LoadModel_NoModel:
				{
					mxMessageBox (this, "Error loading model. The model has no vertices.", g_appTitle, MX_MB_ERROR | MX_MB_OK);
					break;
				}
			}
		}

		return;
	}

	if (slot == -1)
	{
		int i;
		for (i = 0; i < 8; i++)
		{
			if (!mx_strcasecmp( recentFiles[i], filename ))
				break;
		}

		// shift down existing recent files
		for (i = ((i > 7) ? 7 : i); i > 0; i--)
		{
			strcpy (recentFiles[i], recentFiles[i-1]);
		}

		strcpy( recentFiles[0], filename );

		initRecentFiles ();

		setLabel( "%s", filename );
	}
}


//-----------------------------------------------------------------------------
// Purpose: Takes a TGA screenshot of the given filename and exits.
// Input  : pszFile - File to load.
//-----------------------------------------------------------------------------
void MDLViewer::SaveScreenShot( const char *pszFile )
{
	char filename[1024];
	strcpy( filename, pszFile );
	LoadModelResult_t eLoaded = d_cpl->loadModel( filename );

	//
	// Screenshot mode. Write a screenshot file and exit.
	//
	if ( eLoaded == LoadModel_Success )
	{
		g_viewerSettings.bgColor[0] = 117.0f / 255.0f;
		g_viewerSettings.bgColor[1] = 196.0f / 255.0f;
		g_viewerSettings.bgColor[2] = 219.0f / 255.0f;

		// Build the name of the TGA to write.
		char szScreenShot[256];
		strcpy(szScreenShot, filename);
		char *pchDot = strrchr(szScreenShot, '.');
		if (pchDot)
		{
			strcpy(pchDot, ".tga");
		}
		else
		{
			strcat(szScreenShot, ".tga");
		}

		// Center the view and write the TGA.
		d_cpl->centerView();
		d_MatSysWindow->dumpViewport(szScreenShot);
	}

	// Shut down.
	mx::quit();
	return;
}


//-----------------------------------------------------------------------------
// Purpose: One hidden size x size model snapshot for the Asset Browser. The
//          process was created with -thumb, so the window is a transparent
//          layered window the user never sees; this function loads the model,
//          renders one frame, captures it and writes a top-down BMP.
//-----------------------------------------------------------------------------
bool MDLViewer::CaptureModelThumbnail( const char *outPath, int size, const char *modelPath )
{
	if ( !outPath || !outPath[0] || !modelPath || !modelPath[0] || !d_cpl || !d_MatSysWindow )
	{
		return false;
	}
	if ( size < 64 || size > 1024 )
	{
		return false;
	}

	// Render at twice the output size: the D3D9 backend only paints the top
	// half of a 256 px window, and supersampling gives a cleaner thumbnail.
	const int renderSize = ThumbRenderSize( size );

	// The window was sized by the constructor; make sure a viewer setting has
	// not changed the viewport in the meantime.
	d_MatSysWindow->setBounds( 0, 0, renderSize, renderSize );

	LoadModelResult_t eLoaded = d_cpl->loadModel( modelPath );
	if ( eLoaded != LoadModel_Success )
	{
		HLMV_Log("[THUMB] load failed for '%s' code %d\n", modelPath, eLoaded);
		return false;
	}

	// loadModel() may restore saved viewer settings; apply the thumbnail look
	// afterwards so the snapshot is deterministic.
	g_viewerSettings.showBackground = false;
	g_viewerSettings.showGround = false;
	g_viewerSettings.showShadow = false;
	g_viewerSettings.showHitBoxes = false;
	g_viewerSettings.showBones = false;
	g_viewerSettings.showAttachments = false;
	g_viewerSettings.showNormals = false;
	g_viewerSettings.showTangentFrame = false;
	g_viewerSettings.bgColor[0] = 24.0f / 255.0f;
	g_viewerSettings.bgColor[1] = 26.0f / 255.0f;
	g_viewerSettings.bgColor[2] = 31.0f / 255.0f;
	g_viewerSettings.pause = true;

	// A stable front view at frame 0.
	if ( !g_pStudioModel->HasModel() )
	{
		HLMV_Log("[THUMB] model has no geometry after load\n");
		return false;
	}
	g_pStudioModel->SetSequence( 0 );
	g_pStudioModel->SetFrame( 0 );

	Vector bmin, bmax;
	g_pStudioModel->ExtractBbox( bmin, bmax );
	HLMV_Log("[THUMB] bbox=(%.2f %.2f %.2f)-(%.2f %.2f %.2f) renderMode=%d transparent=%d\n",
		bmin.x, bmin.y, bmin.z, bmax.x, bmax.y, bmax.z,
		(int)g_viewerSettings.renderMode, (int)g_pStudioModel->m_bIsTransparent );

	// Let Windows map and paint the window before the first GL frame; the
	// process has not entered mx::run() yet, so nobody pumps messages for it.
	HWND earlyHwnd = (HWND)d_MatSysWindow->getHandle();
	UpdateWindow( earlyHwnd );
	for ( int i = 0; i < 20; ++i )
	{
		MSG msg;
		while ( PeekMessage( &msg, NULL, 0, 0, PM_REMOVE ) )
		{
			TranslateMessage( &msg );
			DispatchMessage( &msg );
		}
		Sleep( 10 );
	}

	// centerView() itself calls redraw(); the window is off-screen, so letting
	// it present the frame is harmless.
	g_bSkipSwap = false;
	g_bSkipEndFrame = false;
	d_cpl->centerView();

	d_MatSysWindow->draw();
	Sleep( 50 );

	// ReadPixels() is unreliable in this fixed-function path (it returns the
	// cleared material-system target), so capture the client area with GDI,
	// the same route as MatSysWindow::dumpViewport().
	HWND hwnd = (HWND)d_MatSysWindow->getHandle();
	RECT rc{};
	GetClientRect( hwnd, &rc );
	const int w = rc.right - rc.left;
	const int h = rc.bottom - rc.top;
	HLMV_Log("[THUMB] draw done, render=%d client=%dx%d output=%d polycount=%d\n",
		renderSize, w, h, size, (int)g_pStudioModel->GetDrawMetrics().PolyCount );
	if ( w < renderSize || h < renderSize )
	{
		HLMV_Log("[THUMB] viewport smaller than requested render size\n");
		return false;
	}

	std::vector<unsigned char> captured;
	if ( !CaptureClientBgra( hwnd, renderSize, renderSize, captured ) )
	{
		HLMV_Log("[THUMB] client capture failed\n");
		return false;
	}

	std::vector<unsigned char> topDown;
	DownscaleBgra( captured, renderSize, size, topDown );

	const bool ok = Backend_WriteBmp32( outPath, size, size, topDown.data() );
	HLMV_Log("[THUMB] %s -> %s (%dx%d from %dx%d)\n", ok ? "wrote" : "failed",
		outPath, size, size, renderSize, renderSize);
	return ok;
}


void MDLViewer::DumpText( const char *pszFile )
{
	char filename[1024];
	strcpy( filename, pszFile );
	LoadModelResult_t eLoaded = d_cpl->loadModel( filename );

	//
	// Screenshot mode. Write a screenshot file and exit.
	//
	if ( eLoaded == LoadModel_Success )
	{
		if ( g_pStudioModel->m_bIsTransparent )
		{
			Msg("%s is transparent\n", filename );
		}
		if ( g_pStudioModel->m_bHasProxy )
		{
			Msg("%s has material proxies\n", filename );
		}
	}

	// Shut down.
	mx::quit();
}


const char* MDLViewer::SteamGetOpenFilename()
{
	if ( !g_FSDialogFactory )
		return NULL;

	static char filename[MAX_PATH];

	IFileSystemOpenDialog *pDlg;
	pDlg = (IFileSystemOpenDialog*)g_FSDialogFactory( FILESYSTEMOPENDIALOG_VERSION, NULL );
	if ( !pDlg )
	{
		char str[512];
		Q_snprintf( str, sizeof( str ), "Can't create %s interface.", FILESYSTEMOPENDIALOG_VERSION );
		MessageBox( NULL, str, "Error", MB_OK );
		return NULL;
	}
	pDlg->Init( g_Factory, NULL );
	pDlg->AddFileMask( "*.jpg" );
	pDlg->AddFileMask( "*.mdl" );
	pDlg->SetInitialDir( "models", "game" );
	pDlg->SetFilterMdlAndJpgFiles( true );

	if (pDlg->DoModal() == IDOK)
	{
		pDlg->GetFilename( filename, sizeof( filename ) );
		pDlg->Release();
		return filename;
	}
	else
	{
		pDlg->Release();
		return NULL;
	}
}


int
MDLViewer::handleEvent (mxEvent *event)
{
	MDLCACHE_CRITICAL_SECTION_( g_pMDLCache );
	
	switch (event->event)
	{
	case mxEvent::Action:
	{
		switch (event->action)
		{
		case IDC_FILE_LOADMODEL:
		{
			const char *ptr = mxGetOpenFileName (this, 0, "*.mdl");
			if (ptr)
			{
				LoadModelFile( ptr );
			}
		}
		break;

		case IDC_FILE_LOADMODEL_STEAM:
		{
			const char *pFilename = SteamGetOpenFilename();
			if ( pFilename )
			{
				LoadModelFile( pFilename );
			}
		}
		break;

		case IDC_FILE_LOADMERGEDMODEL:
		{
			const char *ptr = mxGetOpenFileName (this, 0, "*.mdl");
			if (ptr)
			{
				strcpy( g_viewerSettings.mergeModelFile[0], ptr );
				LoadModelFile( ptr, 0 );
			}
		}
		break;

		case IDC_FILE_LOADMERGEDMODEL_STEAM:
		{
			const char *pFilename = SteamGetOpenFilename();
			if ( pFilename )
			{
				strcpy( g_viewerSettings.mergeModelFile[0], pFilename );
				LoadModelFile( pFilename, 0 );
			}
		}
		break;

		case IDC_FILE_UNLOADMERGEDMODEL:
		{
			// FIXME: move to d_cpl
			if (g_pStudioExtraModel[0])
			{
				strcpy( g_viewerSettings.mergeModelFile[0], "" );
				g_pStudioExtraModel[0]->FreeModel( false );
				delete g_pStudioExtraModel[0];
				g_pStudioExtraModel[0] = NULL;
			}
		}
		break;

		case IDC_FILE_REFRESH:
		{
			Refresh();
			break;
		}

		case IDC_FILE_LOADBACKGROUNDTEX:
		case IDC_FILE_LOADGROUNDTEX:
		{
			const char *ptr = mxGetOpenFileName (this, 0, "*.*");
			if (ptr)
			{
				if (0 /* d_MatSysWindow->loadTexture (ptr, event->action - IDC_FILE_LOADBACKGROUNDTEX) */)
				{
					if (event->action == IDC_FILE_LOADBACKGROUNDTEX)
						d_cpl->setShowBackground (true);
					else
						d_cpl->setShowGround (true);

				}
				else
					mxMessageBox (this, "Error loading texture.", g_appTitle, MX_MB_OK | MX_MB_ERROR);
			}
		}
		break;

		case IDC_FILE_UNLOADGROUNDTEX:
		{
			// d_MatSysWindow->loadTexture (0, 1);
			d_cpl->setShowGround (false);
		}
		break;

		case IDC_FILE_RECENTMODELS1:
		case IDC_FILE_RECENTMODELS2:
		case IDC_FILE_RECENTMODELS3:
		case IDC_FILE_RECENTMODELS4:
		case IDC_FILE_RECENTMODELS5:
		case IDC_FILE_RECENTMODELS6:
		case IDC_FILE_RECENTMODELS7:
		case IDC_FILE_RECENTMODELS8:
		{
			int i = event->action - IDC_FILE_RECENTMODELS1;
			LoadModelFile( recentFiles[i] );
		}
		break;

		case IDC_FILE_EXIT:
		{
			redraw ();
			mx::quit ();
		}
		break;

		case IDC_OPTIONS_COLORBACKGROUND:
		case IDC_OPTIONS_COLORGROUND:
		case IDC_OPTIONS_COLORLIGHT:
		case IDC_OPTIONS_COLORAMBIENT:
		{
			float *cols[4] = { g_viewerSettings.bgColor, g_viewerSettings.gColor, g_viewerSettings.lColor, g_viewerSettings.aColor };
			float *col = cols[event->action - IDC_OPTIONS_COLORBACKGROUND];
			int r = (int) (col[0] * 255.0f);
			int g = (int) (col[1] * 255.0f);
			int b = (int) (col[2] * 255.0f);
			if (mxChooseColor (this, &r, &g, &b))
			{
				col[0] = (float) r / 255.0f;
				col[1] = (float) g / 255.0f;
				col[2] = (float) b / 255.0f;
			}
		}
		break;

		case IDC_OPTIONS_CENTERVIEW:
			d_cpl->centerView ();
			break;
		case IDC_OPTIONS_VIEWMODEL:
		{
			d_cpl->viewmodelView();
		}
		break;

		case IDC_OPTIONS_MAKESCREENSHOT:
		{
			char *ptr = (char *) mxGetSaveFileName (this, "", "*.tga");
			if (ptr)
			{
				if (!strstr (ptr, ".tga"))
					strcat (ptr, ".tga");
				d_MatSysWindow->dumpViewport (ptr);
			}
		}
		break;

		case IDC_OPTIONS_DUMP:
			d_cpl->dumpModelInfo ();
			break;

		case IDC_OPTIONS_HIDEMENU:
		{
			m_controlPanelHidden = !m_controlPanelHidden;
			d_cpl->setVisible(!m_controlPanelHidden);

			// Redraw
			mxEvent e;
			e.event = mxEvent::Size;
			e.width = w2();
			e.height = h2();
			handleEvent(&e);

			break;
		}
		case IDC_VIEW_FILEASSOCIATIONS:
			g_FileAssociation->setAssociation (0);
			g_FileAssociation->setVisible (true);
			break;

		case IDC_VIEW_ACTIVITIES:
			g_viewerSettings.showActivities = !g_viewerSettings.showActivities;
			menuView->setChecked( event->action, g_viewerSettings.showActivities );
			d_cpl->initSequenceChoices();
			d_cpl->resetControlPanel();
			break;

		case IDC_VIEW_HIDDEN:
			g_viewerSettings.showHidden = !g_viewerSettings.showHidden;
			menuView->setChecked( event->action, g_viewerSettings.showHidden );
			d_cpl->initSequenceChoices();
			d_cpl->resetControlPanel();
			break;

#ifdef WIN32
		case IDC_HELP_GOTOHOMEPAGE:
			ShellExecute (0, "open", "http://www.swissquake.ch/chumbalum-soft/index.html", 0, 0, SW_SHOW);
			break;
#endif

		case IDC_HELP_ABOUT:
			mxMessageBox (this,
				"Half-Life Model Viewer v2.0 (c) 2004 Valve Corp.\n"
				"Portions (c) 1999 by Mete Ciragan\n\n"
				"Left-drag inside circle to spin.\n"
				"Left-drag outside circle to rotate.\n"
				"Right-drag to zoom.\n"
				"Shift-left-drag to x-y-pan.\n"
				"Shift-right-drag to z-pan.\n"
				"Ctrl-left-drag to move light.\n\n"
				"Build:\t" __DATE__ ".\n"
				"Email:\tmete@swissquake.ch\n"
				"Web:\thttp://www.swissquake.ch/chumbalum-soft/", "About Half-Life Model Viewer",
				MX_MB_OK | MX_MB_INFORMATION);
			break;

		case IDC_ACCEL_WIREFRAME:
			d_cpl->setOverlayWireframe( !g_viewerSettings.overlayWireframe );
			break;

		case IDC_ACCEL_ATTACHMENTS:
			d_cpl->setShowAttachments( !g_viewerSettings.showAttachments );
			break;

		case IDC_ACCEL_GROUND:
			d_cpl->setShowGround( !g_viewerSettings.showGround );
			break;

		case IDC_ACCEL_HITBOXES:
			d_cpl->setShowHitBoxes( !g_viewerSettings.showHitBoxes );
			break;

		case IDC_ACCEL_BONES:
			d_cpl->setShowBones( !g_viewerSettings.showBones );
			break;

		case IDC_ACCEL_BACKGROUND:
			d_cpl->setShowBackground( !g_viewerSettings.showBackground );
			break;

		case IDC_ACCEL_MOVEMENT:
			d_cpl->setShowMovement( !g_viewerSettings.showMovement );
			break;

		case IDC_ACCEL_NORMALS:
			d_cpl->setShowNormals( !g_viewerSettings.showNormals );
			break;

		case IDC_ACCEL_TANGENTS:
			d_cpl->setShowTangentFrame( !g_viewerSettings.showTangentFrame );
			break;

		case IDC_ACCEL_SHADOW:
			d_cpl->setShowShadow( !g_viewerSettings.showShadow );
			break;

		} //switch (event->action)

	} // mxEvent::Action
	break;

	case mxEvent::Size:
	{
		int w = event->width;
		int h = event->height;
		int y = m_controlPanelHidden ? 0 : ((mb && mb->isVisible()) ? mb->getHeight () : 0);
#ifdef WIN32
#define HEIGHT 240
#else
#define HEIGHT 140
		h -= 40;
#endif
		if(m_controlPanelHidden)
			d_MatSysWindow->setBounds(0, 0, w, h);
		else
		{
			d_MatSysWindow->setBounds (0, y, w, h - HEIGHT - y);
			d_cpl->setBounds (0, y + h - HEIGHT, w, HEIGHT);
		}
	}
	break;

	case KeyDown:
		d_MatSysWindow->handleEvent(event);
		d_cpl->handleEvent(event);
	break;

	case mxEvent::Activate:
	{
		// Embedded (vmtstudio hosts this window): the host owns the viewport,
		// so the frame loop must keep running even when the child window is not
		// the active one; clearing the idle window here would freeze playback
		// and stop every redraw until the next command arrives.
		if (event->action || CommandLine()->FindParm("-embed"))
		{
			mx::setIdleWindow( getMatSysWindow() );
		}
		else
		{
			mx::setIdleWindow( 0 );
		}
	}
	break;

	} // event->event

	return 1;
}



void
MDLViewer::redraw ()
{
	/*
	mxEvent event;
	event.event = mxEvent::Size;
	event.width = w2 ();
	event.height = h2 ();
	handleEvent (&event);
	*/
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : int
//-----------------------------------------------------------------------------
int MDLViewer::GetCurrentHitboxSet( void )
{
	return d_cpl ? d_cpl->GetCurrentHitboxSet() : 0;
}

void HLMV_Log(const char* fmt, ...)
{
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	OutputDebugStringA(buf);
	FILE* f = fopen("hlmv_debug.log", "a");
	if (f) {
		fputs(buf, f);
		fclose(f);
	}
}

SpewRetval_t HLMVSpewFunc( SpewType_t spewType, char const *pMsg )
{
	HLMV_Log("[SPEW %d] %s", spewType, pMsg ? pMsg : "");
	g_bInError = true;
	switch (spewType)
	{
	case SPEW_ERROR:
		HLMV_Log("[FATAL ERROR] %s\n", pMsg ? pMsg : "");
		MessageBox(NULL, pMsg, "FATAL ERROR", MB_OK);
		g_bInError = false;
		return SPEW_ABORT;

	default:
		OutputDebugString(pMsg);
		g_bInError = false;
#ifdef _DEBUG
		return spewType == SPEW_ASSERT ? SPEW_DEBUGGER : SPEW_CONTINUE;
#else
		return SPEW_CONTINUE;
#endif
	}
}


//-----------------------------------------------------------------------------
// The application object
//-----------------------------------------------------------------------------
class CHLModelViewerApp : public CSteamAppSystemGroup
{
public:
	// Methods of IApplication
	virtual bool Create();
	virtual bool PreInit();
	virtual int Main();
	virtual void PostShutdown();
	virtual void Destroy();

private:
	// Sets up the search paths
	bool SetupSearchPaths();

	virtual CSysModule *LoadModuleDLL( const char *pDLLName ) override
	{
		HLMV_Log("[HLMV] CHLModelViewerApp::LoadModuleDLL: %s\n", pDLLName);
		HMODULE hMod = LoadLibraryA( pDLLName );
		if (!hMod)
		{
			const char* pBinDir = CommandLine()->ParmValue("-bin", (const char*)0);
			if (pBinDir && pBinDir[0])
			{
				char fullPath[MAX_PATH];
				V_snprintf(fullPath, sizeof(fullPath), "%s\\%s", pBinDir, pDLLName);
				hMod = LoadLibraryA(fullPath);
			}
		}
		HLMV_Log("[HLMV] CHLModelViewerApp::LoadModuleDLL: %s -> %p\n", pDLLName, hMod);
		return (CSysModule*)hMod;
	}
};


//-----------------------------------------------------------------------------
// Create all singleton systems
//-----------------------------------------------------------------------------
bool CHLModelViewerApp::Create()
{
	SpewOutputFunc( HLMVSpewFunc );
	HLMV_Log("[HLMV] CHLModelViewerApp::Create start\n");

	g_dxlevel = CommandLine()->ParmValue( "-dx", 0 );
	g_bOldFileDialogs = ( CommandLine()->FindParm( "-olddialogs" ) != 0 );

	static const char* s_pMatSysVer = MATERIAL_SYSTEM_INTERFACE_VERSION;
	AppModule_t matSysMod = LoadModule( "materialsystem.dll" );
	if ( matSysMod == APP_MODULE_INVALID )
	{
		HLMV_Log("[HLMV] Failed to load module materialsystem.dll!\n");
		return false;
	}

	if ( !AddSystem( matSysMod, s_pMatSysVer ) )
	{
		HLMV_Log("[HLMV] Failed %s, trying %s...\n", s_pMatSysVer, MATERIAL_SYSTEM_INTERFACE_VERSION_OLD);
		s_pMatSysVer = MATERIAL_SYSTEM_INTERFACE_VERSION_OLD;
		if ( !AddSystem( matSysMod, s_pMatSysVer ) )
		{
			HLMV_Log("[HLMV] Failed both VMaterialSystem versions!\n");
			return false;
		}
	}
	HLMV_Log("[HLMV] MaterialSystem registered as %s\n", s_pMatSysVer);

	AppSystemInfo_t appSystems[] = 
	{
		{ "studiorender.dll",		STUDIO_RENDER_INTERFACE_VERSION },
		{ "vphysics.dll",			VPHYSICS_INTERFACE_VERSION },
		{ "datacache.dll",			DATACACHE_INTERFACE_VERSION },
		{ "datacache.dll",			MDLCACHE_INTERFACE_VERSION },
		{ "datacache.dll",			STUDIO_DATA_CACHE_INTERFACE_VERSION },

		{ "", "" }	// Required to terminate the list
	};

	HLMV_Log("[HLMV] calling AddSystems for core systems...\n");
	if ( !AddSystems( appSystems ) ) 
	{
		HLMV_Log("[HLMV] AddSystems FAILED!\n");
		return false;
	}
	HLMV_Log("[HLMV] Core systems added successfully\n");

	// Optional sound systems (disabled on GMod to avoid sound scripting dependencies)
	if ( !IsGModEngine() )
	{
		AppModule_t soundEmitterMod = LoadModule( "soundemittersystem.dll" );
		if ( soundEmitterMod != APP_MODULE_INVALID )
		{
			AddSystem( soundEmitterMod, SOUNDEMITTERSYSTEM_INTERFACE_VERSION );
		}
		AppModule_t soundSystemMod = LoadModule( "soundsystem.dll" );
		if ( soundSystemMod != APP_MODULE_INVALID )
		{
			AddSystem( soundSystemMod, SOUNDSYSTEM_INTERFACE_VERSION );
		}
	}

	g_Factory = GetFactory();

	if ( IsGModEngine() )
	{
		s_pOriginalFactory = g_Factory;
		g_Factory = (CreateInterfaceFn)HLMV_GModFactoryWrapper;
	}

	ConnectTier1Libraries(&g_Factory, 1);
	ConnectTier2Libraries(&g_Factory, 1);

	// Add in the cvar factory
	AppModule_t cvarModule = LoadModule(VStdLib_GetICVarFactory());
	if ( !AddSystem(cvarModule, CVAR_INTERFACE_VERSION) )
	{
		AddSystem(cvarModule, "VEngineCvar007");
	}


	g_pFileSystem = (IFileSystem*)FindSystem( FILESYSTEM_INTERFACE_VERSION );
	if ( IsGModEngine() && g_pFileSystem )
	{
		g_pFileSystem = (IFileSystem*)WrapGModAppSystem( g_pFileSystem );
	}
	g_pMaterialSystem = (IMaterialSystem*)FindSystem( s_pMatSysVer );
	g_pStudioRender = (IStudioRender*)FindSystem( STUDIO_RENDER_INTERFACE_VERSION );
	g_pDataCache = (IDataCache*)FindSystem( DATACACHE_INTERFACE_VERSION );
	g_pMDLCache = (IMDLCache*)FindSystem( MDLCACHE_INTERFACE_VERSION );
	g_pStudioDataCache = (IStudioDataCache*)FindSystem( STUDIO_DATA_CACHE_INTERFACE_VERSION ); 
	physcollision = (IPhysicsCollision *)FindSystem( VPHYSICS_COLLISION_INTERFACE_VERSION );
	physprop = (IPhysicsSurfaceProps *)FindSystem( VPHYSICS_SURFACEPROPS_INTERFACE_VERSION );
	g_pSoundEmitterBase = (ISoundEmitterSystemBase *)FindSystem( SOUNDEMITTERSYSTEM_INTERFACE_VERSION );
	g_pSoundSystem = (ISoundSystem *)FindSystem( SOUNDSYSTEM_INTERFACE_VERSION );

	HLMV_Log("[HLMV] Systems: fs=%p mat=%p studio=%p mdlcache=%p\n",
		g_pFileSystem, g_pMaterialSystem, g_pStudioRender, g_pMDLCache);

	if ( !g_pFileSystem || !physprop || !physcollision || !g_pMaterialSystem || !g_pStudioRender || !g_pMDLCache || !g_pDataCache )
	{
		HLMV_Log("[HLMV] missing required interface!\n");
		Error("Unable to load required library interface!\n");
	}

	if ( IsGModEngine() )
	{
		HLMV_Log("[HLMV] Installing GMod MaterialSystem Proxy...\n");
		g_pMaterialSystem = (IMaterialSystem*)WrapGModMaterialSystem( g_pMaterialSystem );
		HLMV_Log("[HLMV] Installing GMod MDLCache Proxy...\n");
		g_pMDLCache = (IMDLCache*)WrapGModMDLCache( g_pMDLCache );
		HLMV_Log("[HLMV] Installing GMod StudioRender Proxy...\n");
		g_pStudioRender = (IStudioRender*)WrapGModAppSystem( g_pStudioRender );
		HLMV_Log("[HLMV] Installing GMod DataCache Proxy...\n");
		g_pDataCache = (IDataCache*)WrapGModAppSystem( g_pDataCache );
		HLMV_Log("[HLMV] Installing GMod StudioDataCache Proxy...\n");
		g_pStudioDataCache = (IStudioDataCache*)WrapGModAppSystem( g_pStudioDataCache );
	}

	const char *pShaderDLL = CommandLine()->ParmValue("-shaderdll");
	if(!pShaderDLL)
	{
		pShaderDLL = "shaderapidx9.dll";
	}
	char fullShaderPath[MAX_PATH];
	const char *pBinDir = CommandLine()->ParmValue("-bin", (const char*)0);
	if ( IsGModEngine() && pBinDir && pBinDir[0] )
	{
		V_snprintf(fullShaderPath, sizeof(fullShaderPath), "%s\\%s", pBinDir, pShaderDLL);
		pShaderDLL = fullShaderPath;
	}
	HLMV_Log("[HLMV] SetShaderAPI(%s)...\n", pShaderDLL);
	void* esp_before = nullptr;
	void* esp_after = nullptr;
	__asm { mov esp_before, esp }
	g_pMaterialSystem->SetShaderAPI(pShaderDLL);
	__asm { mov esp_after, esp }
	HLMV_Log("[HLMV] SetShaderAPI ok (esp before=%p, after=%p)\n", esp_before, esp_after);
	if ( !IsGModEngine() )
	{
		HLMV_Log("[HLMV] MaterialSystem Connect...\n");
		g_pMaterialSystem->Connect(g_Factory);
		HLMV_Log("[HLMV] MaterialSystem Connect ok\n");
	}

	g_pMaterialSystemHardwareConfig = (IMaterialSystemHardwareConfig*)FindSystem(MATERIALSYSTEM_HARDWARECONFIG_INTERFACE_VERSION);
	if ( !g_pMaterialSystemHardwareConfig )
	{
		g_pMaterialSystemHardwareConfig = (IMaterialSystemHardwareConfig*)FindSystem("MaterialSystemHardwareConfig012");
	}
	if ( !g_pMaterialSystemHardwareConfig && g_Factory )
	{
		g_pMaterialSystemHardwareConfig = (IMaterialSystemHardwareConfig*)g_Factory(MATERIALSYSTEM_HARDWARECONFIG_INTERFACE_VERSION, NULL);
		if ( !g_pMaterialSystemHardwareConfig )
		{
			g_pMaterialSystemHardwareConfig = (IMaterialSystemHardwareConfig*)g_Factory("MaterialSystemHardwareConfig012", NULL);
		}
	}
	HLMV_Log("[HLMV] CHLModelViewerApp::Create done, g_pMaterialSystemHardwareConfig=%p\n", g_pMaterialSystemHardwareConfig);

	int sysCount = *(int*)((uintptr_t)this + 0x24);
	IAppSystem** sysArr = *(IAppSystem***)((uintptr_t)this + 0x18);
	HLMV_Log("[HLMV] Total systems in m_Systems: %d, arr=%p\n", sysCount, sysArr);
	for (int i = 0; i < sysCount; ++i)
	{
		IAppSystem* sys = sysArr[i];
		void** vt = sys ? *(void***)sys : nullptr;
		HLMV_Log("[HLMV]   m_Systems[%d] = %p (vt=%p, slot0=%p)\n", i, sys, vt, vt ? vt[0] : nullptr);
	}

	return true;
}


void CHLModelViewerApp::Destroy()
{
	DisconnectTier1Libraries();
	DisconnectTier2Libraries();

	g_pFileSystem = NULL;
	g_pMaterialSystem = NULL;
	g_pStudioRender = NULL;
	g_pDataCache = NULL;
	g_pMDLCache = NULL;
	g_pStudioDataCache = NULL;
	physcollision = NULL;
	physprop = NULL;
}


//-----------------------------------------------------------------------------
// Sets up the game path
//-----------------------------------------------------------------------------
bool CHLModelViewerApp::SetupSearchPaths()
{
	HLMV_Log("[HLMV] SetupSearchPaths start\n");
	CFSSteamSetupInfo steamInfo;
	steamInfo.m_pDirectoryName = NULL;
	steamInfo.m_bOnlyUseDirectoryName = false;
	steamInfo.m_bToolsMode = true;
	steamInfo.m_bSetSteamDLLPath = true;
	steamInfo.m_bSteam = g_pFileSystem->IsSteam();
	HLMV_Log("[HLMV] FileSystem_SetupSteamEnvironment...\n");
	if ( FileSystem_SetupSteamEnvironment( steamInfo ) != FS_OK )
	{
		HLMV_Log("[HLMV] FileSystem_SetupSteamEnvironment failed!\n");
		return false;
	}
	HLMV_Log("[HLMV] FileSystem_SetupSteamEnvironment ok, GameInfoPath=%s\n", steamInfo.m_GameInfoPath);

	CFSMountContentInfo fsInfo;
	fsInfo.m_pFileSystem = g_pFileSystem;
	fsInfo.m_bToolsMode = true;
	fsInfo.m_pDirectoryName = steamInfo.m_GameInfoPath;

	HLMV_Log("[HLMV] FileSystem_MountContent...\n");
	if ( FileSystem_MountContent( fsInfo ) != FS_OK )
	{
		HLMV_Log("[HLMV] FileSystem_MountContent failed!\n");
		return false;
	}
	HLMV_Log("[HLMV] FileSystem_MountContent ok\n");

	// Finally, load the search paths for the "GAME" path.
	CFSSearchPathsInit searchPathsInit;
	searchPathsInit.m_pDirectoryName = steamInfo.m_GameInfoPath;
	searchPathsInit.m_pFileSystem = fsInfo.m_pFileSystem;
	HLMV_Log("[HLMV] FileSystem_LoadSearchPaths...\n");
	if ( FileSystem_LoadSearchPaths( searchPathsInit ) != FS_OK )
	{
		HLMV_Log("[HLMV] FileSystem_LoadSearchPaths failed!\n");
		return false;
	}
	HLMV_Log("[HLMV] FileSystem_LoadSearchPaths ok\n");

	char platform[MAX_PATH];
	Q_strncpy( platform, steamInfo.m_GameInfoPath, MAX_PATH );
	Q_StripTrailingSlash( platform );
	Q_strncat( platform, "/../platform", MAX_PATH, MAX_PATH );

	fsInfo.m_pFileSystem->AddSearchPath( platform, "PLATFORM" );
	HLMV_Log("[HLMV] SetupSearchPaths complete\n");

	return true;
}


//-----------------------------------------------------------------------------
// Init, shutdown
//-----------------------------------------------------------------------------
bool CHLModelViewerApp::PreInit( )
{
	HLMV_Log("[HLMV] CHLModelViewerApp::PreInit start\n");
	MathLib_Init( 2.2f, 2.2f, 0.0f, 2.0f, false, false, false, false );

	// Add paths...
	if ( !SetupSearchPaths() )
	{
		HLMV_Log("[HLMV] SetupSearchPaths failed!\n");
		return false;
	}
	HLMV_Log("[HLMV] SetupSearchPaths ok\n");

	// Get the adapter from the command line....
	const char *pAdapterString;
	int nAdapter = 0;
	if (CommandLine()->CheckParm( "-adapter", &pAdapterString ))
	{
		nAdapter = atoi( pAdapterString );
	}

	int nAdapterFlags = 0;
	if ( CommandLine()->CheckParm( "-ref" ) )
	{
		nAdapterFlags |= MATERIAL_INIT_REFERENCE_RASTERIZER;
	}

	g_pMaterialSystem->SetAdapter( nAdapter, nAdapterFlags );
	HLMV_Log("[HLMV] SetAdapter ok\n");

	if ( !g_pFileSystem->IsSteam() || CommandLine()->FindParm( "-OldDialogs" ) )
		g_bOldFileDialogs = true;
	
	LoadFileSystemDialogModule();
	HLMV_Log("[HLMV] PreInit finished\n");

	return true; 
}

void CHLModelViewerApp::PostShutdown()
{
	UnloadFileSystemDialogModule();
}


//-----------------------------------------------------------------------------
// main application
//-----------------------------------------------------------------------------
int CHLModelViewerApp::Main()
{
	HLMV_Log("[HLMV] Main start\n");
	g_pMaterialSystem->ModInit();
	HLMV_Log("[HLMV] MatSys ModInit ok\n");
	if ( g_pSoundEmitterBase )
	{
		g_pSoundEmitterBase->ModInit();
		HLMV_Log("[HLMV] SoundEmitter ok\n");
	}

	g_pDataCache->SetSize( 64 * 1024 * 1024 );

	HLMV_Log("[HLMV] creating MDLViewer...\n");
	g_MDLViewer = new MDLViewer ();
	HLMV_Log("[HLMV] MDLViewer created, handle=%p\n", g_MDLViewer->getHandle());
	// Do not call setMenuBar; the menu bar is unnecessary in backend mode.

	extern void BackendIPC_Init(HWND hWndMain);
	const bool thumbMode = CommandLine()->FindParm( "-thumb" ) != 0;
	if ( !thumbMode )
	{
		BackendIPC_Init((HWND)g_MDLViewer->getHandle());
		HLMV_Log("[HLMV] BackendIPC_Init done\n");
	}

	g_pStudioModel->Init();
	g_pStudioModel->ModelInit();
	g_pStudioModel->SetHeadTarget( Vector( 0, 0, 0 ), 1.0 );
	HLMV_Log("[HLMV] StudioModel init done\n");

	// Load up the initial model
	const char *pMdlName = NULL;
	int nParmCount = CommandLine()->ParmCount();
	if ( nParmCount > 1 )
	{
		pMdlName = CommandLine()->GetParm( nParmCount - 1 );
	}

	if ( thumbMode )
	{
		const char *thumbOut = CommandLine()->ParmValue( "-thumb", (const char*)0 );
		const char *thumbModel = CommandLine()->ParmValue( "-model", (const char*)0 );
		if ( (!thumbModel || !thumbModel[0]) && pMdlName && Q_stristr( pMdlName, ".mdl" ) )
		{
			thumbModel = pMdlName;
		}
		const int thumbSize = CommandLine()->ParmValue( "-size", 256 );
		const bool ok = thumbOut && thumbOut[0] &&
			g_MDLViewer->CaptureModelThumbnail( thumbOut, thumbSize, thumbModel );
		g_thumbExitCode = ok ? 0 : 2;
		// The normal shutdown path corrupts the heap with TF2's material
		// proxies (0xC0000374). This is a one-shot helper process whose only
		// output is the BMP, so terminate it cleanly right after writing it.
		TerminateProcess( GetCurrentProcess(), (UINT)g_thumbExitCode );
	}
	else if ( pMdlName && Q_stristr( pMdlName, ".mdl" ) )
	{
		char absPath[MAX_PATH];
		Q_MakeAbsolutePath( absPath, sizeof( absPath ), pMdlName );

		if ( CommandLine()->FindParm( "-screenshot" ) )
		{
			g_MDLViewer->SaveScreenShot( absPath );
		}
		else if ( CommandLine()->FindParm( "-dump" ) )
		{
			g_MDLViewer->DumpText( absPath );
		}
		else
		{
			g_MDLViewer->LoadModelFile( absPath );
		}
	}

	HLMV_Log("[HLMV] entering mx::run()...\n");
	int nRetVal = mx::run ();
	HLMV_Log("[HLMV] mx::run() finished with %d\n", nRetVal);

	g_pStudioModel->Shutdown();
	g_pMaterialSystem->ModShutdown();

	return g_thumbExitCode >= 0 ? g_thumbExitCode : nRetVal;
}



static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* pEx)
{
	if (pEx && pEx->ExceptionRecord) {
		void* addr = pEx->ExceptionRecord->ExceptionAddress;
		HMODULE hMod = NULL;
		char modName[MAX_PATH] = "<unknown>";
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)addr, &hMod) && hMod) {
			GetModuleFileNameA(hMod, modName, sizeof(modName));
			char* slash = strrchr(modName, '\\');
			if (slash) memmove(modName, slash + 1, strlen(slash));
		}
		HLMV_Log("[CRASH] Exception 0x%08X at %s+%p (abs: %p)\n",
			pEx->ExceptionRecord->ExceptionCode,
			modName, (void*)((uintptr_t)addr - (uintptr_t)hMod), addr);

		if (pEx->ExceptionRecord->NumberParameters >= 2) {
			HLMV_Log("[CRASH] Access violation %s address %p\n",
				pEx->ExceptionRecord->ExceptionInformation[0] == 1 ? "writing to" :
				pEx->ExceptionRecord->ExceptionInformation[0] == 8 ? "executing" : "reading",
				(void*)pEx->ExceptionRecord->ExceptionInformation[1]);
		}

		if (pEx->ContextRecord) {
			HLMV_Log("[CRASH] Registers: EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X ESP=%08X EBP=%08X EIP=%08X\n",
				pEx->ContextRecord->Eax, pEx->ContextRecord->Ebx, pEx->ContextRecord->Ecx, pEx->ContextRecord->Edx,
				pEx->ContextRecord->Esi, pEx->ContextRecord->Edi, pEx->ContextRecord->Esp, pEx->ContextRecord->Ebp,
				pEx->ContextRecord->Eip);
		}

		// Log stack frames
		if (pEx->ContextRecord) {
			uintptr_t* esp = (uintptr_t*)pEx->ContextRecord->Esp;
			for (int i = 0; i < 32; i++) {
				if (IsBadReadPtr(&esp[i], sizeof(uintptr_t))) break;
				uintptr_t ptr = esp[i];
				HMODULE hFrameMod = NULL;
				if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)ptr, &hFrameMod) && hFrameMod) {
					char frameMod[MAX_PATH] = "";
					GetModuleFileNameA(hFrameMod, frameMod, sizeof(frameMod));
					char* sl = strrchr(frameMod, '\\');
					HLMV_Log("[CRASH]   stack[%d] = %s+0x%X\n", i, sl ? sl + 1 : frameMod, ptr - (uintptr_t)hFrameMod);
				}
			}
		}
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

//-----------------------------------------------------------------------------
//   Main entry point
//-----------------------------------------------------------------------------
int main (int argc, char *argv[])
{
	remove("hlmv_debug.log");
	SetUnhandledExceptionFilter(CrashFilter);
	HLMV_Log("[HLMV] main() started, argc=%d\n", argc);

	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "-bin") == 0 && i + 1 < argc) {
			SetDllDirectoryA(argv[i + 1]);
			HLMV_Log("[HLMV] Early SetDllDirectoryA: %s\n", argv[i + 1]);
			break;
		}
	}

	CommandLine()->CreateCmdLine( argc, argv );

	const char *pBinDir = CommandLine()->ParmValue( "-bin", (const char*)0 );
	HLMV_Log("[HLMV] -bin: %s\n", pBinDir ? pBinDir : "null");
	if ( pBinDir && pBinDir[0] )
	{
		SetDllDirectoryA( pBinDir );
	}

	FileSystem_UseVProjectBinDir( true );

	HLMV_Log("[HLMV] calling mx::init\n");
	mx::init( argc, argv );
	HLMV_Log("[HLMV] mx::init returned\n");

	// make sure we start in the right directory
	char szName[256];
	strcpy( szName, mx::getApplicationPath() );
	// mx_setcwd (szName);

class CSteamApplicationGMod : public CSteamApplication
{
public:
	CSteamApplicationGMod( CSteamAppSystemGroup *pAppSystemGroup ) : CSteamApplication( pAppSystemGroup ) {}

	virtual bool Create() override
	{
		HLMV_Log("[HLMV] CSteamApplication::Create start\n");
		bool ok = CSteamApplication::Create();
		HLMV_Log("[HLMV] CSteamApplication::Create returned %d (m_pFileSystem=%p)\n", ok, m_pFileSystem);
		if ( ok && m_pFileSystem && IsGModEngine() )
		{
			m_pFileSystem = (IFileSystem*)WrapGModAppSystem( m_pFileSystem );
		}
		return ok;
	}

	virtual bool PreInit() override
	{
		HLMV_Log("[HLMV] CSteamApplication::PreInit start\n");
		bool ok = CSteamApplication::PreInit();
		HLMV_Log("[HLMV] CSteamApplication::PreInit returned %d\n", ok);
		return ok;
	}

	virtual int Main() override
	{
		HLMV_Log("[HLMV] CSteamApplication::Main start\n");
		int ret = CSteamApplication::Main();
		HLMV_Log("[HLMV] CSteamApplication::Main returned %d\n", ret);
		return ret;
	}
};

	HLMV_Log("[HLMV] running CSteamApplication\n");
	CHLModelViewerApp hlmodelviewerApp;
	CSteamApplicationGMod steamApplication( &hlmodelviewerApp );
	HLMV_Log("[HLMV] &hlmodelviewerApp=%p, &steamApplication=%p\n", &hlmodelviewerApp, &steamApplication);
	int ret = steamApplication.Run();
	HLMV_Log("[HLMV] steamApplication.Run returned %d\n", ret);
	return ret;
}
