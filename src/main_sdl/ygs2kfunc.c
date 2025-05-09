#include "main_sdl/include.h"
#include "SDL_kanji.h"
#include "ygs2kfunc.h"
#include "ygs2kprivate.h"
#include "filesystem.h"
#include "nanotime.h"

#define		YGS_GAME_CAPTION		"Heboris C.E."

typedef struct YGS2kSTextLayer
{
	bool enable;
	int x;
	int y;
	SDL_Color color;
	int size;
	char string[256];
	bool updateTexture;
	SDL_Texture* texture;
	int textureW;
	int textureH;
} YGS2kSTextLayer;

typedef struct YGS2kSWave
{
	Mix_Chunk *chunk;
	int loops;
} YGS2kSWave;

static bool 			s_bInitFast = false;
static SDL_Window		*s_pScreenWindow = NULL;
static SDL_Renderer		*s_pScreenRenderer = NULL;
static SDL_Texture		*s_pScreenRenderTarget = NULL;
static float			s_fScreenSubpixelOffset = 0.0f;

static SDL_Texture		*s_pTexture[YGS_TEXTURE_MAX];

static YGS2kSWave		s_Wave[YGS_WAVE_MAX];
static Mix_Music		*s_pMusic;
static bool			s_bWaveFormatSupported[YGS_WAVE_MAXFORMAT];

#define		YGS_KANJIFONTFILE_MAX	3
static Kanji_Font		*s_pKanjiFont[YGS_KANJIFONTFILE_MAX];

static int			s_iLogicalWidth;
static int			s_iLogicalHeight;

static bool			s_bNoFrameskip;
static nanotime_step_data	s_StepData = { 0 };
static bool			s_bLastFrameSkipped;
static uint64_t			s_uFPSCount;
static unsigned int		s_uFPSCnting;
static unsigned int		s_uFPS;
static unsigned int		s_uNowFPS;
static unsigned int		s_uCursorCnting;

static YGS2kSTextLayer		s_TextLayer[YGS_TEXTLAYER_MAX];
static int32_t			s_iScreenMode;
static int32_t			s_iScreenIndex;

static int			s_iNewOffsetX = 0, s_iNewOffsetY = 0;
static int			s_iOffsetX = 0, s_iOffsetY = 0;

static int			s_iQuitLevel;

static void YGS2kPrivateKanjiFontFinalize();
static void YGS2kPrivateKanjiFontInitialize();

static float YGS2kGetScreenSubpixelOffset()
{
	// The returned subpixel offset nudges all draws to have pixel coordinates
	// that end up centered in the floating point coordinate space. Without
	// this offset, pixel coordinates are at the upper left of the intended
	// pixels, resulting in off-by-one drawing errors sometimes.
	//
	// The numerator of the return value is the fraction of a pixel to adjust
	// draw position rightwards and downwards, and the division by the current
	// scale converts that subpixel amount to the amount to adjust by within
	// the currently set logical resolution (320x240 or 640x480), so it adjusts
	// by less than 0.375 if the render resolution is above logical resolution,
	// or greater than 0.375 if below logical resolution. If the numerator is
	// exactly 0.5f, then system-dependent rounding errors can occur, because
	// the coordinate is located exactly in the pixel center, producing
	// sampling artifacts on some systems, but not on others. By using a
	// numerator less than 0.5f, the rounding of draw coordinates by the
	// graphics implementation should place drawn pixels in the center. I also
	// saw scaling artifacts happen with a numerator of 0.25f, so maybe the
	// numerator needs to be somewhere in the open range (0.25f, 0.5f) to
	// likely work everywhere; for now, the midpoint, 0.375f, is used, and
	// seems to work correctly on some systems I've tested.
	//
	// Getting the scale from SDL_RenderGetScale is always appropriate here,
	// being an integer value when integer scaling is in effect, or a
	// non-integer scale when fill-screen scaling is in effect. Even if the
	// scale value is below 1.0f, the formula is still correct.
	//
	// -Brandon McGriff <nightmareci@gmail.com>
	if ( s_pScreenRenderer )
	{
		float scale;
		SDL_RenderGetScale(s_pScreenRenderer, &scale, NULL);
		return 0.375f / scale;
	}
	else
	{
		return 0.0f;
	}
}

void YGS2kInit(const int soundBufferSize)
{
	if (soundBufferSize <= 0) {
		YGS2kExit(EXIT_FAILURE);
	}

	s_iQuitLevel = 0;

#ifdef __EMSCRIPTEN__
	/* Keyboard input gets a bit broken without this hint set. */
	//SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas");
#endif

	/* SDLの初期化 || SDL initialization */
	if ( !s_bInitFast && SDL_Init(
		SDL_INIT_AUDIO | SDL_INIT_VIDEO
		#ifdef ENABLE_JOYSTICK
		| SDL_INIT_JOYSTICK
		#endif
		#ifdef ENABLE_GAME_CONTROLLER
		| SDL_INIT_GAMECONTROLLER
		#endif
	) < 0 )
	{
		fprintf(stderr, "Couldn't initialize SDL: %s\n", SDL_GetError());
		YGS2kExit(EXIT_FAILURE);
	}

	// If this fails, it doesn't matter, the game will still work. But it's
	// called because if it works, the game might perform better.
	if (!s_bInitFast) SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);

	s_iQuitLevel++;
	if (!s_bInitFast) SDL_SetHint(SDL_HINT_RENDER_BATCHING, "1");

	/* 画像の初期化 || Image initialization */
	if ( !s_bInitFast && IMG_Init(IMG_INIT_PNG) != IMG_INIT_PNG )
	{
		fprintf(stderr, "Couldn't initialize image support: %s\n", IMG_GetError());
		YGS2kExit(EXIT_FAILURE);
	}
	s_iQuitLevel++;

	/* サウンドの初期化 || Sound initialization */
	if (!s_bInitFast) {
		const int formatsInitialized = Mix_Init(
			MIX_INIT_MID |
			MIX_INIT_OGG |
			MIX_INIT_MP3 |
			MIX_INIT_FLAC |
			MIX_INIT_OPUS |
			MIX_INIT_MOD
		);
		if ( !formatsInitialized )
		{
			fprintf(stderr, "Couldn't initialize sound support: %s\n", Mix_GetError());
			YGS2kExit(EXIT_FAILURE);
		}

		s_bWaveFormatSupported[YGS_WAVE_MID] = !!(formatsInitialized & MIX_INIT_MID);
		s_bWaveFormatSupported[YGS_WAVE_WAV] = 1; // WAVEはいつでも利用可能 || WAVE is always supported
		s_bWaveFormatSupported[YGS_WAVE_OGG] = !!(formatsInitialized & MIX_INIT_OGG);
		s_bWaveFormatSupported[YGS_WAVE_MP3] = !!(formatsInitialized & MIX_INIT_MP3);
		s_bWaveFormatSupported[YGS_WAVE_FLAC] = !!(formatsInitialized & MIX_INIT_FLAC);
		s_bWaveFormatSupported[YGS_WAVE_OPUS] = !!(formatsInitialized & MIX_INIT_OPUS);
		s_bWaveFormatSupported[YGS_WAVE_MOD] = !!(formatsInitialized & MIX_INIT_MOD);
		s_bWaveFormatSupported[YGS_WAVE_IT] = !!(formatsInitialized & MIX_INIT_MOD);
		s_bWaveFormatSupported[YGS_WAVE_XM] = !!(formatsInitialized & MIX_INIT_MOD);
		s_bWaveFormatSupported[YGS_WAVE_S3M] = !!(formatsInitialized & MIX_INIT_MOD);
	}
	s_iQuitLevel++;

	if (!s_bInitFast) {
#ifdef __EMSCRIPTEN__
		// In testing on desktop Linux, Firefox seems to cope with a 1024 byte
		// buffer fine, but Chrome produces a fair bit of audio breakup. 2048
		// seems to work fine in both, though.
		if ( Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0 )
#else
		// A 1024 byte buffer seems to be a good choice for all native code
		// ports. It's reasonably low latency but doesn't result in audio
		// breakup.
		if ( Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 1024) < 0 )
#endif
		{
			fprintf(stderr, "Couldn't open audio: %s\n", Mix_GetError());
			YGS2kExit(EXIT_FAILURE);
		}
	}
	s_iQuitLevel++;

	if (!s_bInitFast) Mix_AllocateChannels(100);

	int		configChanged = 0;

	s_iNewOffsetX = 0;	s_iNewOffsetY = 0;
	s_iOffsetX = 0;		s_iOffsetY = 0;

	YGS2kSetFPS(60);

	if (!s_bInitFast) YGS2kInputsOpen();

	/* テクスチャ領域の初期化 || Initialize the texture pointers */
	if (!s_bInitFast) memset(s_pTexture, 0, sizeof(s_pTexture));

	/* サウンドの初期化 || Initialize sound data */
	for ( int i = 0 ; i < YGS_WAVE_MAX ; i ++ )
	{
		if ( !s_bInitFast )
		{
			s_Wave[i] = (YGS2kSWave) { 0 };
		}
	}

	if ( !s_bInitFast ) s_pMusic = NULL;

	/* テキストレイヤーの初期化 || Initialize the text layers */
	for ( int i = 0 ; i < YGS_TEXTLAYER_MAX ; i ++ )
	{
		memset(&s_TextLayer[i], 0, sizeof(YGS2kSTextLayer));
		s_TextLayer[i].color.r = s_TextLayer[i].color.g = s_TextLayer[i].color.b = 255;
		s_TextLayer[i].color.a = SDL_ALPHA_OPAQUE;
		s_TextLayer[i].size = 16;
	}

	if ( !s_bInitFast ) YGS2kPrivateKanjiFontInitialize();

	s_bLastFrameSkipped = false;
	s_uFPSCount = 0u;
	s_uFPS			= 0;
	s_bNoFrameskip		= false;

	srand((unsigned)time(NULL));
	s_bInitFast = true;
}

void YGS2kDeinit()
{
	YGS2kInputsClose();

	/* サウンドの解放 */
	for ( int i = 0 ; i < YGS_WAVE_MAX ; i ++ )
	{
		if ( s_Wave[i].chunk )
		{
			Mix_FreeChunk(s_Wave[i].chunk);
			s_Wave[i].chunk = NULL;
		}
	}
	if ( s_pMusic )
	{
		Mix_FreeMusic(s_pMusic);
		s_pMusic = NULL;
	}

	if ( s_pScreenRenderer )
	{
		for ( int i = 0 ; i < YGS_TEXTLAYER_MAX ; i ++ )
		{
			if ( s_TextLayer[i].texture )
			{
				SDL_DestroyTexture(s_TextLayer[i].texture);
				s_TextLayer[i].texture = NULL;
			}
		}
		memset(s_TextLayer, 0, sizeof(s_TextLayer));

		/* テクスチャ領域の解放 */
		for ( int i = 0 ; i < YGS_TEXTURE_MAX ; i ++ )
		{
			if ( s_pTexture[i] )
			{
				SDL_DestroyTexture(s_pTexture[i]);
				s_pTexture[i] = NULL;
			}
		}
	}

	if ( s_pScreenRenderer ) {
		SDL_SetRenderTarget( s_pScreenRenderer, NULL );
	}
	if ( s_pScreenRenderTarget ) {
		SDL_DestroyTexture( s_pScreenRenderTarget );
		s_pScreenRenderTarget = NULL;
	}
	if ( s_pScreenRenderer ) {
		SDL_DestroyRenderer( s_pScreenRenderer );
		s_pScreenRenderer = NULL;
	}

	if ( s_pScreenWindow ) {
		SDL_DestroyWindow( s_pScreenWindow );
		s_pScreenWindow = NULL;
	}

	YGS2kPrivateKanjiFontFinalize();

	switch ( s_iQuitLevel )
	{
	case 4: Mix_CloseAudio();
	case 3: Mix_Quit();
	case 2: IMG_Quit();
	case 1: SDL_Quit();
	default: break;
	}
	s_iQuitLevel = 0;

	s_bInitFast = false;
}

void YGS2kExit(int exitStatus)
{
	YGS2kDeinit();
	FSDeInit();
	exit(exitStatus);
}

bool YGS2kHalt()
{
	if ( s_pScreenRenderer )
	{
		SDL_RenderFlush( s_pScreenRenderer );

		#ifndef NDEBUG
		s_bNoFrameskip = true;
		#endif
		if ( s_bNoFrameskip )
		{
			/* バックサーフェスをフロントに転送 */
			if ( s_pScreenRenderTarget )
			{
				SDL_SetRenderTarget(s_pScreenRenderer, NULL);
				SDL_RenderClear( s_pScreenRenderer );
				SDL_RenderCopy(s_pScreenRenderer, s_pScreenRenderTarget, NULL, NULL);
				SDL_RenderPresent(s_pScreenRenderer);
				SDL_SetRenderTarget(s_pScreenRenderer, s_pScreenRenderTarget);
			}
			else {
				SDL_RenderPresent(s_pScreenRenderer);
			}

			/* フレームレート待ち || Frame rate waiting */
			s_bLastFrameSkipped = !nanotime_step(&s_StepData);

			/* 画面塗りつぶし || Fill screen */
			SDL_RenderClear( s_pScreenRenderer );
		}
		else
		{
			if ( !s_bLastFrameSkipped )
			{
				/* バックサーフェスをフロントに転送 */
				if ( s_pScreenRenderTarget )
				{
					SDL_SetRenderTarget(s_pScreenRenderer, NULL);
					SDL_RenderClear( s_pScreenRenderer );
					SDL_RenderCopy(s_pScreenRenderer, s_pScreenRenderTarget, NULL, NULL);
					SDL_RenderPresent(s_pScreenRenderer);
					SDL_SetRenderTarget(s_pScreenRenderer, s_pScreenRenderTarget);
				}
				else {
					SDL_RenderPresent(s_pScreenRenderer);
				}

				/* 画面塗りつぶし */
				SDL_RenderClear( s_pScreenRenderer );
			}

			/* フレームレート待ち || Frame rate waiting */
			s_bLastFrameSkipped = !nanotime_step(&s_StepData);
		}
	}

	/* フレームレート計算 || Frame rate calculation */
	s_uFPSCnting ++;

	if ( nanotime_interval(s_uFPSCount, nanotime_now(), s_StepData.now_max) >= NANOTIME_NSEC_PER_SEC )
	{
		s_uFPS = s_uFPSCnting;
		s_uFPSCnting = 0;
		s_uFPSCount = nanotime_now();
	}

	/* イベント処理 || Process events */

	SDL_PumpEvents();
	SDL_Event	ev;
	int showCursor = SDL_DISABLE;
	#if defined(ENABLE_JOYSTICK) || defined(ENABLE_GAME_CONTROLLER)
	bool slotsChanged = false;
	#endif
	while (SDL_PeepEvents(&ev, 1, SDL_GETEVENT, 0, SDL_LASTEVENT) == 1)
	{
		switch(ev.type){
			// ウィンドウの×ボタンが押された時など || When the window's X-button is pressed, etc.
			case SDL_QUIT:
				return false;

			case SDL_WINDOWEVENT:
				if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
					s_fScreenSubpixelOffset = SCREEN_SUBPIXEL_OFFSET;
				}
				break;

			#ifdef ENABLE_JOYSTICK
			case SDL_JOYDEVICEADDED:
			case SDL_JOYDEVICEREMOVED:
				slotsChanged = true;
			#endif

			#ifdef ENABLE_GAME_CONTROLLER
			case SDL_CONTROLLERDEVICEADDED:
			case SDL_CONTROLLERDEVICEREMOVED:
				slotsChanged = true;
				break;
			#endif

			case SDL_MOUSEMOTION:
			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP:
			case SDL_MOUSEWHEEL:
				showCursor = SDL_ENABLE;
				break;
			
			default:
				break;
		}
	}

	if (showCursor == SDL_ENABLE)
	{
		SDL_ShowCursor(SDL_ENABLE);
		s_uCursorCnting = 0u;
	}

	if (SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE && s_uCursorCnting++ >= s_uNowFPS)
	{
		SDL_ShowCursor(SDL_DISABLE);
	}

	#if defined(ENABLE_JOYSTICK) || defined(ENABLE_GAME_CONTROLLER)
	if (slotsChanged) {
		if (!YGS2kPlayerSlotsChanged()) {
			YGS2kExit(EXIT_FAILURE);
		}
	}
	#endif

	/* 画面ずらし量の反映 */
	s_iOffsetX = s_iNewOffsetX;
	s_iOffsetY = s_iNewOffsetY;

	return true;
}

int YGS2kIsPlayMusic()
{
	return Mix_PlayingMusic();
}

#ifdef __EMSCRIPTEN__
static EM_BOOL YGS2kEmscriptenResizeCallback(int eventType, const EmscriptenUiEvent* uiEvent, void* userData)
{
	if (
		eventType != EMSCRIPTEN_EVENT_RESIZE ||
		!s_pScreenWindow
	) {
		return false;
	}

	SDL_SetWindowSize(s_pScreenWindow, uiEvent->windowInnerWidth, uiEvent->windowInnerHeight);
	return true;
}
#endif

bool YGS2kSetScreen(YGS2kEScreenModeFlag *screenMode, int32_t *screenIndex)
{
	Uint32		windowFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
	int		windowX, windowY;
	int		logicalWidth, logicalHeight;

	/* 画面の設定 || Set up the screen */

	/* Validate the window type */
	YGS2kEScreenModeFlag windowType = *screenMode & YGS_SCREENMODE_WINDOWTYPE;
	if ( windowType < 0 || windowType >= YGS_SCREENMODE_NUMWINDOWTYPES )
	{
		goto fail;
	}

	int displayIndex = YGS_SCREENINDEX_DISPLAY_TOVALUE(*screenIndex);
	int modeIndex = YGS_SCREENINDEX_MODE_TOVALUE(*screenIndex);
	int numVideoDisplays = SDL_GetNumVideoDisplays();
	int numDisplayModes = SDL_GetNumDisplayModes(displayIndex);
	if ( numVideoDisplays < 0 || numDisplayModes < 0 )
	{
		goto fail;
	}

	if (
		displayIndex >= numVideoDisplays ||
		((windowType == YGS_SCREENMODE_FULLSCREEN || windowType == YGS_SCREENMODE_FULLSCREEN_DESKTOP) && modeIndex >= numDisplayModes)
	) {
		*screenMode = DEFAULT_SCREEN_MODE;
		*screenIndex = 0;
		windowX = SDL_WINDOWPOS_CENTERED_DISPLAY(*screenIndex);
		windowY = SDL_WINDOWPOS_CENTERED_DISPLAY(*screenIndex);
	}
	else
	{
		windowX = SDL_WINDOWPOS_CENTERED_DISPLAY(displayIndex);
		windowY = SDL_WINDOWPOS_CENTERED_DISPLAY(displayIndex);
		switch ( windowType & YGS_SCREENMODE_WINDOWTYPE )
		{
		case YGS_SCREENMODE_WINDOW_MAXIMIZED:
			windowFlags |= SDL_WINDOW_MAXIMIZED;
			break;

		case YGS_SCREENMODE_FULLSCREEN_DESKTOP:
			windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
			break;

		case YGS_SCREENMODE_FULLSCREEN: {
			windowFlags |= SDL_WINDOW_FULLSCREEN;
			break;
		}
		}
	}

	if ( *screenMode & YGS_SCREENMODE_DETAILLEVEL )
	{
		logicalWidth  = 640;
		logicalHeight = 480;
	}
	else
	{
		logicalWidth  = 320;
		logicalHeight = 240;
	}

	s_iLogicalWidth  = logicalWidth;
	s_iLogicalHeight = logicalHeight;

	/* ウィンドウの作成 || Create and set up the window */
	if (
		windowType == YGS_SCREENMODE_FULLSCREEN ||
		windowType == YGS_SCREENMODE_FULLSCREEN_DESKTOP
	)
	{
		SDL_DisplayMode displayMode;
		int status;
		if ( windowType == YGS_SCREENMODE_FULLSCREEN )
		{
			status = SDL_GetDisplayMode(displayIndex, modeIndex, &displayMode);
		}
		else
		{
			status = SDL_GetDesktopDisplayMode(displayIndex, &displayMode);
		}
		if ( status < 0 )
		{
			*screenMode &= ~YGS_SCREENMODE_WINDOWTYPE;
			*screenMode |= YGS_SCREENMODE_WINDOW;
			*screenIndex = 0;
			if ( !s_pScreenWindow )
			{
				s_pScreenWindow = SDL_CreateWindow(
					YGS_GAME_CAPTION,
					SDL_WINDOWPOS_CENTERED_DISPLAY(displayIndex),
					SDL_WINDOWPOS_CENTERED_DISPLAY(displayIndex),
					logicalWidth,
					logicalHeight,
					SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE
				);
				if ( !s_pScreenWindow )
				{
					goto fail;
				}
			}
			else
			{
				SDL_RestoreWindow(s_pScreenWindow);
				SDL_SetWindowSize(s_pScreenWindow, logicalWidth, logicalHeight);
				SDL_SetWindowResizable(s_pScreenWindow, SDL_TRUE);
				SDL_SetWindowPosition(s_pScreenWindow, SDL_WINDOWPOS_CENTERED_DISPLAY(displayIndex), SDL_WINDOWPOS_CENTERED_DISPLAY(displayIndex));
				if ( SDL_SetWindowFullscreen(s_pScreenWindow, 0u) < 0 )
				{
					goto fail;
				}
			}
		}
		else {
			if ( !s_pScreenWindow )
			{
				s_pScreenWindow = SDL_CreateWindow(YGS_GAME_CAPTION, windowX, windowY, displayMode.w, displayMode.h, windowFlags);
				if ( !s_pScreenWindow )
				{
					goto fail;
				}
			}
			else
			{
				SDL_SetWindowPosition(s_pScreenWindow, windowX, windowY);
				int fullscreenError = -1;
				if ( (windowFlags & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP )
				{
					fullscreenError = SDL_SetWindowFullscreen(s_pScreenWindow, SDL_WINDOW_FULLSCREEN_DESKTOP);
				}
				else if ( (windowFlags & SDL_WINDOW_FULLSCREEN) == SDL_WINDOW_FULLSCREEN )
				{
					fullscreenError = SDL_SetWindowFullscreen(s_pScreenWindow, SDL_WINDOW_FULLSCREEN);
				}
				if ( fullscreenError < 0 )
				{
					goto fail;
				}
			}
			if ( SDL_SetWindowDisplayMode(s_pScreenWindow, &displayMode) < 0 )
			{
				goto fail;
			}
		}
	}
	else if (
		windowType == YGS_SCREENMODE_WINDOW ||
		windowType == YGS_SCREENMODE_WINDOW_MAXIMIZED
	)
	{
		SDL_DisplayMode displayMode;
		if ( SDL_GetDesktopDisplayMode(displayIndex, &displayMode) < 0 )
		{
			goto fail;
		}

		int maxScale;
		if ( displayMode.w <= logicalWidth || displayMode.h <= logicalHeight )
		{
			maxScale = 1;
		}
		else if ( displayMode.w > displayMode.h )
		{
			maxScale = (displayMode.h / logicalHeight) - (displayMode.h % logicalHeight == 0);
		}
		else
		{
			maxScale = (displayMode.w / logicalWidth) - (displayMode.w % logicalWidth == 0);
		}
		int maxWidth = maxScale * logicalWidth;
		int maxHeight = maxScale * logicalHeight;
		int scale = modeIndex + 1;
		int windowW = scale * logicalWidth;
		int windowH = scale * logicalHeight;
		if ( scale > maxScale) {
			windowW = logicalWidth;
			windowH = logicalHeight;
			*screenIndex = YGS_SCREENINDEX_TOSETTING(displayIndex, 0);
		}
		if ( !s_pScreenWindow )
		{
			s_pScreenWindow = SDL_CreateWindow(YGS_GAME_CAPTION, windowX, windowY, windowW, windowH, windowFlags);
			if ( !s_pScreenWindow )
			{
				goto fail;
			}
		}
		else
		{
			if ( windowFlags & SDL_WINDOW_MAXIMIZED ) {
				SDL_MaximizeWindow(s_pScreenWindow);
			}
			else {
				SDL_RestoreWindow(s_pScreenWindow);
			}
			SDL_SetWindowResizable(s_pScreenWindow, SDL_TRUE);
			if ( SDL_SetWindowFullscreen(s_pScreenWindow, 0) < 0 )
			{
				goto fail;
			}
			SDL_SetWindowSize(s_pScreenWindow, windowW, windowH);
			SDL_SetWindowPosition(s_pScreenWindow, windowX, windowY);
		}
	}

	// Create the renderer, if not already created. It's important to not
	// recreate the renderer if it's already created, so restarting without
	// changing the detail level doesn't require reloading graphics. If the
	// renderer were destroyed/created anew every restart, it would be required
	// to reload the graphics every restart, even when detail level isn't
	// changed.
	// TODO: fix to allow rendering to the texture.
	if ( !s_pScreenRenderer )
	{
		
		s_pScreenRenderer = SDL_CreateRenderer(s_pScreenWindow, -1, SDL_RENDERER_TARGETTEXTURE); // ask for render to texture support.
		if (!s_pScreenRenderer)
		{
			goto fail;
		}
	}
	SDL_RenderClear(s_pScreenRenderer);
	SDL_RenderPresent(s_pScreenRenderer);

	// Unset the render target, if currently set, for making renderer setting
	// changes. The render target should only be set once all settings have been
	// set, with no setting changes made after the render target has been set;
	// various bugs have been observed when attempting setting changes while the
	// render target is non-NULL, such bugs disappearing when setting changes
	// are made only while the render target is NULL.
	if ( s_pScreenRenderTarget && SDL_SetRenderTarget(s_pScreenRenderer, NULL) < 0 )
	{
		goto fail;
	}

	/* Clear the whole screen, as the framebuffer might be uninitialized */
	if ( SDL_RenderClear(s_pScreenRenderer) < 0 )
	{
		goto fail;
	}

	// This should be somewhere after the renderer has been created, as
	// SCREEN_SUBPIXEL_OFFSET queries the renderer when a given platform uses
	// nonzero offsets.
	s_fScreenSubpixelOffset = SCREEN_SUBPIXEL_OFFSET;

	if ( SDL_RenderSetVSync(s_pScreenRenderer, !!(*screenMode & YGS_SCREENMODE_VSYNC)) )
	{
		goto fail;
	}

	if ( SDL_RenderSetLogicalSize(s_pScreenRenderer, logicalWidth, logicalHeight) < 0 )
	{
		goto fail;
	}

	/* Use integer scaling, if required */
	if ( *screenMode & YGS_SCREENMODE_SCALEMODE )
	{
		if ( SDL_RenderSetIntegerScale(s_pScreenRenderer, SDL_TRUE) < 0 )
		{
			goto fail;
		}
	}
	else
	{
		if ( SDL_RenderSetIntegerScale(s_pScreenRenderer, SDL_FALSE) < 0 )
		{
			goto fail;
		}
	}

	/* Set up the render target, if required */
	// TODO: Figure out how to get render targets in Emscripten working with
	// SDL_BLENDMODE_BLEND textures. This #ifdef is a workaround for now. It's
	// probably a bug in the Emscripten SDL2 port.
	#ifndef __EMSCRIPTEN__
	if ( SDL_RenderTargetSupported(s_pScreenRenderer) )
	{
		if (!(*screenMode & YGS_SCREENMODE_RENDERLEVEL))
		{
			// There's no need to create a render target texture if the
			// currently created render target texture is already the current
			// logicalWidth x logicalHeight.
			bool createNewRenderTarget = true;

			if ( s_pScreenRenderTarget ) {
				int w, h;
				if ( SDL_QueryTexture(s_pScreenRenderTarget, NULL, NULL, &w, &h) < 0 ) {
					goto fail;
				}

				if ( w == logicalWidth && h == logicalHeight )
				{
					createNewRenderTarget = false;
				}
				else {
					SDL_DestroyTexture(s_pScreenRenderTarget);
					s_pScreenRenderTarget = NULL;
				}
			}

			if ( createNewRenderTarget )
			{
				s_pScreenRenderTarget = SDL_CreateTexture(s_pScreenRenderer, SDL_PIXELFORMAT_RGBX8888, SDL_TEXTUREACCESS_TARGET, logicalWidth, logicalHeight);
				if ( !s_pScreenRenderTarget )
				{
					goto fail;
				}
			}

			if (
				SDL_SetRenderTarget(s_pScreenRenderer, s_pScreenRenderTarget) < 0 ||
				SDL_RenderClear(s_pScreenRenderer) < 0
			)
			{
				goto fail;
			}
		}
		else if ( s_pScreenRenderTarget )
		{
			SDL_DestroyTexture(s_pScreenRenderTarget);
			s_pScreenRenderTarget = NULL;
		}
	}
	else
	#endif
	{
		s_pScreenRenderTarget = NULL;
		*screenMode |= YGS_SCREENMODE_RENDERLEVEL;
	}

	// WARNING: Make no changes to the renderer settings from here on down, as
	// the render target has been set, and bugs have been observed when
	// attempting renderer setting changes while a non-NULL render target is
	// set.

	#ifdef __EMSCRIPTEN__
	static bool emscriptenCallbackSet = false;
	if ( !emscriptenCallbackSet && emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, false, YGS2kEmscriptenResizeCallback) < 0 )
	{
		goto fail;
	}
	else
	{
		emscriptenCallbackSet = true;
		int width = -1, height = -1;
		width = EM_ASM_INT({ return window.innerWidth; });
		height = EM_ASM_INT({ return window.innerHeight; });
		if ( width > 0 && height > 0 )
		{
			SDL_SetWindowSize(s_pScreenWindow, width, height);
		}
	}
	#endif

	s_iScreenMode = *screenMode;
	s_iScreenIndex = *screenIndex;

	/* Hide the mouse cursor at first */
	if ( SDL_ShowCursor(SDL_DISABLE) < 0 )
	{
		goto fail;
	}
	s_uCursorCnting = 0u;

	/* Setup was successful, so return with success */
	return true;

	/* A failure condition was encountered, so clean up and return with error */
	fail:
	if ( s_pScreenRenderTarget )
	{
		SDL_SetRenderTarget(s_pScreenRenderer, NULL);
		SDL_DestroyTexture(s_pScreenRenderTarget);
	}
	if ( s_pScreenRenderer ) SDL_DestroyRenderer(s_pScreenRenderer);
	if ( s_pScreenWindow ) SDL_DestroyWindow(s_pScreenWindow);
	s_pScreenRenderTarget = NULL;
	s_pScreenRenderer = NULL;
	s_pScreenWindow = NULL;
	return false;
}

int YGS2kGetMaxDisplayIndex()
{
	return SDL_GetNumVideoDisplays();
}

int YGS2kGetMaxDisplayMode( int displayIndex )
{
	return SDL_GetNumDisplayModes(displayIndex);
}

bool YGS2kRenderLevelLowSupported()
{
	return s_pScreenRenderer ? SDL_RenderTargetSupported(s_pScreenRenderer) : false;
}

void YGS2kSetConstParam ( const char *param, int value )
{
	// TODO
}

int YGS2kRand ( int max )
{
	return rand() % max;
}

void YGS2kPauseMusic()
{
	Mix_PauseMusic();
}

void YGS2kReplayMusic()
{
	Mix_ResumeMusic();
}

void YGS2kPlayWave ( int num )
{
	if ( s_Wave[num].chunk )
	{
		Mix_PlayChannel(num, s_Wave[num].chunk, s_Wave[num].loops);
	}
}

void YGS2kReplayWave ( int num )
{
	Mix_Resume(num);
}

void YGS2kStopWave ( int num )
{
	Mix_HaltChannel(num);
}

void YGS2kPauseWave ( int num )
{
	Mix_Pause(num);
}

void YGS2kSetVolumeWave( int num, int vol )
{
	int volume = (int)((vol / 100.0f) * YGS_VOLUME_MAX);
	if ( volume > YGS_VOLUME_MAX ) { volume = YGS_VOLUME_MAX; }
	if ( volume < 0 )   { volume = 0; }
	Mix_Volume(num, volume);
}

int YGS2kIsPlayWave( int num )
{
	return Mix_Playing(num);
}

void YGS2kLoadWave( const char* filename, int num )
{
	int len = strlen(filename);
	if ( len < 4 ) { return; }

	SDL_RWops *src;
	src = FSOpenRead(filename);
	if ( !src ) return;
	if ( s_Wave[num].chunk )
	{
		Mix_FreeChunk(s_Wave[num].chunk);
		s_Wave[num].chunk = NULL;
	}
	s_Wave[num].chunk = Mix_LoadWAV_RW(src, 1);
	Mix_VolumeChunk(s_Wave[num].chunk, YGS_VOLUME_MAX);
	s_Wave[num].loops = 0;
}

void YGS2kSetLoopModeWave( int num, int mode )
{
	if ( mode )
	{
		// SDL2 SDL_mixer doesn't really support infinite looping of
		// chunks, but INT_MAX is plenty enough.
		s_Wave[num].loops = INT_MAX;
	}
	else
	{
		s_Wave[num].loops = 0;
	}
}

void YGS2kLoadMusic( const char* filename )
{
	if ( s_pMusic )
	{
		Mix_FreeMusic(s_pMusic);
		s_pMusic = NULL;
	}

	SDL_RWops* src;
	if (!(src = FSOpenRead(filename))) return;
	s_pMusic = Mix_LoadMUS_RW(src, SDL_TRUE);
	Mix_VolumeMusic(YGS_VOLUME_MAX);
}

void YGS2kLoadBitmap( const char* filename, int plane, int val )
{
	if ( !s_pScreenRenderer )
	{
		return;
	}

	if ( s_pTexture[plane] )
	{
		SDL_DestroyTexture(s_pTexture[plane]);
		s_pTexture[plane] = NULL;
	}

	SDL_RWops* src;
	if (!(src = FSOpenRead(filename))) return;
	if (!(s_pTexture[plane] = IMG_LoadTexture_RW(s_pScreenRenderer, src, SDL_TRUE))) return;
	SDL_SetTextureBlendMode(s_pTexture[plane], SDL_BLENDMODE_BLEND);
}

void YGS2kPlayMusic()
{
	if ( s_pMusic )
	{
		Mix_PlayMusic(s_pMusic, -1);
	}
}

void YGS2kStopMusic()
{
	Mix_HaltMusic();
}

void YGS2kSetVolumeMusic(int vol)
{
	int volume = (int)((vol / 100.0f) * YGS_VOLUME_MAX);
	if ( volume > YGS_VOLUME_MAX ) { volume = YGS_VOLUME_MAX; }
	if ( volume < 0 )   { volume = 0; }
	Mix_VolumeMusic(volume);
}

bool YGS2kWaveFormatSupported(YGS2kEWaveFormat format) {
	return s_bWaveFormatSupported[format & YGS_WAVE_FORMAT];
}

void YGS2kSetColorKeyPos(int plane, int x, int y)
{
	// TODO
	// sets transparent color to the specified pixel.  Since we use actual alpha channel in our assets, this is a no-oop
}

void YGS2kEnableBlendColorKey(int plane, int key)
{
	// TODO
	// alows for parial transparency.   again, because we use real transparency, it's a no-op.
}

void YGS2kCreateSurface(int surf, int w, int h)
{
	// TODO
	// required for orignal YGS2K engine. not needed at all for SDL2 renderer.
}

void YGS2kClearSecondary()
{
	// TODO
	// used to write the listed color to all pixels of the rendering area.
	// with SDL2 renderer, we never need to do this, so it's a no-op
}

void YGS2kSetFillColor(int col)
{
	// TODO
	// sets the color that YGS2kClearSecondary uses to fill the render target. since YGS2kClearSecondary is a no-op, so is this.
}

void YGS2kLoadFile( const char* filename, void* buf, size_t size )
{
	SDL_RWops	*src = FSOpenRead(filename);

	if ( src )
	{
		SDL_RWread(src, buf, size, 1);
		SDL_RWclose(src);

		/* エンディアン変換 */
		int32_t* buf2 = (int32_t*)buf;
		for ( size_t i = 0 ; i < size / sizeof(int32_t) ; i ++ )
		{
			buf2[i] = SWAP32(buf2[i]);
		}
	}
}

void YGS2kReadFile( const char* filename, void* buf, size_t size, size_t offset )
{
	SDL_RWops	*src = FSOpenRead(filename);

	if ( src )
	{
		if (SDL_RWseek(src, offset, RW_SEEK_SET) < 0) return;
		SDL_RWread(src, buf, size, 1);
		SDL_RWclose(src);

		/* エンディアン変換 */
		int32_t* buf2 = (int32_t*)buf;
		for ( size_t i = 0 ; i < size / sizeof(int32_t) ; i ++ )
		{
			buf2[i] = SWAP32(buf2[i]);
		}
	}
}


void YGS2kSaveFile( const char* filename, void* buf, size_t size )
{
	/* エンディアン変換 */
	int32_t* buf2 = (int32_t*)buf;
	for ( size_t i = 0 ; i < size / sizeof(int32_t) ; i ++ )
	{
		buf2[i] = SWAP32(buf2[i]);
	}

	SDL_RWops	*dst = FSOpenWrite(filename);

	if ( dst )
	{
		SDL_RWwrite(dst, buf, size, 1);
		if ( SDL_RWclose(dst) < 0 )
		{
			fprintf(stderr, "Error closing: %s\n", SDL_GetError());
		}
		// TODO: Create a custom SDL_RWops object type for Emscripten and put this in that.
		#ifdef __EMSCRIPTEN__
		EM_ASM({
			FS.syncfs(function (err) {
				assert(!err);
			});
		});
		#endif
	}

	/* もどす */
	for ( size_t i = 0 ; i < size / sizeof(int32_t) ; i ++ )
	{
		buf2[i] = SWAP32(buf2[i]);
	}
}

void YGS2kAppendFile( const char* filename, void* buf, size_t size ) {
	/* エンディアン変換 */
	int32_t* buf2 = (int32_t*)buf;
	for ( size_t i = 0 ; i < size / sizeof(int32_t) ; i ++ )
	{
		buf2[i] = SWAP32(buf2[i]);
	}

	SDL_RWops	*dst = FSOpenAppend(filename);

	if ( dst )
	{
		SDL_RWwrite(dst, buf, size, 1);
		// TODO: Create a custom SDL_RWops object type for Emscripten and put this in that.
		SDL_RWclose(dst);
		#ifdef __EMSCRIPTEN__
		EM_ASM({
			FS.syncfs(function (err) {
				assert(!err);
			});
		});
		#endif
	}

	/* もどす */
	for ( size_t i = 0 ; i < size / sizeof(int32_t) ; i ++ )
	{
		buf2[i] = SWAP32(buf2[i]);
	}
}

void YGS2kTextLayerOn ( int layer, int x, int y )
{
	s_TextLayer[layer].enable = true;
	s_TextLayer[layer].x = x;
	s_TextLayer[layer].y = y;
}

void YGS2kTextMove ( int layer, int x, int y )
{
	s_TextLayer[layer].x = x;
	s_TextLayer[layer].y = y;
}

void YGS2kTextColor ( int layer, int r, int g, int b )
{
	if (
		s_TextLayer[layer].color.r != r ||
		s_TextLayer[layer].color.g != g ||
		s_TextLayer[layer].color.b != b
	) {
		s_TextLayer[layer].color.r = r;
		s_TextLayer[layer].color.g = g;
		s_TextLayer[layer].color.b = b;
		s_TextLayer[layer].color.a = SDL_ALPHA_OPAQUE;
		s_TextLayer[layer].updateTexture = true;
	}
}

void YGS2kTextBackColorDisable ( int layer )
{
	// TODO
	// turns off the shadow effect for text in the listed layer. since we don't even use said shadow effect to begin with, it's a no-op.
}

void YGS2kTextSize ( int layer, int size )
{
	if (s_TextLayer[layer].size != size) {
		s_TextLayer[layer].size = size;
		s_TextLayer[layer].updateTexture = true;
	}
}

void YGS2kTextHeight ( int layer, int height )
{
	// TODO
	// only used in flexdraw.c for ExTextHeight. But since ExTextHeight is unused, we don't need to bother implementing it. 
}

void YGS2kTextOut ( int layer, const char* text )
{
	if ( strcmp(text, s_TextLayer[layer].string) != 0 )
	{
		strcpy(s_TextLayer[layer].string, text);
		s_TextLayer[layer].updateTexture = true;
	}
}

void YGS2kTextBlt ( int layer )
{
	if ( !s_pScreenRenderer || !s_TextLayer[layer].enable)
	{
		return;
	}

	int font = 0;

	if ( s_TextLayer[layer].size >= 12 )
	{
		font ++;
	}
	if ( s_TextLayer[layer].size >= 16 )
	{
		font ++;
	}

	if ( !s_pKanjiFont[font] )
	{
		return;
	}

	if ( s_TextLayer[layer].updateTexture )
	{
		if ( s_TextLayer[layer].texture )
		{
			SDL_DestroyTexture(s_TextLayer[layer].texture);
		}
		s_TextLayer[layer].texture = Kanji_CreateTexture(s_pKanjiFont[font], s_pScreenRenderer, s_TextLayer[layer].string, s_TextLayer[layer].color, 32);
		if ( !s_TextLayer[layer].texture )
		{
			SDL_Log("Error creating texture to blit text: %s\n", SDL_GetError());
			YGS2kExit(EXIT_FAILURE);
		}
		if ( SDL_QueryTexture(s_TextLayer[layer].texture, NULL, NULL, &s_TextLayer[layer].textureW, &s_TextLayer[layer].textureH) < 0 )
		{
			SDL_Log("Error getting size of a text layer texture: %s\n", SDL_GetError());
			YGS2kExit(EXIT_FAILURE);
		}
		s_TextLayer[layer].updateTexture = false;
	}

	if ( s_TextLayer[layer].texture )
	{
		if ( s_pScreenRenderTarget )
		{
			const SDL_Rect dstRect =
			{
				s_TextLayer[layer].x + s_iOffsetX,
				s_TextLayer[layer].y + s_iOffsetY,
				s_TextLayer[layer].textureW,
				s_TextLayer[layer].textureH
			};
			if ( SDL_RenderCopy(s_pScreenRenderer, s_TextLayer[layer].texture, NULL, &dstRect) < 0 )
			{
				SDL_Log("Error rendering text layer: %s\n", SDL_GetError());
				YGS2kExit(EXIT_FAILURE);
			}
		}
		else
		{
			const SDL_FRect dstRect =
			{
				s_TextLayer[layer].x + s_iOffsetX + s_fScreenSubpixelOffset,
				s_TextLayer[layer].y + s_iOffsetY + s_fScreenSubpixelOffset,
				s_TextLayer[layer].textureW,
				s_TextLayer[layer].textureH
			};
			if ( SDL_RenderCopyF(s_pScreenRenderer, s_TextLayer[layer].texture, NULL, &dstRect) < 0 )
			{
				SDL_Log("Error rendering text layer: %s\n", SDL_GetError());
				YGS2kExit(EXIT_FAILURE);
			}
		}
	}
}

void YGS2kTextLayerOff ( int layer )
{
	s_TextLayer[layer].enable = false;
}

void YGS2kBltAlways(bool always)
{
	s_bNoFrameskip = always;
}

void YGS2kBlt(int pno, int dx, int dy)
{
	if ( s_pTexture[pno] == NULL ) { return; }
	int w, h;
	SDL_QueryTexture(s_pTexture[pno], NULL, NULL, &w, &h);
	YGS2kBltRect(pno, dx, dy, 0, 0, w, h);
}
void YGS2kBltRect(int pno, int dx, int dy, int sx, int sy, int hx, int hy)
{
	if ( !s_pScreenRenderer )
	{
		return;
	}

	if ( s_pTexture[pno] == NULL ) return;

	if ( s_pScreenRenderTarget )
	{
		SDL_Rect	src = { 0 };
		SDL_Rect	dst = { 0 };

		src.x = sx;			src.y = sy;
		src.w = hx;			src.h = hy;
		dst.x = dx + s_iOffsetX;	dst.y = dy + s_iOffsetY;
		dst.w = hx;			dst.h = hy;

		SDL_RenderCopy(s_pScreenRenderer, s_pTexture[pno], &src, &dst);
	}
	else
	{
		SDL_Rect	src = { 0 };
		SDL_FRect	dst = { s_fScreenSubpixelOffset, s_fScreenSubpixelOffset };

		src.x  = sx;			src.y  = sy;
		src.w  = hx;			src.h  = hy;
		dst.x += dx + s_iOffsetX;	dst.y += dy + s_iOffsetY;
		dst.w  = hx;			dst.h  = hy;

		SDL_RenderCopyF(s_pScreenRenderer, s_pTexture[pno], &src, &dst);
	}
}

void YGS2kBltFast(int pno, int dx, int dy)
{
	YGS2kBlt(pno, dx, dy);
}

void YGS2kBltFastRect(int pno, int dx, int dy, int sx, int sy, int hx, int hy)
{
	YGS2kBltRect(pno, dx, dy, sx, sy, hx, hy);
}

void YGS2kBlendBlt(int pno, int dx, int dy, int ar, int ag, int ab, int br, int bg, int bb)
{
	if ( s_pTexture[pno] == NULL ) return;

	SDL_SetTextureAlphaMod(s_pTexture[pno], ar);
	YGS2kBlt(pno, dx, dy);
	SDL_SetTextureAlphaMod(s_pTexture[pno], SDL_ALPHA_OPAQUE);
}

void YGS2kBlendBltRect(int pno, int dx, int dy, int sx, int sy, int hx, int hy, int ar, int ag, int ab, int br, int bg, int bb)
{
	if ( s_pTexture[pno] == NULL ) return;

	SDL_SetTextureAlphaMod(s_pTexture[pno], ar);
	YGS2kBltRect(pno, dx, dy, sx, sy, hx, hy);
	SDL_SetTextureAlphaMod(s_pTexture[pno], SDL_ALPHA_OPAQUE);
}

void YGS2kBltR(int pno, int dx, int dy, int scx, int scy)
{
	YGS2kBltRectR(pno, dx, dy, 0, 0, s_iLogicalWidth, s_iLogicalHeight, scx, scy);
}

void YGS2kBltRectR(int pno, int dx, int dy, int sx, int sy, int hx, int hy, int scx, int scy)
{
	if ( !s_pScreenRenderer )
	{
		return;
	}
	if ( s_pTexture[pno] == NULL ) return;

	// ちゃんと拡大して描画する
	if ( s_pScreenRenderTarget )
	{
		SDL_Rect	src = { 0 };
		SDL_Rect	dst = { 0 };

		src.x = sx;					src.y = sy;
		src.w = hx;					src.h = hy;
		dst.x = dx + s_iOffsetX;	dst.y = dy + s_iOffsetY;
		dst.w = hx * (scx / 65536.0f);
		dst.h = hy * (scy / 65536.0f);

		if ( src.w == 0 || src.h == 0 || dst.w == 0 || dst.h == 0 ) { return; }

		SDL_RenderCopy( s_pScreenRenderer, s_pTexture[pno], &src, &dst );
	}
	else
	{
		SDL_Rect	src = { 0 };
		SDL_FRect	dst = { s_fScreenSubpixelOffset, s_fScreenSubpixelOffset };

		src.x  = sx;			src.y  = sy;
		src.w  = hx;			src.h  = hy;
		dst.x += dx + s_iOffsetX;	dst.y += dy + s_iOffsetY;
		dst.w  = (int)(hx * (scx / 65536.0f));
		dst.h  = (int)(hy * (scy / 65536.0f));

		if ( src.w == 0 || src.h == 0 || dst.w == 0 || dst.h == 0 ) { return; }

		SDL_RenderCopyF( s_pScreenRenderer, s_pTexture[pno], &src, &dst );
	}
}

void YGS2kBltFastR(int pno, int dx, int dy, int scx, int scy)
{
	YGS2kBltR(pno, dx, dy, scx, scy);
}

void YGS2kBltFastRectR(int pno, int dx, int dy, int sx, int sy, int hx, int hy, int scx, int scy)
{
	YGS2kBltRectR(pno, dx, dy, sx, sy, hx, hy, scx, scy);
}

void YGS2kBltTrans(int pno, int dx, int dy)
{
	// TODO
	// completely unused.  so we don't need to care what it even does.
}

void YGS2kBlendBltR(int pno, int dx, int dy, int ar, int ag, int ab, int br, int bg, int bb, int scx, int scy)
{
	YGS2kBlendBltRectR(pno, dx, dy, 0, 0, s_iLogicalWidth, s_iLogicalHeight, ar, ag, ab, br, bg, bb, scx, scy);
}

void YGS2kBlendBltRectR(int pno, int dx, int dy, int sx, int sy, int hx, int hy, int ar, int ag, int ab, int br, int bg, int bb, int scx, int scy)
{
	if ( !s_pScreenRenderer )
	{
		return;
	}

	if ( s_pTexture[pno] == NULL ) return;

	// ちゃんと拡大して描画する
	if ( s_pScreenRenderTarget )
	{
		SDL_Rect	src = { 0 };
		SDL_Rect	dst = { 0 };

		src.x = sx;					src.y = sy;
		src.w = hx;					src.h = hy;
		dst.x = dx + s_iOffsetX;	dst.y = dy + s_iOffsetY;
		dst.w = hx * (scx / 65536.0f);
		dst.h = hy * (scy / 65536.0f);

		if ( src.w == 0 || src.h == 0 || dst.w == 0 || dst.h == 0 ) { return; }

		SDL_SetTextureAlphaMod(s_pTexture[pno], ar);
		SDL_RenderCopy( s_pScreenRenderer, s_pTexture[pno], &src, &dst );
		SDL_SetTextureAlphaMod(s_pTexture[pno], SDL_ALPHA_OPAQUE);
	}
	else
	{
		SDL_Rect	src = { 0 };
		SDL_FRect	dst = { s_fScreenSubpixelOffset, s_fScreenSubpixelOffset };

		src.x  = sx;			src.y  = sy;
		src.w  = hx;			src.h  = hy;
		dst.x += dx + s_iOffsetX;	dst.y += dy + s_iOffsetY;
		dst.w  = hx * (scx / 65536.0f);
		dst.h  = hy * (scy / 65536.0f);

		if ( src.w == 0 || src.h == 0 || dst.w == 0 || dst.h == 0 ) { return; }

		SDL_SetTextureAlphaMod(s_pTexture[pno], ar);
		SDL_RenderCopyF( s_pScreenRenderer, s_pTexture[pno], &src, &dst );
		SDL_SetTextureAlphaMod(s_pTexture[pno], SDL_ALPHA_OPAQUE);
	}
}

void YGS2kSetSecondaryOffset(int x, int y)
{
	s_iNewOffsetX = x;
	s_iNewOffsetY = y;
}

void YGS2kSetColorKeyRGB(int pno, int r, int g, int b)
{
	// TODO
	//  again because we have actual transparency in our assets, this is a no-op.
}

void YGS2kResetFrameStep()
{
	nanotime_step_init(&s_StepData, NANOTIME_NSEC_PER_SEC / s_uNowFPS, nanotime_now_max(), nanotime_now, nanotime_sleep);
}

void YGS2kSetFPS(unsigned fps)
{
	if (fps == 0) {
		s_uNowFPS = 1u;
	}
	else {
		s_uNowFPS = fps;
	}
	YGS2kResetFrameStep();
}

int YGS2kGetFPS()
{
	return s_uNowFPS;
}

int YGS2kGetRealFPS()
{
	return s_uFPS;
}

void YGS2kStrCpy(char *dest, const char *src)
{
	strcpy(dest, src);
}

void YGS2kStrCat(char *str1, const char *str2)
{
	strcat(str1, str2);
}

int YGS2kStrLen(const char *stri)
{
	return strlen(stri);
}

void YGS2kMidStr(const char *src, int start, int len, char *dest)
{
	int		i;
	for ( i = 0 ; i < len ; i ++ )
	{
		dest[i] = src[start - 1 + i];
	}
	dest[len] = '\0';
}

void YGS2kLeftStr(const char *src, int len, char *dest)
{
	YGS2kMidStr(src, 1, len, dest);
}

char YGS2kCharAt(const char *stri, int pos)
{
	return stri[pos];
}

int YGS2kValLong(const char *stri)
{
	return atoi(stri);
}

void YGS2kFillMemory(void* buf, int size, int val)
{
	memset(buf, val, size);
}

////////////////////////////////////////////////////

static void YGS2kPrivateKanjiFontInitialize()
{
	const char* const filenames[YGS_KANJIFONTFILE_MAX] = {
		"res/font/font10.bdf",
		"res/font/font12.bdf",
		"res/font/font16.bdf"
	};

	/* フォント読み込み */
	/* Load fonts */
	for (int i = 0; i < YGS_KANJIFONTFILE_MAX; i++) {
		if (s_pKanjiFont[i]) {
			continue;
		}
		SDL_RWops *src = FSOpenRead(filenames[i]);
		if ( !src ) {
			SDL_Log("Failed to open file for font \"%s\"; continuing without it.", filenames[i]);
			s_pKanjiFont[i] = NULL;
			continue;
		}
		s_pKanjiFont[i] = Kanji_OpenFont(src);
		if ( !s_pKanjiFont[i] ) {
			SDL_Log("Failed to load font \"%s\": %s\n", filenames[i], SDL_GetError());
			SDL_RWclose(src);
			YGS2kExit(EXIT_FAILURE);
		}
		SDL_RWclose(src);
	}
}

static void YGS2kPrivateKanjiFontFinalize()
{
	for (int i = 0; i < YGS_KANJIFONTFILE_MAX; i++) {
		if (s_pKanjiFont[i]) {
			Kanji_CloseFont(s_pKanjiFont[i]);
		}
		s_pKanjiFont[i] = NULL;
	}
}
