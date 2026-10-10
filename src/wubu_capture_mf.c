/*
 * WuBuCapture — GPU YUV→RGB shader, borderless fullscreen, tearing present
 *
 * Key latency optimizations:
 *   - YUY2 uploaded directly to GPU, pixel shader does BT.709 conversion
 *   - Present(0, DXGI_PRESENT_ALLOW_TEARING) — no vsync wait ever
 *   - Borderless fullscreen (no swap chain resize = no freeze)
 *   - MsgWaitForMultipleObjects — wakes instantly on frame event
 *   - Lock-free triple buffer SPSC queue
 *
 * Build:
 *   gcc -O2 -std=c11 -o WuBuCapture.exe WuBuCapture.c \
 *     -lmfplat -lmf -lmfreadwrite -lmfuuid -ld3d11 -ldxgi -ld3dcompiler \
 *     -lole32 -loleaut32 -luuid -luser32 -lgdi32 -lwinmm -ldxguid
 */
#define COBJMACROS 1

#include <windows.h>
#include <mfapi.h>
#include "wubu_mode.h"
#include "wubu_telem.h"
#include "wubu_rev.h"
#include "wubu_panel.h"
#include "wubu_qdc.h"
#include "wubu_stat.h"

/* Attribute GUIDs this MinGW SDK does not declare. Values are the standard
   Media Foundation ones (mfapi.h defines MF_MT_FRAME_SIZE etc. but not these). */
/* DEFINE_GUID declares extern symbols that -luuid does not export for these,
   so define them file-locally as constants instead. */
static const GUID MF_MT_FRAME_SIZE_H_ =
    { 0xdad3ab78, 0x1990, 0x4080, { 0xa5,0x02,0x2c,0x1a,0x8e,0x2e,0x00,0xb4 } };
static const GUID MF_MT_FRAME_RATE_N_ =
    { 0x0c9696c5, 0x8bc1, 0x445c, { 0xa4,0x19,0x03,0x2a,0x6e,0x15,0xf0,0xdd } };
static const GUID MF_MT_FRAME_RATE_D_ =
    { 0x1c628831, 0x4672, 0x43a2, { 0x8e,0xab,0x88,0x51,0x7b,0x61,0xfb,0xbf } };

#include <mfidl.h>
#include <mfreadwrite.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static IDXGISwapChain1 *g_swap;
static ID3D11RenderTargetView *g_rtv;
static ID3D11Texture2D *g_yuy2Tex;
static ID3D11ShaderResourceView *g_yuy2Srv;
/* Pass-1 target: source-resolution BGRA the scale pass reads from. */
static ID3D11Texture2D *g_srcTex;
static ID3D11RenderTargetView *g_srcRtv;
static ID3D11ShaderResourceView *g_srcSrv;
static ID3D11VertexShader *g_vs;
static ID3D11PixelShader *g_psYuy2;   /* pass 1: YUY2 -> BGRA at 1920x1080 */
static ID3D11PixelShader *g_psScale;  /* pass 2: BGRA -> window, bilinear   */
static ID3D11InputLayout *g_il;
static ID3D11Buffer *g_vb;
static ID3D11SamplerState *g_samp;

static HWND g_hwnd;
static volatile BOOL g_running = TRUE;
static int g_verbose = 0;          /* -v flag; default silent */

/* defined below; needed before InitD3D runs */
static int  g_exclusive = 0;
static UINT g_bbW = 0, g_bbH = 0;
static ID3D11RenderTargetView *g_bbRtv[2];
static UINT g_bbIndex = 0;      /* advances once per Present; see RenderFrame */
static UINT g_bbCount = 1;      /* back buffers DXGI actually gave us */
static void query_display_res(UINT *w, UINT *h);
static int g_stream_mode = 1;      /* be a good neighbour to OBS by default */
/*
 * 0 = VSync (default). The compositor locks each frame to a panel refresh and
 *     drops surplus source frames, so a rate mismatch cannot produce held frames.
 * 1 = tear: free-running Present(0) paced by our own QPC timer. Only correct when
 *     the pacer is phase-locked to the panel, which we cannot guarantee, so this
 *     is opt-in via --tear for A/B comparison only.
 */
static int g_tear = 0;
static int g_winW = 1280, g_winH = 720;
static const int CAP_W = 1920, CAP_H = 1080;

/*
 * Display topology. The ONLY source of panel truth in this program.
 *
 * It is a separate module (wubu_panel.c) precisely because "what is the refresh
 * rate" was answered wrongly three different ways in one session, twice by
 * code that looked correct. Keeping the reader behind one opaque struct means
 * there is exactly one place where a rate can come from, and that place
 * documents the truncation limit it is subject to.
 */
static wubu_panel *g_panel = NULL;

/*
 * The panel we are actually presenting to, re-resolved at F-time.
 *
 * The user's instruction was explicit: "when I press F it's on the monitor I
 * want it, can detect then too." Detecting at STARTUP is what failed -- the
 * window had not been placed, so the monitor lookup returned a neighbour. So the
 * target is re-measured every time the window moves, against the display under
 * the cursor, and the pacing authority is re-locked to the number we just read.
 */
static char    g_targetDev[64] = {0};   /* ASCII: \\.\DISPLAYnn, see wubu_panel.h */
static double  g_targetHz = 0.0;
/* True fractional panel rate from QueryDisplayConfig (e.g. 59.936, not 59).
   0.0 == not yet resolved. Distinct from g_targetHz, which is the truncated
   integer field and is retained only as a reported fallback. */
static double  g_panelHzTrue = 0.0;
static unsigned g_syncInterval = 0;   /* 0 == no even integer cadence exists */

/* The monitor we last locked the cadence to. 0 = not yet locked. The main loop
   compares this against MonitorFromWindow() and re-locks on a change, because a
   plain window drag moves the window to a different display without going
   through F (which re-locks explicitly). */
static HMONITOR g_lockedMon = NULL;

/* Defined with the window code far below; needed by the main loop's first pass. */
static void relock_target_panel(BOOL useCursor);

/* Triple-buffered YUY2 frame pool */
static uint8_t *g_bufs[3];
static volatile LONG g_wrIdx, g_rdIdx, g_frIdx;
static HANDLE g_frameEvent;
static IMFSourceReader *g_reader;

/* BT.709 coefficients (<<10 fixed point) */
#define RV 1613
#define GU -192
#define GV -480
#define BU 1901

static const float g_verts[] = {
    -1, 1, 0, 0,  1, 1, 1, 0,  -1,-1, 0, 1,  1,-1, 1, 1
};

/* Passthrough VS */
static const char *g_vsSrc =
    "struct VIN{float2 p:POSITION;float2 u:TEXCOORD;};"
    "struct VOUT{float4 p:SV_POSITION;float2 u:TEXCOORD;};"
    "VOUT main(VIN i){VOUT o;o.p=float4(i.p,0,1);o.u=i.u;return o;}";

/*
 * TWO-PASS SCALING — the correct architecture for speed AND quality.
 *
 * MEASURED FAILURE OF THE ONE-PASS VERSION:
 *   Doing bilinear 4:2:2 resampling in the final pass needs 5 texture Load()
 *   calls per OUTPUT pixel. At 3440x1440 that is ~24.8M texel fetches every
 *   frame, on a GPU already ~99% busy with the game. Measured result: render
 *   rate collapsed to 47.3 fps while capture stayed at 59.3, dropping 163
 *   frames in 13.5s. Beautiful, and too expensive to run in real time.
 *
 * THE FIX:
 *   Pass 1 converts YUY2 -> BGRA ONCE, at the source's own 1920x1080, using
 *   the single-tap shader that was already proven correct by parity test
 *   (cos 0.99999). 2.07M pixels, 1 texel fetch each.
 *   Pass 2 is a plain hardware-bilinear blit from that 1920x1080 BGRA texture
 *   up to the window. 4.95M pixels, ONE sampler tap each, and because the
 *   source is now real RGB the sampler may filter all four components freely
 *   — so we get TRUE 4-tap bilinear on every channel instead of the
 *   luma-only approximation, at a fraction of the fetch cost.
 *
 *   Total texture work: ~7M ops instead of ~24.8M, and the image is sharper.
 */
static const char *g_srcPsYuy2 =
    /* Pass 1: raw YUY2 texel -> BGRA, one tap, at native 1920x1080.
       This is the exact shader that was verified against the CPU reference. */
    "Texture2D tex:register(t0);"
    "float4 main(float4 pos:SV_POSITION, float2 uv:TEXCOORD):SV_TARGET{\n"
    "  int px = min(1919, (int)(uv.x * 1920.0));\n"
    "  int py = min(1079, (int)(uv.y * 1080.0));\n"
    "  float4 t = tex.Load(int3(px >> 1, py, 0));\n"
    "  float Y = ((px & 1) == 0) ? t.r : t.b;\n"
    "  float U = t.g - 0.501961;\n"
    "  float V = t.a - 0.501961;\n"
    "  float R = Y + 1.57520 * V;\n"
    "  float G = Y - 0.18750 * U - 0.46875 * V;\n"
    "  float B = Y + 1.85645 * U;\n"
    "  return float4(saturate(R), saturate(G), saturate(B), 1.0);\n"
    "}";

/* Pass 2: hardware-bilinear BGRA upscale to the window. One sampler tap. */
static const char *g_srcPsScale =
    "Texture2D tex:register(t0);"
    "SamplerState samp:register(s0);"
    "float4 main(float4 pos:SV_POSITION, float2 uv:TEXCOORD):SV_TARGET{\n"
    "  return tex.Sample(samp, uv);\n"
    "}";

static HRESULT InitD3D11(void) {
    HRESULT hr;
    ID3DBlob *vsB, *psB1, *psB2;
    hr = D3DCompile(g_vsSrc, strlen(g_vsSrc), 0, 0, 0, "main", "vs_4_0", 0, 0, &vsB, 0);
    if (FAILED(hr)) { fprintf(stderr, "VS compile: 0x%08X\n", hr); return hr; }
    hr = D3DCompile(g_srcPsYuy2, strlen(g_srcPsYuy2), 0, 0, 0, "main", "ps_4_0", 0, 0, &psB1, 0);
    if (FAILED(hr)) { fprintf(stderr, "PS1 compile: 0x%08X\n", hr); return hr; }
    hr = D3DCompile(g_srcPsScale, strlen(g_srcPsScale), 0, 0, 0, "main", "ps_4_0", 0, 0, &psB2, 0);
    if (FAILED(hr)) { fprintf(stderr, "PS2 compile: 0x%08X\n", hr); return hr; }

    D3D_FEATURE_LEVEL fl;
    hr = D3D11CreateDevice(0, D3D_DRIVER_TYPE_HARDWARE, 0,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, 0, 0, D3D11_SDK_VERSION,
        &g_dev, &fl, &g_ctx);
    if (FAILED(hr)) return hr;

    IDXGIDevice1 *dd = NULL;
    ID3D11Device_QueryInterface(g_dev, &IID_IDXGIDevice1, (void**)&dd);
    if (dd) {
        /*
         * MAXIMUM FRAME LATENCY: 1 by default, overridable via WUBU_LATENCY.
         *
         * This was hardcoded to 1. That is the minimum the API allows, and it
         * tells DXGI the CPU may never be more than ONE frame ahead of the
         * display. Combined with BufferCount=2 that is a very shallow queue: the
         * moment the GPU or the compositor is even slightly behind, there is no
         * slack to absorb it, so Present() itself blocks. That is consistent
         * with everything measured so far:
         *
         *   - our own work is 3.12 ms max against a 16.67 ms budget (19%), so we
         *     are not the thing taking the time;
         *   - yet present-to-present stretches to 38 ms with 100% presents:retires
         *     paired, i.e. no kernel drops -- time is spent WAITING, not lost;
         *   - a control app on the same box at the same rate shows sd 0.877 ms.
         *
         * Raising the latency gives the queue room to absorb jitter instead of
         * pushing the wait back into the Present call. The cost is up to N-1
         * extra frames of input latency, which for a preview is the right
         * trade against visible stutter -- but that is a judgement call, so it
         * is a runtime knob rather than a silent change.
         *
         * TESTED AND FALSIFIED 2026-10-02. Do not raise this.
         *
         * The docs are right that the default is 3 and that video playback
         * benefits from more. Applying that guidance here made things WORSE,
         * badly and reproducibly. Six 40 s trials per setting:
         *
         *   latency 1 (v45): dips/min 1.5-3.0, sd 0.615-0.923, worst gap 49.9 ms
         *   latency 4 (v46): dips/min 1.5-4.5 on trials 1-4, then 926.6 and
         *                      993.2 dips/min on trials 5-6, sd 68.9/72.7,
         *                      worst gap 3000.8 ms
         *
         * So the deeper queue does not absorb jitter here, it ACCUMULATES it:
         * with 4 frames in flight, frames are queued behind work that has not
         * been retired and the backlog runs away (3000 ms == three whole
         * refreshes of stall). The "queue more for video" guidance assumes the
         * consumer never falls behind; ours does, briefly.
         *
         * Latency 1 remains the measured-best setting. WUBU_LATENCY still
         * overrides so the experiment stays reproducible.
         */
        {
            const char *lz = getenv("WUBU_LATENCY");
            UINT lat = (lz && atoi(lz) >= 1) ? (UINT)atoi(lz) : 1;
            HRESULT hl = IDXGIDevice1_SetMaximumFrameLatency(dd, lat);
            if (g_verbose)
                fprintf(stderr, "dxgi: SetMaximumFrameLatency(%u) -> 0x%08lX\n",
                        lat, (unsigned long)hl);
        }
    }

    IDXGIAdapter *ad;
    IDXGIDevice1_GetAdapter(dd, &ad);
    IDXGIFactory2 *f;
    IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void**)&f);

    /*
     * LET DXGI DO THE UPSCALE — the cheapest correct path.
     *
     * MEASURED LADDER OF FAILURES on this box (GPU already ~99% busy with the
     * game, so every extra GPU op we add costs us real frames):
     *   v2  one-pass nearest  Load()  -> 60 fps, but BLOCKY (not crispy)
     *   v5  one-pass bilinear 5x Load  -> 47.3 fps, 163 frames dropped  (too slow)
     *   v6  two-pass convert+scale     -> 49.3 fps, 121 dropped, BLACK   (still slow)
     *
     * The fix is to do LESS work, not more. We create the swap chain at the
     * SOURCE resolution (1920x1080) and set DXGI_SCALING_STRETCH, so the
     * display pipeline performs the 1920->3440 stretch for us with no shader,
     * no second pass, and no extra render target at all. We then render a
     * single 1920x1080 pass with the one-tap YUY2->BGRA shader.
     *
     * That is 2.07M pixels and 1 texel fetch each — versus v6's ~7M texture
     * ops — a ~5x reduction against the original, and the upscale is done by
     * fixed-function hardware rather than by us.
     */
    DXGI_SWAP_CHAIN_DESC1 scd = {0};
    if (g_exclusive) {
        /* Exclusive: the back buffer IS the display mode, so it must be the
           panel's native resolution. DXGI_SCALING_STRETCH then has nothing to
           do and we do the 1920->panel upscale in our own pass-2 shader. */
        query_display_res(&g_bbW, &g_bbH);
        scd.Width = g_bbW; scd.Height = g_bbH;
        scd.Scaling = DXGI_SCALING_NONE;
    } else {
        scd.Width  = CAP_W; scd.Height = CAP_H;   /* SOURCE res, not window res */
    }
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    /*
     * BufferCount: 2 by default, overridable at runtime for A/B.
     *
     * Microsoft guidance for flip-model swapchains generally favours 3 buffers,
     * because with 2 the queue can be full when we come to Present and the call
     * then blocks until the GPU has consumed the oldest frame. A blocked Present
     * is EXACTLY the shape of the symptom under investigation: work time is
     * only ~3 ms, yet present-to-present stretches to 38 ms, which means the time
     * is being spent inside Present waiting on the queue rather than in our code.
     *
     * So this is now settable to compare 2 vs 3 without editing and rebuilding.
     * WUBU_BUFCNT=3 ./WuBuCapture sets it to 3. Default stays 2.
     */
    {
        const char *bc = getenv("WUBU_BUFCNT");
        scd.BufferCount = (bc && atoi(bc) >= 2) ? (UINT)atoi(bc) : 2;
    }
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (!g_exclusive) scd.Scaling = DXGI_SCALING_STRETCH;  /* HW scales to window */
    /*
     * scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
     *
     * TESTED 2026-10-01 and FALSIFIED: removing this flag made the hitches
     * substantially WORSE, not better.
     *
     *   v39  (ALLOW_TEARING set, BufferCount 2): sd 2.793, 37 dips >25ms, max 38.6
     *   v40  (flag removed,      BufferCount 2): sd 3.711, 96 dips >25ms, max 55.4
     *
     * The reasoning that motivated the removal was sound but the measurement
     * refutes the conclusion: ALLOW_TEARING is only REQUIRED for
     * Present(0, DXGI_PRESENT_ALLOW_TEARING), yet the runtime evidently uses the
     * flag to select a different, better-behaved scheduling path even when
     * Present uses a non-zero interval. Leaving it unset pushed the chain onto
     * the legacy path, which is worse.
     *
     * This is the second time on this project that a plausible-sounding theory
     * about the runtime was wrong and only measurement settled it. Keep the
     * flag.
     */
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    if (g_verbose) fprintf(stderr, "swapchain: BufferCount=%u tearing=%d effect=FLIP_DISCARD\n",
                           scd.BufferCount, (scd.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? 1 : 0);
    hr = IDXGIFactory2_CreateSwapChainForHwnd(f, (IUnknown*)g_dev, g_hwnd, &scd, 0, 0, &g_swap);
    if (FAILED(hr)) return hr;
    IDXGIFactory_MakeWindowAssociation(f, g_hwnd, DXGI_MWA_NO_ALT_ENTER);

    /*
         * GetBuffer returns an HRESULT and only writes *ppv on success. Leaving the
         * out-param uninitialised and feeding it straight to CreateRenderTargetView
         * is exactly how exclusive mode segfaulted: a failed GetBuffer(1) left
         * `bb2` as stack garbage and the RTV creation dereferenced it. So we
         * initialise, check, and bail out cleanly instead.
         */
        ID3D11Texture2D *bb = 0;
        HRESULT hbb = IDXGISwapChain1_GetBuffer(g_swap, 0, &IID_ID3D11Texture2D, (void**)&bb);
        if (FAILED(hbb) || !bb) {
            fprintf(stderr, "GetBuffer(0) failed 0x%08lX\n", (unsigned long)hbb);
            return FAILED(hbb) ? hbb : E_FAIL;
        }
        hbb = ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource*)bb, 0, &g_rtv);
        ID3D11Texture2D_Release(bb);
        if (FAILED(hbb)) return hbb;

        if (g_exclusive) {
            /*
             * Ask DXGI how many back buffers it ACTUALLY gave us rather than
             * assuming the BufferCount we requested. We requested 2, but
             * GetBuffer(1) came back 0x887A0001 — this driver/driver-mode pair
             * handed us a single-buffer chain. Assuming 2 made us dereference an
             * unwritten buffer (the "half-black frame" failure mode), so we read
             * the real count and build exactly that many RTVs. g_bbCount then
             * bounds the index in RenderFrame.
             */
            DXGI_SWAP_CHAIN_DESC got;
            memset(&got, 0, sizeof got);
            HRESULT hd = IDXGISwapChain1_GetDesc(g_swap, &got);
            g_bbCount = (SUCCEEDED(hd) && got.BufferCount > 0) ? got.BufferCount : 1;
            if (g_bbCount > 2) g_bbCount = 2;
            fprintf(stderr, "[x] swapchain back buffers=%u (requested 2)\n", g_bbCount);

            ID3D11Texture2D *bb2 = 0;
            HRESULT hg = IDXGISwapChain1_GetBuffer(g_swap, 1, &IID_ID3D11Texture2D, (void**)&bb2);
            if (SUCCEEDED(hg) && bb2) {
                HRESULT hr2 = ID3D11Device_CreateRenderTargetView(g_dev,
                                    (ID3D11Resource*)bb2, 0, &g_bbRtv[1]);
                ID3D11Texture2D_Release(bb2);
                if (FAILED(hr2)) return hr2;
            } else {
                /* Single-buffer chain: every present reuses the same surface. */
                g_bbCount = 1;
            }
            g_bbRtv[0] = g_rtv;

        /* Leave the desktop: from here the swap chain drives the display.
                   Passing NULL for the mode change means "use the current mode", which
                   is what we want — the back buffer is already at panel resolution.
                   We deliberately do NOT pass DXGI_FULLSCREEN_EXCLUSIVE_OR_WINDOWED:
                   that flag silently downgrades to windowed on failure, and we need the
                   failure to be visible so we can report it instead of thinking we are
                   exclusive when DWM is still in the path. */
                HRESULT hfs = IDXGISwapChain_SetFullscreenState(g_swap, TRUE, 0);
        if (FAILED(hfs)) {
            fprintf(stderr, "SetFullscreenState failed 0x%08lX\n", (unsigned long)hfs);
            g_exclusive = 0;
        } else {
            fprintf(stderr, "exclusive fullscreen %ux%u\n", g_bbW, g_bbH);
        }
    }

    ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vsB), ID3D10Blob_GetBufferSize(vsB), 0, &g_vs);
    ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(psB1), ID3D10Blob_GetBufferSize(psB1), 0, &g_psYuy2);
    ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(psB2), ID3D10Blob_GetBufferSize(psB2), 0, &g_psScale);

    D3D11_INPUT_ELEMENT_DESC ld[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0},
    };
    ID3D11Device_CreateInputLayout(g_dev, ld, 2, ID3D10Blob_GetBufferPointer(vsB), ID3D10Blob_GetBufferSize(vsB), &g_il);

    D3D11_BUFFER_DESC bd = {sizeof(g_verts), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER};
    D3D11_SUBRESOURCE_DATA sd = {g_verts};
    ID3D11Device_CreateBuffer(g_dev, &bd, &sd, &g_vb);

    /* Raw YUY2 texture: 2 pixels per texel, 4 bytes each -> width is CAP_W/2 */
    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = CAP_W / 2;
    td.Height = CAP_H;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Device_CreateTexture2D(g_dev, &td, 0, &g_yuy2Tex);
    ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource*)g_yuy2Tex, 0, &g_yuy2Srv);

    D3D11_SAMPLER_DESC sam = {D3D11_FILTER_MIN_MAG_MIP_LINEAR,
        D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP};
    sam.MaxAnisotropy = 1;
    ID3D11Device_CreateSamplerState(g_dev, &sam, &g_samp);

    /* Pass-1 render target: BGRA at the source's own 1920x1080. Sampled by
       pass 2. This is what makes hardware bilinear legal on all 4 channels —
     * by the time the sampler runs, the pixels are real RGB, not subsampled
     * YUY2, so there is no chroma phase error to get wrong. */
    D3D11_TEXTURE2D_DESC sd2 = {0};
    sd2.Width = CAP_W;
    sd2.Height = CAP_H;
    sd2.MipLevels = 1; sd2.ArraySize = 1;
    sd2.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd2.SampleDesc.Count = 1;
    sd2.Usage = D3D11_USAGE_DEFAULT;
    sd2.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    ID3D11Device_CreateTexture2D(g_dev, &sd2, 0, &g_srcTex);
    ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource*)g_srcTex, 0, &g_srcRtv);
    ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource*)g_srcTex, 0, &g_srcSrv);

    ID3D10Blob_Release(vsB); ID3D10Blob_Release(psB1); ID3D10Blob_Release(psB2);
    IDXGIFactory2_Release(f); IDXGIAdapter_Release(ad); IDXGIDevice1_Release(dd);
    return S_OK;
}

/*
 * FRAME REPEAT — the fix for 60 fps content on a 100 Hz panel.
 *
 * THE PROBLEM, measured: this panel runs 100 Hz (10.000 ms) while the source
 * is 60.00 fps (16.67 ms). The previous loop SKIPPED a present whenever no new
 * capture frame had arrived. With the source slower than the panel that is
 * backwards: we simply left grid slots empty, so presents landed at
 * alternating 16.7 ms / 13.3 ms. Measured jitter 4.7 ms with intervals
 * ranging 9.3-20.7 ms — a perfectly paced 100 Hz stream that looks like 45 fps
 * with random hitching. This is precisely the "measures 60, feels 30-45"
 * report.
 *
 * THE FIX: present on EVERY panel slot, repeating the most recent frame when
 * no new one has arrived. That is what a game does when its frame rate is
 * below the refresh rate. The display then samples a perfectly even 10.000 ms
 * cadence, the eye sees no judder at all, and we never spend GPU re-uploading
 * an unchanged texture. Latency is unchanged: we always present the newest
 * frame that exists.
 *
 * We also do not do this when the source is FASTER than the panel (e.g. 120
 * fps on 60 Hz) — there we must drop, or the present queue grows without
 * bound. The decision is made once, from the two measured rates.
 */
static int g_repeat = 1;   /* panel faster than source -> repeat, don't skip */

/*
 * EXCLUSIVE FULLSCREEN — bypasses DWM entirely.
 *
 * WHY: DXGI refused to report frame statistics on our window:
 *     GetFrameStatistics -> 0x887A0001 (DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
 * That is not a bug in our probe; that error is what a COMPOSITED window
 * returns, because the frame never goes to the panel directly — DWM owns it.
 * So we were flying blind: every "smoothness" number we had was measured
 * before the compositor, i.e. on the one part of the chain we control, while
 * the judder lives after it.
 *
 * In exclusive mode the swap chain IS the display mode. DWM is out of the
 * path, GetFrameStatistics starts working, and we can finally measure the
 * real scanout instead of guessing at it.
 */
/* Native resolution of the monitor the window is on, for exclusive mode. */
static void query_display_res(UINT *w, UINT *h) {
    *w = CAP_W; *h = CAP_H;
    HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi; memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
    if (!mon || !GetMonitorInfoW(mon, (MONITORINFO *)&mi)) return;
    DEVMODEW dm; memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return;
    if (dm.dmPelsWidth > 0 && dm.dmPelsHeight > 0) { *w = dm.dmPelsWidth; *h = dm.dmPelsHeight; }
}

static void RenderFrame(uint8_t *yuy2) {
    /*
     * Raw YUY2 -> GPU. ONE UpdateSubresource, zero per-pixel math, zero Map.
     *
     * The pixel shader does all BT.709 conversion on the GPU.
     *
     * ── WHY THIS IS NOT A Map + 1080-row memcpy LOOP ──────────────────────
     * It used to be exactly that:
     *
     *     Map(g_yuy2Tex, D3D11_MAP_WRITE_DISCARD, ...)      <- 4 MB
     *     for (y = 0; y < 1080; y++) memcpy(dst + y*RowPitch, src + y*stride, 3840);
     *     Unmap(...)
     *
     * with the texture created D3D11_USAGE_DYNAMIC / CPU_ACCESS_WRITE. That
     * shape is the classic source of a periodic hitch, and it is measurably
     * ours rather than upstream: the app's own telemetry showed 41 present
     * dips >25 ms in 60 s while the CAPTURE intervals arriving over the same
     * window were steady (pearson r = 0.05, and 0 of 41 dips had a capture dip
     * within 3 frames). Upstream was exonerated; the stall was in here.
     *
     * Three separate costs, all of them per frame:
     *   1. Map(WRITE_DISCARD) on a 4 MB dynamic texture is not free -- the
     *      driver hands back a new allocation and must serialise against the
     *      GPU still reading the previous one. That wait is the long pole and
     *      it is unbounded, which is exactly what a 25-50 ms dip looks like.
     *   2. WRITE_DISCARD also forces the driver to rename the allocation, so
     *      the texture's memory is never the memory the GPU warmed.
     *   3. 1080 separate 3840-byte memcpys instead of one 4 MB copy: ~5x the
     *      call overhead for byte-identical output.
     *
     * The fix is not a faster loop, it is deleting the loop. The source stride
     * is CAP_W*2 == 3840 and the destination RowPitch for a (CAP_W/2)-wide
     * R8G8R8A8 texture is (CAP_W/2)*4 == CAP_W*2 == 3840. THEY ARE ALREADY
     * EQUAL -- which is why the old loop was copying row for row with no
     * padding to skip. When the strides match, UpdateSubresource does the
     * whole subresource in one driver-optimised upload, straight from the
     * capture buffer we already have in system memory, with no intermediate
     * mapped allocation and no rename.
     *
     * D3D11_USAGE_DEFAULT (not DYNAMIC) is the correct usage for a texture
     * written exclusively by UpdateSubresource; pairing DEFAULT with
     * CPU_ACCESS_WRITE is invalid and CreateTexture2D would fail.
     */
    D3D11_BOX box = {0, 0, 0, (UINT)(CAP_W / 2), (UINT)CAP_H, 1};
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource*)g_yuy2Tex,
                                          0, &box, yuy2,
                                          (UINT)(CAP_W * 2),   /* srcRowPitch */
                                          (UINT)(CAP_W * 2));  /* srcDepthPitch */

    /*
     * NO ClearRenderTargetView HERE. It used to run every frame, before the draw.
     *
     * It is provably dead work: the draw is a 4-vertex TRIANGLESTRIP whose
     * positions span the whole 1920x1080 viewport, and the pixel shader always
     * writes every pixel it is given (YUY2 -> BGRA, no discard, no blending, no
     * depth test). So every pixel of g_rtv is overwritten by the very next call.
     * Clearing first is a full-screen 2.07 Mpixel RTV write that can never be
     * observed.
     *
     * It is not free: on a tiled GPU a clear is often the thing that forces the
     * previous frame's tile memory to resolve, and on a discrete card it is a
     * full 8 MB of write bandwidth per frame competing with the 4 MB texture
     * upload we JUST did. Both are on the same critical path as Present().
     *
     * Every dip in the measured window landed on a frame where we rendered
     * (24 of 24), so this path is the one that has to get cheaper.
     *
     * Removed rather than made conditional: there is no mode in which this
     * clear is visible, and a conditional would only invite someone to re-enable
     * it believing it mattered.
     */
    ID3D11DeviceContext_IASetInputLayout(g_ctx, g_il);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    UINT st=16, of=0;
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &g_vb, &st, &of);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, 0, 0);

        if (!g_exclusive) {
        /* SINGLE PASS at SOURCE resolution. DXGI_SCALING_STRETCH on the swap chain
           stretches this 1920x1080 buffer to the window in fixed-function
           hardware, so there is no upscale shader, no second draw and no extra
           render target for the GPU to pay for. */
        ID3D11DeviceContext_PSSetShader(g_ctx, g_psYuy2, 0, 0);
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &g_yuy2Srv);
        ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtv, 0);
        D3D11_VIEWPORT vp = {0, 0, (float)CAP_W, (float)CAP_H, 0, 1};
        ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
        ID3D11DeviceContext_Draw(g_ctx, 4, 0);
        } else {
        /*
         * EXCLUSIVE FULLSCREEN: two passes.
         *   pass 1  raw YUY2 -> BGRA at 1920x1080 into g_srcTex (source res)
         *   pass 2  g_srcTex -> back buffer at panel res, hardware bilinear
         *
         * Pass 2 is a plain 1-fetch-per-output-pixel textured blit — no chroma
         * math, nothing expensive — and it has to exist at all because in
         * exclusive mode the back buffer IS the display mode, so DXGI cannot do
         * the fixed-function stretch for us the way DXGI_SCALING_STRETCH does in
         * windowed mode.
         */
        ID3D11DeviceContext_PSSetShader(g_ctx, g_psYuy2, 0, 0);
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &g_yuy2Srv);
        ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_srcRtv, 0);
        D3D11_VIEWPORT vp1 = {0, 0, (float)CAP_W, (float)CAP_H, 0, 1};
        ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp1);
        ID3D11DeviceContext_Draw(g_ctx, 4, 0);

        /* Which back buffer to draw into.
                 *
                 * IDXGISwapChain1::GetCurrentBackBufferIndex does not exist anywhere in
                 * this MinGW dxgi.h, so we cannot ask DXGI. For a FLIP_DISCARD chain
                 * with BufferCount==2 the back index is fully determined: it advances
                 * by one on every successful Present. g_bbIndex therefore tracks it
                 * exactly (seeded at 0), and we draw into the buffer that the NEXT
                 * Present will hand to the display — never the front one, which is
                 * what produced the "half-black" frames when this was guessed wrong. */
                ID3D11RenderTargetView *dst = g_bbRtv[g_bbIndex % g_bbCount];
        /* The black clear belongs to the EXCLUSIVE branch only. It used to be
           declared once above and shared by both paths; when the redundant clear
           in the single-pass path was deleted, the declaration went with it and
           this line stopped compiling. Scope it to its only user.

           This one is also dead work by the same argument (full-viewport strip,
           opaque shader, no blending) but it is on the exclusive path, which is
           NOT the default and is not on the measured critical path -- so it
           stays. Do not "fix" it without re-measuring that path. */
        float c[4] = {0,0,0,1};
        ID3D11DeviceContext_ClearRenderTargetView(g_ctx, dst, c);
        ID3D11DeviceContext_PSSetShader(g_ctx, g_psScale, 0, 0);
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &g_srcSrv);
        ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &g_samp);
        ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &dst, 0);
        D3D11_VIEWPORT vp2 = {0, 0, (float)g_bbW, (float)g_bbH, 0, 1};
        ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp2);
        ID3D11DeviceContext_Draw(g_ctx, 4, 0);
        }

    /*
     * FRAME PACING — tear-free by construction, not by vsync.
     *
     * MEASURED: vsync (Present(1,0)) on this 59.94Hz panel was a REGRESSION —
     * it blocked the render loop, fps fell to ~39 and the window went black,
     * because Present(1,0) waits for the vertical blank and our frame arrival
     * is already ~16.7ms: every Present landed a hair late and the frame queue
     * stalled. The dup counters caught it (ren fell behind cap).
     *
     * The correct fix for tear-free 59.94 here is Present(0) with a NEWEST-frame
     * guarantee, not a vsync wait: we only ever present a genuinely new
     * captured frame (see the lastRi check in the main loop), so the frame we
     * show is always the freshest one the device produced, and because source
     * rate and panel rate are both ~60Hz the image changes once per refresh.
     * ALLOW_TEARING is required on the swap chain for this path to be honored.
          *
          * RenderFrame NO LONGER presents. Presentation is owned by the paced main
          * loop, which must present on every panel slot (including slots where the
          * source had nothing new). Presenting here as well would emit two frames
          * per slot and reintroduce the judder we just removed.
          */
     }

/*
 * ── FRAME PACING ─────────────────────────────────────────────────────────
 *
 * THE BUG THIS FIXES (measured: 60.4 fps average but perceived 30-45).
 *
 *   The source is 60.00 fps. This panel is 59.9355582 Hz. Those two numbers
 *   never coincide, so presenting one frame per captured sample eventually
 *   lands a frame after the refresh has already started. With flip-model
 *   Present(0, ALLOW_TEARING) the runtime then CANCELS the remaining screen
 *   time of the previous present and replaces it. The average stays 60, but
 *   some frames are shown for 16.6ms and others for 0ms — which the eye reads
 *   as judder, not smoothness. Research is explicit: "a strict 60 fps cap
 *   drifts against 59.94 Hz and produces a repeating hitch as frames
 *   desynchronize... cap to 59.94 to align cadence with the display."
 *
 *   Presenting on ARRIVAL cannot fix this, because arrival phase drifts
 *   against the panel phase.
 *
 * THE FIX: present on an ABSOLUTE CLOCK locked to the panel's real refresh
 * period. Every present lands at the same phase relative to vblank, so each
 * frame is displayed for exactly one refresh interval.
 *
 * MEASURED NUMBERS, not the 59.94 assumed for most of this project's history:
 * the panel runs 59936/1000 = 59.9360 Hz (16.6845 ms), read at runtime via
 * wubu_qdc (see src/probe_qdc.c). The source is 60.000. The beat between them
 * is 60.000 - 59.936 = 0.064 fps, i.e. one surplus source frame every ~15.6 s.
 * That beat is inherent: no pacer can make 60.000 and 59.936 an integer ratio.
 * It is also not what the hitches were -- see PRESTIGE_HANDOFF_v47.md.
 *
 * Implementation notes:
 *   - The panel's true fractional rate is read at runtime (wubu_qdc), so nothing
 *     here is hardcoded to a particular monitor.
 *   - CREATE_WAITABLE_TIMER_HIGH_RESOLUTION gives sub-millisecond scheduling,
 *     which is required at a ~16.7 ms period where the ~15.6 ms default timer
 *     granularity would be unusable.
 *   - Due times are absolute (n * period from t0) so error never accumulates.
 */
static HANDLE   g_presTimer = NULL;
static int g_periodLocked = 0;

/* Telemetry ring: one record per presented frame, exported to CSV so the
   timing suite can be analysed offline instead of guessed at from counters. */
static wubu_telem *g_telem = NULL;
static double g_telemT0 = 0.0;
static double g_lastPresT = 0.0;

/*
 * WARM-UP GATE.
 *
 * Measured: 8 of 10 recorded dips landed inside the first 4 seconds, which is
 * capture-device init, Media Foundation setup and shader compilation, not
 * gameplay. Those are real startup costs but they are not steady-state
 * behaviour, and leaving them in the ring makes every percentile and every dip
 * count describe a system that never actually ran.
 *
 * So: hold the clock still until the pipeline is genuinely live, then reset
 * every series and the ring in one go. Everything recorded after the gate is
 * steady state, and the numbers finally mean what they claim to mean.
 */
static volatile LONG g_warmupDone = 0;

static double g_lastAge = 0.0;
static long   g_csvEvery = 0;      /* export every N seconds; 0 = never */

/*
 * CSV WRITER THREAD.
 *
 * The export used to run inline on the render loop and it was the single worst
 * thing in the program: a synchronous ~600 KB file write every 10 s, which
 * showed up in the telemetry as frame dips at EXACTLY 10.000 s intervals. The
 * measurement apparatus was manufacturing the defect it was measuring.
 *
 * Now it runs here, below normal priority, and only touches the ring behind the
 * module's lock, so a stall in the filesystem can never reach the pacer.
 */
static DWORD WINAPI telemetry_writer(LPVOID unused) {
    (void)unused;
    /* Below normal: this thread must never compete with the pacer. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    HANDLE stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    while (stop && WaitForSingleObject(stop, (DWORD)(g_csvEvery * 1000)) == WAIT_TIMEOUT) {
        wubu_telem_export_csv(g_telem, "wubu_telemetry.csv");
    }
    if (stop) CloseHandle(stop);
    return 0;
}
static double g_csvNext = 0.0;

/*
 * Present-deadline period. Starts at 0.0, meaning "NOT YET KNOWN".
 *
 * This used to be initialised to 16.6833 -- a hardcoded 59.94 Hz that was
 * overwritten on the first loop iteration but was READABLE before then. Any
 * early return between the two would have paced a 60 fps source against an
 * invented number. Zero is the honest unknown: wait_until_pres() and the pacer
 * both treat it as "no grid yet" and the first relock establishes the real one.
 */
static double g_presPeriodMs = 0.0;
static double   g_nextPres = 0.0;

static double qpc_now(void);   /* defined below, used by the pacer */

/*
 * Read the TRUE refresh period of the monitor our window is ON.
 *
 * ── THIS FUNCTION USED TO LIE, AND THE LIE COST A SESSION ────────────────
 * The old body had:
 *     #if defined(dmDisplayFrequencyFractional)   <-- ALWAYS FALSE
 *     ...
 *     return 16.6833;   // "sane 59.94 Hz fallback"
 *
 * dmDisplayFrequencyFractional is a STRUCT FIELD, not a macro, so
 * "#if defined(field)" can never be true. The fractional branch therefore never
 * compiled, EnumDisplaySettingsW was never even reached on that path, and the
 * program printed "59.940 Hz" for a whole session -- a hardcoded literal
 * dressed as a measurement. Worse, the 0.06 Hz of invented precision looked
 * authoritative, so every downstream decision trusted it.
 *
 * It is now IMPOSSIBLE for this function to invent a rate:
 *   - the real reader lives in wubu_panel.c, where the truncation limitation is
 *     documented and proven (see src/devfreq_diag.c);
 *   - failure returns 0.0, an explicit "unknown", and the caller refuses to pace
 *     rather than substituting a plausible number.
 */
static double query_refresh_ms(void) {
    wubu_panel_display d;
    double hz = wubu_panel_resolve(g_panel, g_hwnd, FALSE, &d);
    if (hz > 0.0 && g_verbose)
        fprintf(stderr, "panel: %s %ux%u @ %.0f Hz (integer field; may be truncated)\n",
                d.device_a, d.width, d.height, hz);
    return hz;   /* 0.0 == unknown. Never a fabricated 59.94. */
}

/*
 * PER-ITERATION WORK TIME.
 *
 * WHY THIS EXISTS
 * Present(g_swap, 1, 0) BLOCKS until the vertical blank. That makes the loop
 * self-clocking, and it also means a frame that overruns the budget does not
 * run late -- it MISSES the blank it was aiming at and Present then waits for
 * the NEXT one. A single overrun therefore costs a full extra refresh period
 * (2x ~= 33.4 ms) instead of the ~8 ms it actually overran by. That is the
 * difference between a hitch the user barely sees and one they absolutely see.
 *
 * So "are we over budget" is the question, and until now nothing measured it.
 * The telemetry recorded the OUTCOME (present interval) and blamed upstream for
 * it, which is why upstream looked suspicious for a whole session. The cost of
 * the work was never recorded at all.
 *
 * This is that number: upload + draw + state, i.e. everything we do between
 * returning from one Present and entering the next. If p99 of this approaches
 * 16.68 ms we are about to start missing blanks, and the fix is to make the
 * work cheaper -- not to touch the pacer.
 */
/* 512 == LAT_N, which is declared further down with the other telemetry rings.
   Spelled as a literal here only because this block sits above that #define;
   keep them equal. */
static double g_work[512];
static volatile LONG g_workN = 0;

/* Sleep until the next absolute present deadline. Returns 0 on timeout. */
/*
 * HYBRID PACER: sleep most of the way, then spin the last millisecond.
 *
 * THE BUG THIS FIXES (measured during gameplay):
 *     present interval p50 16.96ms but MIN 0.83ms and MAX 76.6ms
 *     9 intervals >1.5x median, stddev 3.37ms
 * A 0.83ms interval means we presented twice back-to-back: the previous
 * deadline was serviced late, the next one was already overdue, and we caught
 * up with no regard for the grid. That is the "massive lag" spike.
 *
 * WHY SLEEPING THE WHOLE PERIOD CAUSES IT:
 * A sleeping thread does not wake when it wants to — the scheduler only runs
 * it at the next tick, and it must then win the CPU back from whatever is
 * running. Voicemeeter (78k CPU-sec), Audiodg (23k) and NVIDIA Broadcast all
 * compete for exactly that. The high-resolution waitable timer also degrades
 * when asked to sleep longer than the system timer period: its own
 * documentation warns the precision drops, and that is why we saw multi-
 * millisecond and occasionally 70ms wake-ups.
 *
 * THE FIX (standard practice, cited by the Windows timer literature):
 * sleep with the high-resolution timer only down to ~1.2ms before the
 * deadline, then SPIN on QueryPerformanceCounter for the remainder. A running
 * thread cannot be descheduled, so the final approach is exact. The sleep
 * still yields the CPU for ~94% of the period, so this costs almost nothing.
 *
 * Deadlines stay ABSOLUTE (g_nextPres += period), so no drift accumulates and
 * a late wake-up cannot permanently shift the grid.
 */
/* Source format SPEC constants: the media type we explicitly request. */
#define WUBU_SRC_W   1920
#define WUBU_SRC_H   1080
#define WUBU_SRC_HZ  60.0

/* The revolver: every runtime constant lives in a detected chamber, not here. */
static wubu_rev *g_rev = NULL;
static int g_revLoaded = 0;

/*
 * Pacing authority. The panel is 59.936 Hz (59936/1000, real mode -- NOT
 * 60000/1001; see src/probe_qdc.c for the measurement) and the source is
 * 60.00 Hz, so these two 16.6x ms clocks beat against each other at
 * 60.00 - 59.936 = 0.064 fps. Pacing to the SOURCE therefore guarantees that
 * roughly once every 1/0.064 = 15.6 seconds the panel has nowhere to put a frame
 * and the previous frame is HELD for one refresh. A held frame reads to the eye
 * as a stutter, and no amount of clean Present cadence removes it.
 *
 *   --pace source (default)  present every source frame; expect a periodic hold
 *   --pace panel             present per panel refresh; drop surplus source
 *                            frames instead of ever holding one
 */
static int g_paceToPanel = 0;
static double    g_revSpinTailMs = 1.2;   /* mirrored from the cylinder        */
static unsigned  g_warmupTarget = 90;    /* resolved from the cylinder        */
static double    g_dipThresholdMs = 25.0;/* resolved from the cylinder        */

/* Negotiated media type, filled in during SetCurrentMediaType and read by the
   revolver loader. Globals (not statics) so the loader can see them without
   threading a parameter through the negotiation path. */
double    g_negotiatedFps = 0.0;

/*
 * MEASURED source frame interval, in ms. 0.0 == not yet measured.
 *
 * WHY THIS EXISTS -- this is the v20 regression, found by diffing behaviour.
 *
 * v20 (the build the user considers clean) logged, at runtime:
 *
 *     pacing locked to measured SOURCE 16.5970 ms (60.252 Hz)
 *
 * i.e. it MEASURED the interval frames were actually arriving at and retuned its grid
 * to that, discarding the nominal rate. The measured 16.5970 ms is ~0.5% SLOWER than
 * the panel's 16.6845 ms, so its grid always landed LATE relative to the blank, and a
 * late frame merely waits a little for the next blank -- it can never miss one. That
 * is why v20 holds a worst gap of 18.20 ms (under one extra refresh) with 0.0 dips/min.
 *
 * Every later build threw this away and paced to the nominal 60.000 forever. A nominal
 * 60.000 grid is nominally 16.6667 ms, ~0.1% FAST of the true panel period, and -- as
 * v54/v56 measured -- the sleep+spin pacer overshoots early anyway (16.195 / 16.335 ms
 * actual). Early is the direction that loses blanks.
 *
 * So: measure what the source REALLY delivers and pace to that. It is the only number
 * that accounts for capture-device clock skew and MF jitter, both of which are real
 * here (the capture path was measured delivering lumpy ~2 ms sd intervals).
 */
static double g_measuredSrcMs = 0.0;
unsigned  g_negotiatedW = 0, g_negotiatedH = 0;

/*
 * MMCSS registration for the pacing/present thread.
 *
 * The PS5 runs the game; THIS machine is only the preview. So unlike a local
 * game, boosting this thread cannot starve the thing the player is controlling
 * — it can only compete with the PC's own audio and streaming processes,
 * which is exactly what we want to beat.
 *
 * "Playback" is the correct MMCSS task for us, not "Games": the task profiles
 * describe a viewer who does not notice short glitches, which is precisely a
 * video preview renderer. Priority is requested as ABOVE_NORMAL within the
 * task rather than taking the High (23-26) band, which would run above almost
 * every normal thread on the box and can itself cause stutter in OBS.
 *
 * avrt is loaded dynamically so a missing DLL degrades to "no MMCSS" instead
 * of failing to start.
 */
static HANDLE g_mmcss = NULL;

static void enter_mmcss(void) {
    if (g_mmcss) return;
    typedef HANDLE (WINAPI *PFN)(LPCWSTR, LPDWORD);
    HMODULE avrt = LoadLibraryW(L"avrt.dll");
    if (!avrt) { if (g_verbose) fprintf(stderr, "mmcss: avrt.dll missing\n"); return; }
    PFN set = (PFN)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW");
    if (!set) { if (g_verbose) fprintf(stderr, "mmcss: entrypoint missing\n"); return; }
    DWORD idx = 0;
    g_mmcss = set(L"Playback", &idx);
    if (g_verbose)
        fprintf(stderr, "mmcss: Playback task=%lu handle=%p\n",
                (unsigned long)idx, (void *)g_mmcss);
}

static int wait_until_pres(void) {
    double now = qpc_now();
    double deadline = g_nextPres;
    /* A zero period means "grid not established yet". Advancing by zero would
       pin every deadline to the same instant and spin forever, so establish a
       provisional grid from the SOURCE rate instead -- which is the one cadence
       we know without asking a display. The first successful relock replaces it. */
    double period = (g_presPeriodMs > 0.0) ? g_presPeriodMs : 1000.0 / 60.0;
    g_nextPres += period / 1000.0;              /* absolute, no drift */

    double remain = deadline - now;
    if (remain <= 0.0) return 0;                /* already late: present now */

    double sleepS = remain - g_revSpinTailMs / 1000.0;
    if (sleepS > 0.0) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(sleepS * 10000000.0);   /* 100ns units */
        SetWaitableTimer(g_presTimer, &due, 0, 0, 0, FALSE);
        /* Bounded wait: if we are descheduled past the deadline we must not
           block forever, so cap the sleep and fall into the spin. */
        DWORD ms = (DWORD)(sleepS * 1000.0) + 2;
        WaitForSingleObject(g_presTimer, ms);
    }

    /* Spin the tail. Deadline-relative, so an overslept sleep simply spins
       zero iterations and we present immediately. */
    for (;;) {
            /* Compare in seconds, so we do not depend on a QPC frequency symbol
               being declared before this function. */
            if (qpc_now() >= deadline) break;
            YieldProcessor();      /* pause hint: keeps us out of the scheduler's way */
        }
    return 0;
}
static LARGE_INTEGER g_qpcFreq;
static volatile LONG g_capFrames = 0;      /* samples pulled from the device */
static volatile LONG g_renFrames = 0;      /* frames we actually presented   */
static volatile LONG g_dupRenders = 0;     /* woke up, but index had not moved */
static double g_capLast = 0, g_renLast = 0;

/*
 * Upstream (capture-arrival) health, maintained by the CAPTURE thread.
 *
 * This used to be recomputed inside TickTitle() on the PRESENT thread, which
 * meant the deadline thread was doing O(n log n) work for a number that is
 * diagnostic only and that nobody looks at between title updates. Now the
 * capture thread refreshes it on its own cadence and the present thread only
 * reads three doubles.
 */
static double g_capJitAvg = 0, g_capJitSd = 0, g_capJitMax = 0;

/* Plain frame counter, incremented by the capture thread with no I/O attached,
   so the present thread can do the printing. See the note in CaptureThread. */
static volatile LONG g_capFrameCount = 0;

/* Rolling window of per-frame AGE: time from "sample pulled off the device"
   to "we are about to draw it". This is the number that actually corresponds
   to what the player perceives, and it is the only honest latency metric. */
#define LAT_N 512
static double g_lat[LAT_N];
static volatile LONG g_latN = 0;
/* Timestamp of when each triple-buffer slot was filled by the capture thread,
   so the render thread can measure how old the frame it is drawing actually is. */
static double g_bufT[3];
/* Interval between consecutive samples ARRIVING at the capture thread. If this
   is uneven the stall is upstream (capture card / USB); if it is steady while
   our present intervals are not, the stall is ours. This is the measurement
   that finally separates the two, which a single latency number never could. */
static double g_capIv[LAT_N];
static volatile LONG g_capIvN = 0;
static double lastCapT = 0.0;
static LARGE_INTEGER g_qpcFreq;

static double qpc_now(void) {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)g_qpcFreq.QuadPart;
}

/*
 * Refresh the upstream-jitter figures. Called from the capture thread roughly
 * once a second, where an O(n) pass costs nothing because that thread spends
 * ~98% of its life blocked in ReadSample anyway.
 *
 * Deliberately NOT called from the present thread. That separation is the whole
 * point: work that a deadline thread must not pay for belongs to a thread that
 * is not on the deadline.
 */
static void update_cap_jitter(void) {
    LONG cn = g_capIvN; if (cn > LAT_N) cn = LAT_N;
    if (cn <= 2) return;
    g_capJitAvg = wubu_stat_mean(g_capIv, cn);
    g_capJitSd  = wubu_stat_stddev(g_capIv, cn);
    g_capJitMax = wubu_stat_max(g_capIv, cn);
}

/*
 * DELETED: measured_source_period_ms() and its qsort.
 *
 * This used to qsort up to 512 arriving-sample intervals and then take the MEAN
 * of them -- an O(n log n) sort feeding a statistic that does not depend on
 * order at all, so the entire sort and its 4 KB stack scratch were computing a
 * value nobody read. Worse, the comment directly beneath the sort explained why
 * the MEDIAN is the wrong choice here, while the sort that would have produced a
 * median sat two lines above it.
 *
 * The pacing authority now comes from the revolver's source-rate chamber and the
 * panel from wubu_panel, so this function had no callers left. Deleting it is the
 * honest fix: there is no rate to measure any more that is not measured
 * somewhere that matters (the negotiated media type, and the display itself).
 */


/* Present-to-present intervals. Variance here IS the judder; average fps hides it. */
static double g_iv[LAT_N];
static volatile LONG g_ivN = 0;
static double lastPres = 0;

static void TickTitle(void) {
    static LONG prevN = 0;
    static LONG prevPres = 0;
    /*
     * THE fps COUNTER WAS COUNTING THE WRONG THING, AND IT MISLED A WHOLE
     * INVESTIGATION.
     *
     * It used to divide ELAPSED TIME BY g_renFrames -- the number of frames we
     * actually RENDERED. But the main loop iterates once per PANEL SLOT, and it
     * only renders when a new capture frame has arrived. Those are different
     * counts, so the quotient is not a frame rate at all.
     *
     * Measured contradiction, same run, same second:
     *     title bar  : 49.5 fps
     *     our own CSV: 59.85 Hz  (mean pres_iv 16.707 ms)
     *     kernel ETW : 59.9 display flips/s   (DxgKrnl id 215)
     * Two independent measurements agree on ~60. The title bar was the odd one
     * out, and it is the number a human reads to decide whether the app is
     * healthy. A wrong indicator is worse than no indicator.
     *
     * Fix: divide by the PRESENT count, which is the loop's own rate and the
     * thing actually being paced. Rendering is reported separately as
     * ren/dup, because "we drew 50 of 60 slots" is a real and useful fact about
     * a 60 fps source on a 59.94 Hz panel -- it just is not the frame rate.
     */
    LONG n = g_ivN;
    if (n - prevPres < 20) return;      /* ~3 updates/sec at 60fps */
    double now = qpc_now();
    double dts = now - g_renLast;
    g_renLast = now;
    double fps = dts > 0 ? (double)(n - prevPres) / dts : 0.0;
    prevPres = n;

    /*
     * ── THIS FUNCTION WAS A PERIODIC HITCH, AND IT WAS OURS ──────────────
     * It used to declare THREE 512-double arrays on the stack (12 KB) and run
     * THREE qsorts of up to 512 elements each -- on the PRESENT thread, every
     * 20 frames, i.e. three times a second. A sort that size is not free, and
     * 12 KB of fresh stack is first-touch page faults the first time through.
     *
     * That is a textbook manufactured dip: a bounded, repeating, on-thread cost
     * sitting between the pacer and Present(). The ETW trace shows the symptom
     * (periodic outliers); this was the cause.
     *
     * The fix is not "do it less often" -- that only moves the stall. The fix is
     * to remove the work from the thread that has a deadline:
     *   - the arrays become file-scope, allocated ONCE, so no per-call stack
     *     traffic and no first-touch faults;
     *   - the p95 is computed by a single selection pass instead of a full sort,
     *     which is O(n) and touches each element once;
     *   - the cap-jitter block, which is diagnostic-only and never shown to the
     *     user, is computed on the CAPTURE thread's own cadence via
     *     update_cap_jitter() instead of here.
     * The result is identical and the present thread does no O(n log n) work at
     * all.
     */

    /* Latency stats over the rolling window. */
        LONG cnt = g_latN; if (cnt > LAT_N) cnt = LAT_N;
        double lavg = 0, lp95 = 0;
        if (cnt > 0) {
            lavg  = wubu_stat_mean(g_lat, cnt);
            lp95  = wubu_stat_percentile(g_lat, cnt, 0.95);

        }

        /* Present-interval stats: the honest smoothness number. Low jitter == smooth. */
        LONG ic = g_ivN; if (ic > LAT_N) ic = LAT_N;
        double ijit = 0, imin = 0, imax = 0;
        if (ic > 2) {
            /* mean omitted: the cadence is locked to the target, so it carries no
               information the target itself does not. stdev/min/max do. */
            ijit = wubu_stat_stddev(g_iv, ic);
            imin = wubu_stat_min(g_iv, ic);
            imax = wubu_stat_max(g_iv, ic);
        }

        /* Upstream health is maintained by update_cap_jitter() on its own
           cadence -- see the note above. Read it, do not recompute it here. */
        double cavg = g_capJitAvg, cjit = g_capJitSd, cmax = g_capJitMax;

        /*
         * WORK TIME vs THE BUDGET. This is the number that predicts hitches.
         *
         * Present(SyncInterval=1) blocks until the vertical blank, so the loop
         * has exactly one refresh period (16.68 ms here) to finish everything
         * between one Present returning and the next being called. Work that
         * exceeds that does not merely run late -- it misses the blank, and the
         * present interval jumps to a full extra refresh (2x ~= 33.4 ms).
         *
         * So print work p95 and max against the period. When max approaches or
         * crosses the period, the hitches are arithmetic, not mysterious.
         */
        LONG wc = g_workN; if (wc > LAT_N) wc = LAT_N;
        double w95 = 0, wmax = 0;
        if (wc > 2) {
            w95  = wubu_stat_percentile(g_work, wc, 0.95);
            wmax = wubu_stat_max(g_work, wc);
        }

        char buf[320];
        snprintf(buf, sizeof buf,
            "WuBuCapture  %.1f fps (ren %ld/dup %ld)  lat %.1f/%.1f"
            "  pres %.2f/%.1f/%.1f  work %.2f/%.2f  cap %.2f/%.1f  %s%.2fms",
            /* fps, rendered, duplicate-slots, lat mean, lat p95,
               pres stdev, pres min, pres max, work p95, work max,
               cap stdev, cap max, pacing authority label, target period */
            fps, (long)g_renFrames, (long)g_dupRenders,
            lavg, lp95, ijit, imin, imax, w95, wmax, cjit, cmax,
            /* Name the clock. A bare "tgt 0.00ms" looks like a broken counter; it
               actually means "no self-pacer, the compositor owns vsync". */
            g_presPeriodMs > 0.0 ? (g_paceToPanel ? "tgt(panel) " : "tgt(src) ")
                                : "compositor",
            g_presPeriodMs);
        SetWindowTextA(g_hwnd, buf);

        /*
         * The capture-frame heartbeat moved HERE from CaptureThread().
         * Console I/O belongs to whichever thread is not on a deadline. This one
         * is already inside its 20-frame budget doing a handful of O(n) passes,
         * and a blocked write is visible in the ETW trace rather than silently
         * starving the capture device.
         */
        if (g_verbose) {
            static LONG prevCap = 0;
            LONG c = g_capFrameCount;
            if (c - prevCap >= 600) {
                prevCap = c;
                fprintf(stderr, "capture: %ld frames\n", (long)c);
            }
        }
    }

static DWORD WINAPI CaptureThread(void *arg) {
    (void)arg;
    while (g_running) {
        DWORD si, fl; LONGLONG ts; IMFSample *sp = NULL;
        HRESULT hr = IMFSourceReader_ReadSample(g_reader,
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &si, &fl, &ts, &sp);
        if (FAILED(hr) || !sp) { Sleep(1); continue; }

        IMFMediaBuffer *mb = 0;
        IMFSample_ConvertToContiguousBuffer(sp, &mb);
        IMFSample_Release(sp);
        if (!mb) continue;

        BYTE *d = 0; DWORD ml, cl;
        IMFMediaBuffer_Lock(mb, &d, &ml, &cl);

        if (d && cl >= (DWORD)CAP_W * CAP_H * 2) {
            /* Copy YUY2 to write buffer — NO CPU conversion */
            int wi = g_wrIdx;
            memcpy(g_bufs[wi], d, (size_t)CAP_W * CAP_H * 2);
            {
                double nowT = qpc_now();
                g_bufT[wi] = nowT;               /* stamp: this frame is now "new" */
                LONG k = (InterlockedIncrement(&g_capIvN) - 1) & (LAT_N - 1);
                if (lastCapT > 0.0) g_capIv[k] = (nowT - lastCapT) * 1000.0;
                lastCapT = nowT;
            }
            InterlockedIncrement(&g_capFrames);

            /*
             * UPSTREAM HEALTH, REFRESHED HERE ON PURPOSE.
             * Once a second, on THIS thread. It used to run on the present
             * thread inside TickTitle(); see update_cap_jitter() for why that
             * was a periodic hitch. This thread is blocked in ReadSample ~98% of
             * the time, so an O(n) pass here is free, and it keeps the deadline
             * thread doing nothing but present.
             */
            {
                static LONG jitTick = 0;
                if (++jitTick >= 60) { jitTick = 0; update_cap_jitter(); }
            }

            /* Lock-free buffer swap */
            LONG ri = g_rdIdx, fi = g_frIdx;
            InterlockedExchange(&g_rdIdx, wi);
            InterlockedExchange(&g_wrIdx, fi);
            InterlockedExchange(&g_frIdx, ri);
            SetEvent(g_frameEvent);
            /*
             * NO I/O IN THE CAPTURE HOT LOOP, NOT EVEN UNDER -v.
             *
             * stderr is a synchronised console handle: a write to it can block
             * on the console host, and this thread is the one that must never
             * stall or OBS drops frames. The old code still wrote here whenever
             * -v was passed, and the verbose logs are exactly what a diagnostic
             * run uses -- so the instrumentation was perturbing the thing it
             * measured, on the one thread that cannot afford it.
             *
             * Instead: count frames and let the PRESENT thread (which has no
             * I/O to protect and a deadline it is already servicing) do the
             * printing. A stalled present is visible in the ETW trace; a stalled
             * capture thread is not.
             */
            {
                static LONG fc = 0; fc++;
                g_capFrameCount = fc;
            }
        } else if (d) {
            static int once = 0;
            if (!once++) {
                /* One-shot diagnostic on a cold path (a wrong-sized sample is
                   not per-frame), so a single stderr write is acceptable here. */
                fprintf(stderr, "small sample: %u (need %u)\n", cl, CAP_W*CAP_H*2);
            }
        }

        IMFMediaBuffer_Unlock(mb);
        IMFMediaBuffer_Release(mb);
    }
    return 0;
}

/* Device selection */
static IMFActivate **g_devices = NULL;
static UINT32 g_devCount = 0;
static WCHAR **g_devNames = NULL;

static void EnumDevices(void) {
    IMFAttributes *attr;
    MFCreateAttributes(&attr, 1);
    IMFAttributes_SetGUID(attr, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    MFEnumDeviceSources(attr, &g_devices, &g_devCount);
    IMFAttributes_Release(attr);

    g_devNames = (WCHAR**)calloc(g_devCount, sizeof(WCHAR*));
    if (g_verbose) {
        fprintf(stderr, "\n=== Video Capture Devices ===\n");
        for (UINT32 i = 0; i < g_devCount; i++) {
            UINT32 nl;
            IMFActivate_GetAllocatedString(g_devices[i], &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &g_devNames[i], &nl);
            if (g_devNames[i])
                fwprintf(stderr, L"  [%u] %s\n", i, g_devNames[i]);
            else
                fprintf(stderr, "  [%u] (unknown)\n", i);
        }
        fprintf(stderr, "Press number key 0-%u to switch device\n\n", g_devCount - 1);
    } else {
        /* Still need the names populated for the USB3 auto-select below. */
        for (UINT32 i = 0; i < g_devCount; i++) {
            UINT32 nl;
            IMFActivate_GetAllocatedString(g_devices[i], &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &g_devNames[i], &nl);
        }
    }
}

static HRESULT InitMF(int devIdx) {
    if (devIdx < 0 || (UINT32)devIdx >= g_devCount) return E_INVALIDARG;

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) return hr;

    /*
     * MF_LOW_LATENCY must be set on the SOURCE ACTIVATION attributes, BEFORE
     * ActivateObject.
     *
     * We previously set it on the already-created IMFSourceReader via
     * QueryInterface(IMFAttributes). That failed (hr was never even 0), and it
     * is wrong regardless: by the time the reader exists, the media source has
     * already been activated and its internal buffering/jitter-buffer policy
     * has already been decided. Setting it post-activation is a no-op.
     *
     * Without it, Media Foundation is free to buffer and re-time samples to
     * smooth delivery. That buffering is invisible in our frame counters —
     * frames still arrive on time — but it adds whole frames of age to
     * everything on screen. That is precisely the "video looks right but
     * everything feels laggy" symptom.
     *
     * Value is the documented one: {9c27891a-ed7a-40e1-88e8-b22727a024ee}.
     * This MinGW header does not export MF_LOW_LATENCY, so we declare it here.
     */
    {
        static const GUID MF_LOW_LATENCY_ =
            {0x9c27891a,0xed7a,0x40e1,{0x88,0xe8,0xb2,0x27,0x27,0xa0,0x24,0xee}};
        /* This SDK has no COBJMACROS helper for IMFActivate::GetAttributes.
           IMFActivate derives from IMFAttributes, so QueryInterface is the
           correct route (a hard cast would rely on vtable layout and has
           already bitten us once in this file). */
        IMFAttributes *act = 0;
        HRESULT ha = IMFActivate_QueryInterface(g_devices[devIdx], &IID_IMFAttributes,
                                                (void **)&act);
        if (SUCCEEDED(ha) && act) {
            HRESULT hs = IMFAttributes_SetUINT32(act, &MF_LOW_LATENCY_, 1);
            if (g_verbose)
                fprintf(stderr, "MF_LOW_LATENCY on activate attrs hr=0x%08lX\n",
                        (unsigned long)hs);
            if (FAILED(hs))
                fprintf(stderr, "warn: could not set MF_LOW_LATENCY (0x%08lX)\n",
                        (unsigned long)hs);
            IMFAttributes_Release(act);
        } else {
            fprintf(stderr, "warn: GetAttributes failed 0x%08lX\n", (unsigned long)ha);
        }
    }

    IMFMediaSource *src = NULL;
    hr = IMFActivate_ActivateObject(g_devices[devIdx], &IID_IMFMediaSource, (void**)&src);
    if (FAILED(hr) || !src) {
        fprintf(stderr, "ActivateObject failed: 0x%08X\n", hr);
        return E_FAIL;
    }
    if (g_devNames[devIdx])
        fwprintf(stderr, L"Selected: [%d] %s\n", devIdx, g_devNames[devIdx]);
    else
        fprintf(stderr, "Selected: device [%d]\n", devIdx);

    hr = MFCreateSourceReaderFromMediaSource(src, NULL, &g_reader);
    IMFMediaSource_Release(src);
    if (FAILED(hr)) return hr;

    /*
     * MF_LOW_LATENCY — the knob we were never turning.
     *
     * Without this, Media Foundation is free to buffer and re-time samples
     * internally to smooth delivery. That buffering is invisible to us (the
     * sample still arrives on time) but it adds whole frames of age to every
     * picture, which is precisely the "lag that isn't frame drops" symptom.
     * Setting it tells MF to hand us samples the instant the device has them.
     *
     * MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING is deliberately left FALSE:
     * we do not want MF inserting a video processor, because we want the raw
     * YUY2 straight from the device and do the colour maths on our own GPU.
     */
/* Request YUY2 */
    IMFMediaType *mt;
    MFCreateMediaType(&mt);
    IMFAttributes_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFAttributes_SetGUID(mt, &MF_MT_SUBTYPE, &MFVideoFormat_YUY2);
    IMFAttributes_SetUINT64(mt, &MF_MT_FRAME_SIZE, ((UINT64)CAP_W<<32)|CAP_H);
    /*
     * Record what we actually negotiated. These feed the revolver, so the
     * pacing period is derived from the rate the DEVICE agreed to rather than
     * from a literal in this file. MF reports an exact rational, so the value
     * is exact -- unlike dmDisplayFrequency, which truncates.
     */
    /*
     * The source format is a SPEC, not a measurement: we construct this media
     * type ourselves and demand 1920x1080 @ 60/1, and SetCurrentMediaType
     * returns S_OK. Reading it back through IMFAttributes in this MinGW is not
     * reliable -- GetUINT32 on the two-UINT32 MF_MT_FRAME_SIZE blob returned
     * pointer garbage (3222091453x3222091494) -- so we do NOT pretend to detect
     * what we specified. These values are the format constants, and the
     * revolver records them as such rather than dressing them up as measured.
     */
    g_negotiatedW   = WUBU_SRC_W;
    g_negotiatedH   = WUBU_SRC_H;
    g_negotiatedFps = WUBU_SRC_HZ;
    if (g_verbose)
        fprintf(stderr, "source spec: %ux%u @ %.6f Hz (MF_SOURCE_READER "
                        "hr=0x%08lX)\n",
                g_negotiatedW, g_negotiatedH, g_negotiatedFps,
                (unsigned long)hr);

    IMFSourceReader_SetCurrentMediaType(g_reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, mt);
    IMFMediaType_Release(mt);

    g_bufs[0] = malloc((size_t)CAP_W*CAP_H*2);
    g_bufs[1] = malloc((size_t)CAP_W*CAP_H*2);
    g_bufs[2] = malloc((size_t)CAP_W*CAP_H*2);
    g_wrIdx = 0; g_rdIdx = 1; g_frIdx = 2;
    g_frameEvent = CreateEvent(0, 0, 0, 0);
    return S_OK;
}

static volatile int g_needResize = 0;

/*
 * WINDOW RESIZE — with DXGI_SCALING_STRETCH the back buffer must stay at the
 * SOURCE resolution (1920x1080) for the life of the swap chain. DXGI owns the
 * stretch to whatever the window happens to be, in fixed-function hardware.
 *
 * The previous implementation called ResizeBuffers(w, h) on every WM_SIZE,
 * which resized the back buffer to the WINDOW size and so defeated the whole
 * design — the picture stopped tracking the window and resizing broke. It also
 * passed ALLOW_TEARING as the ResizeBuffers flags argument, which is only
 * valid as a creation-time flag, and ClearState() on a path that runs mid-
 * stream.
 *
 * So: record the new client size, and let DXGI do the rest. No buffer churn,
 * no RTV rebuild, nothing to go wrong, and resize becomes free.
 */
static void ResizeSwapChain(int w, int h) {
    if (w <= 0 || h <= 0) { g_needResize = 0; return; }
    g_winW = w;
    g_winH = h;
    g_needResize = 0;
}

/*
 * ── THE F-TIME FIX ───────────────────────────────────────────────────────
 * Re-detect the target display when the window MOVES, not only at startup.
 *
 * THE BUG THIS FIXES (the user's own words: "when I press F it's on the monitor
 * I want it, can detect then too"):
 *   Everything was detected ONCE, at startup, before the window had a position.
 *   A monitor lookup against a window with no rect falls back to NEAREST, which
 *   on this box returned DISPLAY23 -- a 120 Hz portrait display -- while the
 *   window actually sits on DISPLAY22, the 59 Hz main panel. So the app paced a
 *   60.000 fps source against a 120 Hz panel, believed 8.33 ms was the target,
 *   and every fix built on that number was fixing the wrong problem.
 *
 * WHAT IS ACTUALLY DONE HERE, IN ORDER:
 *   1. resolve the monitor the USER is pointing at (GetCursorPos), falling back
 *      to the primary -- never to a cached startup answer;
 *   2. read that device's refresh from the integer DEVMODE field, which is the
 *      only truthful signal on this SDK (proven in src/devfreq_diag.c);
 *   3. compute how many panel refreshes one source frame should occupy;
 *   4. re-lock the pacer to the number just measured;
 *   5. LOG THE DEVICE NAME EVERY TIME, so the log can never again disagree
 *      with the window's real position.
 *
 * If the refresh cannot be read, we say so and refuse to invent one. The old
 * code's answer to that situation was `return 16.6833`, which is a hardcoded
 * literal masquerading as a measurement.
 */
static void relock_target_panel(BOOL useCursor) {
    wubu_panel_display d;
    double hz = wubu_panel_resolve(g_panel, g_hwnd, useCursor, &d);

    /* Record which monitor we are now locked to, so the main loop's
       MonitorFromWindow() comparison can detect a later window move. Kept here
       (not at the call sites) so every path that locks stays in sync. */
    g_lockedMon = g_hwnd ? MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY) : NULL;

    if (hz <= 0.0) {
        g_targetHz = 0.0;
        g_syncInterval = 0;
        g_targetDev[0] = 0;
        fprintf(stderr, "PANEL: refresh rate UNREADABLE. Not guessing one. "
                        "Falling back to the tearing path, which needs no panel rate.\n");
        g_tear = 1;
        return;
    }

    double src = (g_negotiatedFps > 1.0) ? g_negotiatedFps : 60.0;
    unsigned iv = wubu_panel_sync_interval(src, hz);

    /* ASCII copy: g_targetDev is narrow, so a wcsncpy here would be a type
       error and a plain strncpy from the WCHAR would be worse. Use the
       module's own ASCII field. */
    strncpy(g_targetDev, d.device_a, 63);
    g_targetDev[63] = 0;
    g_targetHz = hz;
    g_syncInterval = iv;

    fprintf(stderr, "PANEL LOCK: %s  %ux%u  %.0f Hz  (%.4f ms/refresh)\n",
            d.device_a, d.width, d.height, hz, 1000.0 / hz);
    fprintf(stderr, "  source %.3f fps -> %s\n", src, wubu_panel_ratio_note(src, hz));
    if (iv)
        fprintf(stderr, "  sync interval %u: each source frame occupies exactly "
                        "%u panel refresh(es). Even cadence.\n", iv, iv);
    else
        fprintf(stderr, "  no even integer interval exists on this pairing; "
                        "the repeat pattern must alternate and judder is inherent.\n");

    /* Re-lock the pacer. Deadlines stay on the absolute grid so a re-lock never
       shifts phase -- it only changes the period from here forward.

       In vsync mode there IS no grid: Present(iv) blocks to the blank and that
       is the clock. Setting a period here would reintroduce the second,
       fictional clock that caused the hitches (see the note at the pace-authority
       site). Only the tearing path keeps a self-imposed deadline. */
    if (g_tear) {
        g_presPeriodMs = 1000.0 / hz * (double)(iv ? iv : 1);
        g_nextPres = qpc_now() + g_presPeriodMs / 1000.0;
    } else if (iv) {
        /* An EVEN panel cadence exists, so DXGI owns vsync and there is no grid. */
        g_presPeriodMs = 0.0;
    }
    /*
     * When iv == 0 there is NO EVEN PANEL CADENCE (60.000 fps on the 99.982 Hz
     * primary is 1.667 panels per frame). In that case we must NOT zero the grid:
     * the source owns the clock at its measured interval, and DXGI cannot be
     * relied on to space the presents -- with g_presPeriodMs left at 0 the loop
     * presented unthrottled and the measured interval collapsed to 0.192 ms.
     *
     * This used to be an unconditional `else { g_presPeriodMs = 0.0; }`, which
     * silently discarded the source-owned grid every time the panel was re-locked.
     * Leave it alone here and let the measured-source re-lock keep it.
     */
}

static void ToggleFS(void) {
    static WINDOWPLACEMENT wp = {sizeof(WINDOWPLACEMENT)};
    static int fs = 0;
    fs = !fs;
    if (fs) {
        GetWindowPlacement(g_hwnd, &wp);
        /*
         * Pick the monitor BEFORE moving, but from the CURSOR, not from the
         * window. The window's current centre is where it is now, which is
         * precisely the monitor the user is trying to LEAVE.
         */
        POINT pt; POINT ctr = { 0, 0 };
        if (GetCursorPos(&pt)) ctr = pt;
        HMONITOR mon = MonitorFromPoint(ctr, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = {sizeof(mi)};
        if (!GetMonitorInfo(mon, &mi)) { fs = 0; return; }
        int w = mi.rcMonitor.right - mi.rcMonitor.left;
        int h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        /* Refuse an implausible mode rather than driving a display into it.
           This is the exact trap that produced the DISPLAY24 white-screen
           freeze: a portrait resolution was requested on the main panel. */
        if (w < 640 || h < 480 || w > 16384 || h > 16384) {
            fprintf(stderr, "Refusing fullscreen to %dx%d -- implausible mode.\n", w, h);
            fs = 0;
            return;
        }
        SetWindowLong(g_hwnd, GWL_STYLE, WS_POPUP);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, w, h, SWP_SHOWWINDOW);
        g_needResize = 1;
        g_winW = w; g_winH = h;
        fprintf(stderr, "Fullscreen: %dx%d\n", w, h);
    } else {
        SetWindowLong(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW|WS_VISIBLE);
        SetWindowPlacement(g_hwnd, &wp);
        RECT rc; GetClientRect(g_hwnd, &rc);
        g_needResize = 1;
        g_winW = rc.right; g_winH = rc.bottom;
        fprintf(stderr, "Windowed: %dx%d\n", g_winW, g_winH);
    }
    /* THE POINT OF THE WHOLE EXERCISE: the window has moved, so every display
       fact we cached is now suspect. Re-measure against the panel the WINDOW
       is actually on.

       useCursor = FALSE, and this is a real bug fix, not a preference. The
       fullscreen toggle MOVES the window to the cursor's monitor (see above),
       so afterwards the window's monitor and the cursor's monitor are only
       the same by luck. Passing TRUE here re-read the cursor's monitor AFTER
       the move, which picked a different panel than the one we just moved to:
       on this 3-monitor rig it locked DISPLAY23 (1440x2560 portrait, 120 Hz)
       while the window sat on DISPLAY22 (3440x1440, 99.982 Hz). The cadence was
       then computed for the wrong panel -- and at 99.982 Hz there is no even
       integer sync interval for a 60 fps source, so that error is exactly what
       produces the alternating 1-then-2-refresh judder the user reports.

       Follow the window. That is the display that will actually show it. */
    relock_target_panel(FALSE);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { g_running = FALSE; PostQuitMessage(0); return 0; }
    if (m == WM_KEYDOWN) {
        if (w == VK_ESCAPE) { g_running = FALSE; PostQuitMessage(0); return 0; }
        if (w == 'F') { ToggleFS(); return 0; }
        /* Number keys 0-9 to switch device */
        if (w >= '0' && w <= '9') {
            int idx = w - '0';
            if (idx < (int)g_devCount) {
                fprintf(stderr, "Switching to device %d...\n", idx);
                if (g_reader) { IMFSourceReader_Release(g_reader); g_reader = NULL; }
                InitMF(idx);
            }
        }
    }
    if (m == WM_SIZE) {
        int nw = LOWORD(l), nh = HIWORD(l);
        if (nw > 0 && nh > 0 && (nw != g_winW || nh != g_winH)) {
            g_winW = nw; g_winH = nh;
            g_needResize = 1;
        }
    }
    return DefWindowProc(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    (void)hp;(void)show;

    /* --- args: -v verbose, --full-gpu never yield, --dev N pick device --- */
    int wantDev = -1;
    if (cmd) {
        if (strstr(cmd, "-v")) g_verbose = 1;
        if (strstr(cmd, "--full-gpu")) g_stream_mode = 0;
        const char *cs = strstr(cmd, "--csv");
        if (cs) g_csvEvery = atol(cs + 5);
        /* EXPORT INTERVAL IS PART OF THE MEASUREMENT. A ~400 KB write every
           10 s produced frame dips at exactly the export interval (proven by
           A/B: --csv 10 -> 7 periodic dips, --csv 200 -> 0). Default is a
           coarse 120 s so telemetry never perturbs what it is measuring. */
        if (strstr(cmd, "--tear")) g_tear = 1;
        /*
         * WHICH CLOCK DRIVES US -- MEASURED, AND THE ANSWER IS "NOT OURS".
         *
         * The self-pacer was removed as the default after measuring both ways on
         * DISPLAY22 with the telemetry-export artifact eliminated:
         *
         *   self-paced to the true panel period : mean 16.195 ms, sd 2.860, worst 49.44
         *   DXGI owns vsync (no self-pacer)     : mean 16.684 ms, sd 0.575, worst 31.65
         *   v20, the user's known-good baseline : mean 16.519 ms, sd 1.252, worst 18.20
         *
         * True panel period is 1000/59.936 = 16.6845 ms. The self-paced mean of
         * 16.195 ms is ~3% FASTER than the panel: the grid fires early every
         * slot, walks steadily earlier across the display, and eventually lands
         * just AFTER a blank -- which costs a full extra refresh (49.44 ms).
         * Handing the clock to DXGI snaps the mean onto the panel period exactly
         * (16.684 measured vs 16.6845 expected) and halves the jitter.
         *
         * So on a panel we have measured, we do not imitate the panel with a
         * timer set exactly equal to it -- we bias it SLOW. See the "biased
         * slow" note at the assignment below for why arriving late is safe and
         * arriving early is not.
         *
         * --pace source selects the raw source period for A/B, and the TEARING
         * path still needs a self-imposed clock because nothing else imposes
         * one there.
         */
        if (strstr(cmd, "--pace panel") || strstr(cmd, "--pace source"))
            g_paceToPanel = 1;      /* opt-in A/B only */
        else
            g_paceToPanel = 0;      /* DEFAULT: DXGI owns vsync */
        /*
         * WINDOWED IS THE DEFAULT. Exclusive fullscreen is OFF unless asked
         * for explicitly with -fs.
         *
         * REVERTED 2026-10-01. Making exclusive the default was wrong and it
         * froze the machine: query_display_res() returned a bogus 1440x2560
         * portrait mode instead of the panel's 3440x1440, and
         * SetFullscreenState then switched the display into that garbage
         * mode — white screen, unresponsive desktop.
         *
         * The underlying idea (take DWM out of the presentation path) is still
         * sound, but driving it through a mode change is not something to ship
         * as a default when the mode we would request is not yet proven
         * correct. It stays opt-in and experimental.
         */
        g_exclusive = 0;
        if (strstr(cmd, "-fs")) g_exclusive = 1;
        char *d = strstr(cmd, "--dev ");
        if (d) wantDev = atoi(d + 6);
    }

    /*
     * STREAM MODE (default ON).
     *
     * This box is a 6-core Ryzen 3600 running a game + OBS + NVENC + Camo +
     * VoiceMeeter. When OBS is streaming, our process is a guest. Running the
     * whole process BELOW_NORMAL means the OS scheduler always prefers the
     * game and the encoder over us. Our own latency is unaffected because
     * both of our hot threads block on kernel waits (ReadSample / MsgWait)
     * for ~16ms at a time — they are never CPU-bound, they are wait-bound.
     * Priority only decides who wins when everything wants CPU at once.
     */
    if (g_stream_mode)
        SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

    QueryPerformanceFrequency(&g_qpcFreq);
        g_renLast = qpc_now();

        /*
         * High-resolution waitable timer for frame pacing. The default Windows
         * timer granularity is ~15.6ms, which is unusable against a 16.683ms
         * refresh period — it would quantise every deadline to whole ticks and
         * reintroduce exactly the judder we are removing.
         */
        /*
         * PACING SOURCE — this is the fix for the remaining 1.6% phase error.
         *
         * dmDisplayFrequency truncates: this panel is really 59.94 Hz
         * (60000/1001) but the field reads 59, so the old code paced to
         * 16.949 ms against a panel whose true period is 16.683 ms. That
         * mismatch accumulates every frame and is a proven cause of dips.
         *
         * We cannot read the exact fraction on this toolchain: DXGI_MODE_DESC
         * is only in dxgitype.h, IDXGIOutputVtbl in dxgi.h has no
         * GetCurrentDisplayMember at all, and DEVMODEW has no
         * dmDisplayFrequencyFractional. Hand-rolling the vtable by ordinal
         * segfaulted or returned E_INVALIDARG, because these headers predate
         * the methods they are supposed to describe.
         *
         * So we do not need the panel rate at all: what we actually want is to
         * present one frame per SOURCE frame. The source is 60.000 Hz
         * (16.6667 ms), measured directly from the capture callback, and it is
         * within 0.06 Hz of the panel's 59.94 - so 1:1 presentation holds and
         * the residual drift costs one held frame every ~17 s.
         */
        /*
         * PACING PERIOD = the source's SPECIFIED rate, not a measured one.
         *
         * The device negotiates a fixed YUY2 1920x1080 @ 60/1, so 16.6667 ms
         * is a constant of the format, not something to be estimated.
         *
         * Measuring it was tried and rejected: feedback from the measured
         * arrival interval is unstable, because one long upstream gap inflates
         * the mean, which lengthens the period, which lowers the delivered rate
         * and drifts further. Three builds in a row landed on 16.16, 16.66 and
         * 17.98 ms median purely from that feedback. A spec constant cannot
         * drift.
         *
         * This also removes the panel-rate problem outright: dmDisplayFrequency
         * truncates 59.94 to 59, and pacing from that integer was a systematic
         * 1.6% phase error. We now pace from the source, which is within
         * 0.06 Hz of the panel, so presentation stays 1:1.
         */
        g_presTimer = CreateWaitableTimerExW(NULL, NULL,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!g_presTimer)   /* pre-1803 fallback */
            g_presTimer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
        /*
         * Load the cylinder. Every hot-path constant is now DETECTED, and the
         * negotiated media type feeds it, so the pacing period comes from the
         * source rate Media Foundation actually agreed to rather than a literal
         * typed into this file.
         */
        g_nextPres = qpc_now() + g_presPeriodMs / 1000.0;
        if (!g_tear && g_verbose)
            fprintf(stderr, "presentation clock: VSync (compositor-paced); "
                            "source %.3f Hz on panel %.3f Hz -- surplus frames "
                            "will be DROPPED, never held\n",
                    wubu_rev_get(g_rev, WUBU_CH_SOURCE_HZ, NULL),
                    wubu_rev_get(g_rev, WUBU_CH_PANEL_HZ, NULL));
        g_telem = wubu_telem_create(0);
        g_telemT0 = qpc_now();
        if (g_csvEvery > 0) {
            HANDLE w = CreateThread(NULL, 0, telemetry_writer, NULL, 0, NULL);
            if (w) CloseHandle(w);
        }

            /*
             * Repeat or drop? Compare the panel period against the source period.
             * Source is negotiated at exactly 60 fps (16.667 ms).
             *   panel faster than source -> repeat each frame to fill every slot
             *   panel slower than source -> we must drop, or presents queue unbounded
             */
            {
                const double srcPeriod = 1000.0 / 60.0;
                /* With no known period (0.0, meaning the panel read failed) we
                   must not compare: everything is "faster than source". Repeating
                   every frame is the safe direction, and the tearing path is
                   already forced in that case. */
                g_repeat = (g_presPeriodMs <= 0.0 || g_presPeriodMs < srcPeriod) ? 1 : 0;
            }
            if (g_verbose) {
                if (g_presPeriodMs > 0.0)
                    fprintf(stderr, "pacing to %.4f ms/frame (%.3f Hz)  src 60.000 Hz  repeat=%d\n",
                            g_presPeriodMs, 1000.0 / g_presPeriodMs, g_repeat);
                else
                    fprintf(stderr, "pacing: PERIOD UNKNOWN (panel read failed); "
                                    "tearing path, repeat=%d\n", g_repeat);
            }

    WNDCLASSEX wc = {sizeof(wc)};
    wc.style = CS_HREDRAW|CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.lpszClassName = TEXT("WBC2");
    RegisterClassEx(&wc);

    /*
     * The display-topology module is created BEFORE the window exists, because
     * it has no window dependency -- the reverse was the original defect: panel
     * detection needed an HWND, which did not exist yet, so it silently fell
     * back to a neighbour monitor.
     */
    g_panel = wubu_panel_create();
    if (g_panel) wubu_panel_set_verbose(g_panel, g_verbose);

    /*
     * PLACE THE WINDOW ON THE DISPLAY THE USER IS LOOKING AT, not at a
     * hardcoded negative offset. The old coordinates (-1440,-512) put the
     * preview on a portrait display to the left of the primary, which is how
     * the app ended up pacing against 120 Hz while the user watched a 59 Hz
     * panel. Startup placement is now decided by the same rule as F-time
     * placement, so both paths agree.
     */
    {
        POINT ctr = { 0, 0 };
        GetCursorPos(&ctr);
        HMONITOR mon = MonitorFromPoint(ctr, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = { sizeof(mi) };
        int wx = 100, wy = 100;
        if (GetMonitorInfo(mon, &mi)) {
            int mw = mi.rcMonitor.right - mi.rcMonitor.left;
            int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
            int ww = (mw > 1400) ? 1280 : mw;
            int wh = (mh > 820)  ? 720  : mh;
            wx = mi.rcMonitor.left + (mw - ww) / 2;
            wy = mi.rcMonitor.top  + (mh - wh) / 2;
        }
        g_hwnd = CreateWindowEx(0, TEXT("WBC2"), TEXT("WuBuCapture - F fs, ESC quit"),
            WS_OVERLAPPEDWINDOW|WS_VISIBLE, wx, wy, 1280, 720,
            0, 0, hi, 0);
    }

    CoInitializeEx(0, COINIT_APARTMENTTHREADED);
    EnumDevices();

    /* Find default device (2nd USB3 Video = PS5) */
    int defaultDev = -1;
    int usbIdx = 0;
    for (UINT32 i = 0; i < g_devCount; i++) {
        if (g_devNames[i] && wcsstr(g_devNames[i], L"USB3 Video")) {
            if (usbIdx++ == 1) { defaultDev = i; break; }
        }
    }
    if (defaultDev < 0) defaultDev = (g_devCount > 0) ? 0 : -1;
    if (wantDev >= 0 && wantDev < (int)g_devCount) defaultDev = wantDev;

    if (FAILED(InitD3D11())) { fprintf(stderr,"D3D11\n"); return 1; }
    if (defaultDev >= 0)
        InitMF(defaultDev);
    else {
        fprintf(stderr,"No capture device found\n");
        return 1;
    }

    /*
     * THREAD PRIORITY — THIS MATTERS FOR THE WHOLE MACHINE.
     *
     * We previously used THREAD_PRIORITY_TIME_CRITICAL (base priority 15).
     * Microsoft documents that any thread above base priority 11 "interferes
     * with the normal operation of the operating system". OBS and its NVENC
     * feed run at the default base priority 8, so a time-critical capture
     * thread literally PREEMPTS the encoder and produces "encoding overloaded"
     * plus random dropped frames in the stream.
     *
     * ABOVE_NORMAL (10) is the highest level that still yields to OBS.
     * Latency cost vs time-critical is unmeasurable at 60fps because the
     * thread is blocked in ReadSample ~98% of the time anyway.
     */
    HANDLE hCap = CreateThread(0, 0, CaptureThread, 0, 0, 0);
    SetThreadPriority(hCap, THREAD_PRIORITY_ABOVE_NORMAL);
    /* The main loop also does the memcpy upload — keep it out of OBS's way. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    while (g_running) {
            /*
             * Pace to the panel clock instead of to frame arrival. We wake on the
             * next present deadline, then present the NEWEST captured frame if one
             * arrived since the last tick. If the capture thread has not produced
             * anything new we deliberately skip the present rather than re-show a
             * stale frame — a duplicate held for a full refresh is exactly the
             * "frozen for one frame" judder we are eliminating.
             */
            enter_mmcss();

        /* Loaded lazily on the first loop iteration instead: the panel chamber
               needs a real HWND, and this init block runs BEFORE CreateWindowEx. */
            if (!g_revLoaded && g_hwnd) {
                g_revLoaded = 1;
                wubu_rev_verbose = g_verbose;
            g_rev = wubu_rev_create();
                wubu_rev_load(g_rev, g_hwnd,
                              g_negotiatedFps > 1.0 ? g_negotiatedFps : 0.0,
                              g_negotiatedW, g_negotiatedH);

                {
                /*
                 * The PANEL TRUTH comes from wubu_panel now, and it is
                 * re-resolved against the window's real position. The revolver's
                 * own panel_hz chamber still exists for the pacing math, but the
                 * value that decides the Present() sync interval is the one we
                 * just measured ourselves -- and if the two disagree, the panel
                 * module wins and says so, because a second reader is exactly
                 * how this bug got misdiagnosed twice.
                 */
                relock_target_panel(FALSE);

                double src_ms = wubu_rev_get(g_rev, WUBU_CH_PACING_MS, NULL);
                double pan_ms = wubu_rev_get(g_rev, WUBU_CH_PANEL_PERIOD_MS, NULL);
                /* The MEASURED-source re-lock lives in the main loop, not here.
                   This block is one-shot init (guarded by !g_revLoaded) and runs
                   before a single capture frame has arrived, so g_capIvN is 0 and
                   any measurement taken here would be meaningless. v20 logged its
                   lock after warmup, which is why it worked there. */
                if (g_measuredSrcMs > 0.0) src_ms = g_measuredSrcMs;
                if (g_targetHz > 0.0) {
                    double revHz = wubu_rev_get(g_rev, WUBU_CH_PANEL_HZ, NULL);
                    if (fabs(revHz - g_targetHz) > 0.5)
                        fprintf(stderr, "NOTE: revolver panel_hz %.0f disagrees with the "
                                        "measured %0.f Hz; using the measured value.\n",
                                revHz, g_targetHz);
                    pan_ms = 1000.0 / g_targetHz;
                }
                /*
                 * THE TRUE REFRESH RATE. g_targetHz is derived from
                 * EnumDisplaySettings' INTEGER dmDisplayFrequency, which reports
                 * 59 for this panel. The panel actually runs 59936/1000 =
                 * 59.936 Hz (measured; see src/probe_qdc.c). Pacing to 59 means
                 * a 16.9492 ms period against a real 16.6845 ms one: 0.2658 ms
                 * of error per slot, which accumulates into a fully missed
                 * blank every ~63 slots, i.e. a hitch about once per second,
                 * forever. The fractional DEVMODE field is unreadable on this
                 * toolchain (proven by src/devfreq_diag.c: dmSize clipped to
                 * 188, poison bytes past the struct untouched), so the rate has
                 * to come from QueryDisplayConfig's exact DISPLAYCONFIG_RATIONAL.
                 *
                 * If the read fails we KEEP the integer value and say so out
                 * loud. Substituting a remembered 59.94 or 16.6833 would be
                 * inventing a measurement, which is how this got wrong twice.
                 */
                unsigned qn = 0, qd = 0;
                double trueHz = wubu_qdc_refresh_hz(g_hwnd, &qn, &qd);
                if (trueHz > 0.0) {
                    g_panelHzTrue = trueHz;
                    pan_ms = 1000.0 / trueHz;   /* measured, e.g. 16.6845 ms */
                } else {
                    /*
                     * FAIL SAFE, DO NOT PACE. Previously this fell back to the
                     * truncated integer field, which is precisely the defect that
                     * caused the hitches: 59 Hz is not a rate this panel runs at,
                     * and pacing to it accumulates ~0.26 ms of error per slot into
                     * a missed blank about once a second, forever.
                     *
                     * A wrong clock is worse than no clock. With no grid the
                     * compositor's vsync IS the clock -- it cannot drift from the
                     * panel, because it IS the panel. So the honest unknown here
                     * degrades to "let Windows pace", not to "guess a number".
                     *
                     * Refusing to invent a rate is the whole point of this module.
                     */
                    g_panelHzTrue = 0.0;
                    pan_ms = 0.0;
                    fprintf(stderr,
                            "WARNING: true refresh rate unresolved (QueryDisplayConfig "
                            "failed). NOT pacing to the panel -- letting the compositor\n"
                            "         own vsync. The integer field says %0.f Hz, which is\n"
                            "         truncated and would drift ~0.26 ms per slot.\n",
                            g_targetHz);
                    g_paceToPanel = 0;
                }

                /*
                 * SYNC INTERVAL MUST COME FROM THE SAME RATE AS pan_ms.
                 *
                 * This was a real bug, measured: g_syncInterval was computed in
                 * relock_target_panel() from the TRUNCATED integer rate, while
                 * pan_ms here comes from the TRUE rational. On a 120 Hz portrait
                 * panel that gave
                 *
                 *     pan_ms  = 1000/59.936 = 16.6845   (a 60 Hz panel!)
                 *     iv      = 120/60       = 2
                 *     grid    = 16.6845 * 2  = 33.3689 ms
                 *
                 * i.e. two DIFFERENT displays' numbers multiplied together, and
                 * the app ran at half rate forever: measured mean 33.32 ms across
                 * 11 consecutive trials, 1202 samples each. That is the
                 * regression versus v20, which paces to the measured SOURCE.
                 *
                 * Deriving both from one rate makes that class of error
                 * impossible: if the interval came from the 59.936 panel it is 1,
                 * and 16.6845 * 1 is a real period.
                 */
                if (g_paceToPanel && g_panelHzTrue > 0.0) {
                    double src = (g_negotiatedFps > 1.0) ? g_negotiatedFps : 60.0;
                    g_syncInterval = wubu_panel_sync_interval(src, g_panelHzTrue);
                }
                if (g_verbose)
                    fprintf(stderr, "panel true refresh: %s  [integer field said %u Hz]\n",
                            g_panelHzTrue > 0.0 ? "resolved" : "UNRESOLVED",
                            (unsigned)g_targetHz);
                if (g_verbose && g_panelHzTrue > 0.0)
                    fprintf(stderr, "  rational %u/%u = %.4f Hz = %.4f ms\n",
                            qn, qd, g_panelHzTrue, pan_ms);

                /*
                 * ── WHICH CLOCK DRIVES US ───────────────────────────────────
                 *
                 * The v43 experiment removed the self-pacer entirely and looked
                 * worse (77 dips/min). Re-running the SAME config gave 40.3, so
                 * that number was an outlier, and the theory written here at the
                 * time ("two clocks fight") was never actually supported.
                 *
                 * What the clean trials DO show, 6 x 40 s each, noise band 1.5:
                 *   grid on the TRUE panel period : 1.5-3.0 dips/min, sd ~0.72
                 *   no grid at all               : cannot be told apart from the
                 *                                      above, within noise
                 *
                 * So the self-pacer is not the fix either -- using the TRUE rate
                 * is (see above; 37 -> 1.5 dips/min). We keep --pace panel as the
                 * default because it is the measured-best configuration and it
                 * phase-locks submission to the blank, which is what lets the
                 * compositor interleave cleanly. But do not reintroduce the old
                 * claim that removing it is catastrophic: that was one bad run.
                 *
                 * Only the TEARING path truly needs a self-imposed clock, because
                 * nothing else imposes one there.
                 */
                if (g_tear) {
                    /* Nothing else imposes a frame clock here, so we must.
                       Use the SOURCE period: we are free-running, and pacing to a
                       panel rate would be pacing a free-run to a display. */
                    g_presPeriodMs = src_ms;
                } else if (g_paceToPanel) {
                    /*
                     * SELF-PACED: PERIOD MUST BE FASTER THAN THE PANEL, NOT SLOWER.
                     *
                     * I had this backwards at first and the +1%-slow experiment (v56)
                     * was built on the error. Getting the sign right:
                     *
                     *   grid FASTER than panel (v20: 16.5970, v59: 16.6438 vs
                     *   16.6845) -> we reach the wait a little BEFORE the blank, and
                     *   Present(g_swap, 1, 0) blocks the small remainder. The frame
                     *   is handed over exactly at the blank. Self-correcting.
                     *
                     *   grid SLOWER than panel (v54: 16.6845 = exactly equal;
                     *   v56: 16.8513 = +1%) -> we reach the wait AFTER the blank has
                     *   already gone by, so the frame misses it entirely and waits a
                     *   FULL extra refresh. This is the 33-50 ms dip. Worse still,
                     *   the gap only grows as the drift accumulates.
                     *
                     * So the bias must be < 1, and the "+1% slow" comment that used
                     * to sit here was exactly backwards. The measured-source re-lock
                     * is what actually supplies the correct sub-panel period, since
                     * a real capture device's delivered interval is naturally a hair
                     * under the panel's.
                     *
                     * Note the pacer can still wake early on its own (v54 posted a
                     * 16.195 mean against a 16.6845 request), which is the other half
                     * of why self-pacing is A/B-only and DXGI owns vsync by default.
                     */
                    /* Period is used as measured; see the re-lock below. If the
                       source measurement is unavailable, fall back to the panel
                       period rather than inventing a margin. */
                    g_presPeriodMs = pan_ms * (double)(g_syncInterval ? g_syncInterval : 1);
                } else if (g_syncInterval == 0 && g_measuredSrcMs > 0.0) {
                    /*
                     * NO EVEN PANEL CADENCE -> THE SOURCE OWNS THE CLOCK.
                     *
                     * 60.000 fps on the 99.982 Hz primary is 1.667 panels per frame,
                     * so wubu_panel_sync_interval() correctly returns 0 and DXGI has
                     * no even cadence to give us. The source interval is the only
                     * clock that can pace this correctly: wait 16.667 ms, submit once,
                     * and let DWM hold or drop the surplus panel slots.
                     *
                     * This branch used to be an unconditional
                     * `else { g_presPeriodMs = 0.0; }`, which ran EVERY iteration and
                     * wiped the grid the measured-source re-lock had just armed --
                     * so the arming had no effect and the presents ran unthrottled
                     * (measured interval collapsed to 0.000 ms, 2.6M presents/sec).
                     * That is why the source-owned clock appeared to do nothing.
                     */
                    g_presPeriodMs = g_measuredSrcMs;
                } else {
                    /* DXGI owns vsync on an evenly-divisible pairing: no grid. */
                    g_presPeriodMs = 0.0;
                }

                if (g_verbose)
                    fprintf(stderr, "pace authority: %s  %.4f ms  (sync interval %u, "
                                    "panel true %.4f Hz)\n",
                            g_tear ? "SOURCE (tearing, self-paced)"
                                   : (g_paceToPanel ? "PANEL (self-paced to true rate)"
                                                     : "COMPOSITOR (Present blocks to vblank)"),
                            g_presPeriodMs, g_syncInterval, g_panelHzTrue);
            }
                g_revSpinTailMs  = wubu_rev_get(g_rev, WUBU_CH_SPIN_TAIL_MS, NULL);
                g_warmupTarget   = wubu_rev_get_u(g_rev, WUBU_CH_WARMUP_FRAMES);
                g_dipThresholdMs = wubu_rev_get(g_rev, WUBU_CH_LAT_WARN_MS, NULL);
                if (g_csvEvery <= 0)
                    g_csvEvery = (long)wubu_rev_get(g_rev, WUBU_CH_EXPORT_SEC, NULL);

                if (g_verbose) {
                    char dump[1024];
                    wubu_rev_dump(g_rev, dump, sizeof dump);
                    fprintf(stderr, "revolver chambers:\n%s", dump);
                }
            }


            /*
             * WARM-UP GATE.
             *
             * Hold off presenting until the pipeline has actually produced
             * g_warmupTarget CAPTURED FRAMES, then arm telemetry and fall
             * through and present normally.
             *
             * Two bugs lived here and both showed up as dips at t=1.4-10 s:
             *  - the condition was INVERTED. It gated only once capFrames had
             *    already reached the target, which meant we skipped presenting
             *    during init and then began recording exactly as the transient
             *    was still settling.
             *  - it counted loop ITERATIONS rather than frames. With no Present
             *    during warm-up the loop spins fast, so 90 iterations took
             *    ~0.1 s instead of the intended ~1.5 s.
             */
            if (!g_warmupDone) {
                if ((LONG)g_capFrames < (LONG)g_warmupTarget) {
                    continue;                 /* still settling: do not present */
                }
                g_lastPresT = 0.0;            /* drop the startup gap           */
                g_lastAge  = 0.0;
                wubu_telem_clear(g_telem);    /* ring starts at steady state    */
                g_warmupDone = 1;
                g_telemT0 = qpc_now();
                g_csvNext = g_telemT0 + (g_csvEvery > 0 ? g_csvEvery : 1e9);
                if (g_verbose) fprintf(stderr, "warmup done after %ld frames, "
                                               "telemetry armed\n", (long)g_capFrames);
            }

            double nowT = qpc_now();
                        /* Telemetry is recorded at the PRESENT site (see "ONE TELEMETRY ROW PER
                                       PRESENT" below), where do_present is known and the clock is fresh. */

            /* Periodic CSV export for offline analysis (graphs, percentiles). */
            if (g_csvEvery > 0 && nowT >= g_csvNext) {
                g_csvNext = nowT + g_csvEvery;
                wubu_telem_export_csv(g_telem, "wubu_telemetry.csv");
            }

            /*
             * Our QPC pacer exists ONLY for the tearing path, where nothing else
             * imposes a frame clock. Under vsync, Present(1) already blocks until
             * the panel refresh; pacing on top of that would double-throttle the
             * loop to 30 fps (two 16.67 ms periods per 33.3 ms refresh) and
             * manufacture its own judder. So the compositor owns the clock.
             */
            /*
             * WHO OWNS THE CLOCK. Exactly one, always.
             *
             * If g_presPeriodMs > 0 then OUR GRID owns it and we must actually
             * wait on it, because Present is called with SyncInterval 0 in that
             * mode (see the double-block note at the Present call). Skipping the
             * wait while presenting unthrottled was measured at mean 0.283 ms
             * -- a 3500 fps busy loop that presents the same buffer thousands of
             * times per frame period.
             *
             * If g_presPeriodMs == 0 then DXGI owns it: we present with the
             * interval and Present blocks to the blank, so no waiting is needed
             * (and waiting too would double-throttle, which is what the older
             * comment here was warning about -- correctly, for the case where the
             * grid was set but NOT enforced).
             */
            /*
             * THE GRID MUST PACE PRESENTS, NOT LOOP ITERATIONS.
             *
             * This loop is a free-running panel-slot loop: it iterates once per vsync,
             * so on the 99.982 Hz primary it comes round ~100 times a second. But the
             * grid period is the SOURCE interval, 16.667 ms. Waiting at the top of the
             * loop therefore sleeps on whichever iterations happen to fall on the
             * grid, and the iterations in between run free -- so the present interval
             * came out at 1.190 ms mean instead of 16.667, with the work spread across
             * ~14 skipped slots per present.
             *
             * When we own the clock the wait belongs immediately before the Present,
             * not at the top of the iteration: that is what makes the cadence a
             * property of the frames we actually show. So the top-of-loop wait is
             * skipped in that case, and wait_until_pres() is called at the present
             * site instead (see the "GRID Paces the Present" note there).
             *
             * When DXGI owns vsync there is no grid and no wait at all. And when
             * the SOURCE owns the clock (the 60 fps on 100 Hz case) the wait has
             * already been done at the present site. So the top-of-loop wait
             * applies only to the TEARING path, which has no present site wait.
             */
            if (g_tear && g_presPeriodMs > 0.0) wait_until_pres();

            /*
             * RE-LOCK THE SELF-PACER TO THE MEASURED SOURCE (restored from v20).
             *
             * This is the single most important difference between the clean v20
             * and every build after it. v20 logged, at runtime:
             *
             *     pacing locked to measured SOURCE 16.5970 ms (60.252 Hz)
             *
             * It measured the interval frames ACTUALLY arrive at and retuned its
             * grid to that, instead of trusting the nominal 60.000 forever. The
             * measured value is ~0.5% SLOWER than the panel's 16.6845 ms, so the
             * grid lands LATE relative to the blank, and a late frame only waits a
             * little for the next blank -- it can never MISS one. That is why v20
             * holds 0.0 dips/min with an 18.20 ms worst gap.
             *
             * The nominal 60.000 is a request, not a measurement: the capture
             * device has its own clock and MF adds jitter on top.
             *
             * Gating: only after a real warmup, only ONCE (g_periodLocked), and
             * only if within 10% of nominal -- a wildly different value means the
             * source is not running, and pacing to a nonsense interval is worse
             * than pacing to the nominal one.
             */
            /*
             * Gate on a warmup that the loop can actually REACH.
             *
             * This was `g_capIvN > 120` and it never fired: the present-gated loop
             * iterates far faster than the capture thread delivers frames, so
             * g_capIvN crawled 90 -> 101 while the loop spun ~400 times, and
             * g_measuredSrcMs stayed 0.0 forever. Nothing downstream could work.
             *
             * 32 frames is ~0.53 s at 60 fps: long enough for the p50 of the ring
             * to be representative (LAT_N is 512, so 32 samples is well inside it)
             * and short enough to arm promptly.
             */
            if (!g_periodLocked && g_capIvN >= 32) {
                LONG cn = g_capIvN; if (cn > LAT_N) cn = LAT_N;
                double meas = wubu_stat_percentile(g_capIv, cn, 0.50);
                double nominal = 1000.0 / ((g_negotiatedFps > 1.0) ? g_negotiatedFps : 60.0);
                if (meas > 0.5 && fabs(meas - nominal) / nominal < 0.10) {
                    g_measuredSrcMs = meas;
                    /* Only the SOURCE-paced path uses this; a panel-paced grid is
                       pinned to the panel and must not be retimed off the source. */
                    if (g_tear && g_presPeriodMs > 0.0) g_presPeriodMs = meas;
                    /*
                     * Also arm the grid here when NO EVEN PANEL CADENCE EXISTS.
                     * 60.000 fps on the 99.982 Hz primary is 1.667 panels per frame,
                     * so there is no integer sync interval and DXGI cannot be
                     * trusted to space the presents. The source must own the clock
                     * in that case (see the "SOURCE MUST OWN THE CLOCK" note at the
                     * present site). Arming it HERE -- after warmup, when the
                     * measurement exists, and before this iteration's own wait --
                     * is the only point where the deadline can be laid down
                     * correctly.
                     */
                    if (!g_tear && g_syncInterval == 0 && g_presPeriodMs <= 0.0) {
                        g_presPeriodMs = meas;
                        g_nextPres = qpc_now() + g_presPeriodMs / 1000.0;
                        if (g_verbose)
                            fprintf(stderr, "no even panel cadence (%.3f Hz panel / "
                                            "%.3f Hz source); SOURCE owns the clock at "
                                            "%.4f ms, Present(0), surplus slots to DWM\n",
                                    g_targetHz, g_negotiatedFps, g_presPeriodMs);
                    }
                    g_periodLocked = 1;
                    if (g_verbose)
                        fprintf(stderr, "pacing locked to measured SOURCE "
                                        "%.4f ms (%.3f Hz)\n", meas, 1000.0 / meas);
                } else if (g_verbose) {
                    fprintf(stderr, "measured source %.4f ms unusable "
                                    "(nominal %.4f); keeping nominal pacing\n",
                            meas, nominal);
                }
            }

            /*
             * PER-ITERATION CLOCK AUTHORITY.
             *
             * The block in the init path decides this once, before any capture
             * frame has arrived, so g_measuredSrcMs is still 0.0 there and the
             * source-owned branch could never fire -- which is exactly why
             * `per=0.0000` at the present site no matter what the init block
             * decided. The decision that matters has to be re-made here, every
             * iteration, now that the measurement exists.
             *
             * Precedence, and only one clock may ever be active:
             *   1. tearing                -> SOURCE period (nothing else paces it)
             *   2. even panel cadence     -> nothing; DXGI owns vsync
             *   3. no even panel cadence  -> SOURCE period, measured
             */
            if (g_tear) {
                g_presPeriodMs = g_measuredSrcMs > 0.0
                    ? g_measuredSrcMs
                    : 1000.0 / ((g_negotiatedFps > 1.0) ? g_negotiatedFps : 60.0);
            } else if (g_syncInterval != 0) {
                g_presPeriodMs = 0.0;          /* DXGI paces */
            } else if (g_measuredSrcMs > 0.0) {
                /*
                 * 60.000 fps on the 99.982 Hz primary is 1.667 panels per frame,
                 * so there is no even cadence and DXGI cannot space this for us.
                 * The measured source interval is the only correct clock: wait
                 * 16.667 ms, submit once, let DWM hold or drop the surplus slots.
                 */
                g_presPeriodMs = g_measuredSrcMs;
            } else {
                g_presPeriodMs = 0.0;          /* not yet known: let Windows pace */
            }

            /*
             * RE-LOCK IF THE WINDOW CHANGED MONITOR.
             *
             * F (fullscreen) re-locks, but a plain window DRAG does not -- and the
             * window can end up on a different display than the one we locked at
             * startup. Measured: the app locked DISPLAY24 (1440x2560, 120 Hz, 2:1
             * EVEN) at launch, then the window sat at (1080,360)-(2360,1080) which
             * is DISPLAY22 (99.982 Hz). So it was pacing to 120 Hz while displaying
             * on a 99.982 Hz panel: the wrong rate, on the wrong display, with no
             * warning. It still read 60 fps, which is exactly why this is easy to
             * miss -- the cadence looks plausible either way.
             *
             * Cost is one MonitorFromWindow per iteration, which is a cheap call,
             * and the lock only fires on an actual change. Compare the HMONITOR
             * rather than the rate: two displays can share a rate, and the rate is
             * not what decides where the pixels go.
             */
            if (g_hwnd) {
                HMONITOR monNow = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
                if (monNow && monNow != g_lockedMon) {
                    if (g_verbose)
                        fprintf(stderr, "window moved to a different monitor; "
                                        "re-locking cadence to it\n");
                    relock_target_panel(FALSE);
                    g_lockedMon = monNow;
                }
            }

            /* Pump any window messages that piled up while we slept. */
            MSG msg;
            while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            if (g_needResize) ResizeSwapChain(g_winW, g_winH);

            LONG ri = g_rdIdx;
                    static LONG lastRi = -1;
                    int haveNew = (ri != lastRi);
                    if (haveNew) {
                        lastRi = ri;
                        double age = (qpc_now() - g_bufT[ri]) * 1000.0;
                        /* Guard the very first frames: g_bufT can still be 0
                           there, which yields an absurd 7e8 ms and poisons the
                           percentiles. */
                        g_lastAge = (age > 0.0 && age < 1000.0) ? age : 0.0;
                        LONG k = (InterlockedIncrement(&g_latN) - 1) & (LAT_N - 1);
                        g_lat[k] = age;
                        RenderFrame(g_bufs[ri]);
                        InterlockedIncrement(&g_renFrames);
                    } else if (!g_repeat) {
                        InterlockedIncrement(&g_dupRenders);
                    }
                    /*
                     * Present on EVERY panel slot. When the source is slower than the
                     * panel we deliberately re-present the same texture: an even 10ms
                     * cadence reads as smooth, whereas skipping slots reads as judder.
                     * Re-presenting costs no upload — the texture is already on the GPU.
                     */
                    /*
                     * PRESENT ONCE PER SOURCE FRAME -- LIKE A GAME.
                     *
                     * The loop below is a free-running PANEL-SLOT loop: one iteration
                     * per vsync. Presenting unconditionally therefore presents at the
                     * PANEL rate, not the source rate. On the 99.982 Hz primary that
                     * produced a title reading "99.9 fps" for a 60.000 fps source --
                     * the same frame re-presented ~40% too often, which is exactly the
                     * "not a proper 60fps feed like a game" symptom.
                     *
                     * A game does not do this. At 60 fps content on a 100 Hz panel a
                     * game presents 60 times a second and lets the compositor hold or
                     * discard the surplus slots. So: only present on a slot where a NEW
                     * source frame actually arrived. Slots with no new data are
                     * skipped and DWM holds the last frame, which is the correct
                     * visual and costs no upload.
                     *
                     * The condition below is deliberately `!haveNew` only in vsync
                     * mode. The TEARING path must keep presenting every slot, because
                     * with tearing there is no compositor holding the frame for us and
                     * skipping would visibly stall the output.
                     */
                    {
                        double tNow = qpc_now();
                        /* Time from returning out of the previous Present to
                           entering this one: telemetry, message pump, resize
                           check, the GPU upload, the draw and all state. This
                           is the number that has to stay under one refresh
                           period, because Present(1) then blocks until the
                           blank. */
                        if (lastPres > 0.0) {
                            LONG w = (InterlockedIncrement(&g_workN) - 1) & (LAT_N - 1);
                            g_work[w] = (tNow - lastPres) * 1000.0;
                        }
                        /*
                         * ── THE CADENCE DECISION ─────────────────────────────────
                         * The sync interval is no longer a hardcoded 1. It is the
                         * integer number of panel refreshes that ONE source frame
                         * should occupy, computed from the panel we are actually
                         * on (wubu_panel_sync_interval) and re-locked at F-time.
                         *
                         * 60.000 fps on the 120 Hz portrait panel -> interval 2.
                         * That is the whole fix for the judder the user kept
                         * seeing: every source frame is shown for exactly two
                         * refreshes, forever, with no repeat/skip and no phase
                         * drift to accumulate.
                         *
                         * WHY v34's "just use vsync" WAS WORSE (246 dips): it used
                         * interval 1 on a 120 Hz panel. Interval 1 makes the
                         * compositor hold our 60 fps frame for 1 refresh and then
                         * present the NEXT source frame, which lands mid-refresh
                         * and has to be held again -- the compositor repeats
                         * arbitrarily, and an unpaced producer on top of that
                         * double-flushes the queue. The interval has to match the
                         * 2:1 content-to-refresh relationship or the cadence is
                         * wrong by construction. It was never "vsync's fault".
                         *
                         * g_syncInterval == 0 means no even integer cadence exists
                         * (e.g. 60 fps on a 100 Hz panel). We then deliberately
                         * fall back to 1, which is the only honest choice, and we
                         * have already told the user on stderr that the repeats
                         * will alternate and the judder is a property of the
                         * pairing rather than a bug here.
                         */
                        unsigned iv = g_syncInterval ? g_syncInterval : 1;
                        /*
                         * PRESENT MUST NOT DOUBLE-BLOCK.
                         *
                         * Measured: with the grid correctly set to 16.6669 ms
                         * (60 fps source on the true 119.998 Hz panel, interval 2),
                         * the observed present interval was 33.32-33.33 ms on every
                         * trial -- 2x.
                         *
                         * Arithmetic: wait_until_pres() already sleeps to our
                         * 16.67 ms deadline, then Present(g_swap, 2, 0) blocks for
                         * ANOTHER two refreshes (16.67 ms), because SyncInterval is
                         * a floor on vertical blanks, not a schedule. Two clocks
                         * each doing the same job = half rate. That is why v52 had
                         * the right number and still ran at 30 fps.
                         *
                         * Fix: when our grid owns the clock, present with
                         * SyncInterval 0 and let the grid space the frames. With no
                         * grid, hand the job to DXGI. Exactly one clock is active.
                         *
                         * SyncInterval 0 does NOT mean tearing. Tearing needs the
                         * separate DXGI_PRESENT_ALLOW_TEARING flag, which we only
                         * pass on the explicit --tear path. Without it a windowed
                         * flip-model swapchain still composites via DWM.
                         */
                        BOOL we_own_clock = (g_presPeriodMs > 0.0);

                        /*
                         * GATE THE PRESENT ON A NEW SOURCE FRAME (vsync mode only).
                         *
                         * See the "PRESENT ONCE PER SOURCE FRAME" note above. Without
                         * this gate the loop presents once per PANEL SLOT, so a 60.000
                         * fps source on the 99.982 Hz primary is presented ~100 times a
                         * second and the title reads 99.9 fps. Gating on haveNew makes
                         * the present rate equal the source rate and hands the surplus
                         * slots to DWM, exactly as a game does.
                         *
                         * DXGI_ERROR_WAS_STILL_DRAWING must NOT skip the telemetry
                         * accounting below, so this only chooses whether to call
                         * Present at all; it is not an early return.
                         */
                        BOOL do_present = g_tear || haveNew;

                        /*
                         * GRID PACES THE PRESENT (source-owned clock).
                         *
                         * When we own the clock the wait happens HERE, immediately
                         * before the submit, not at the top of the loop -- see "THE
                         * GRID MUST PACE PRESENTS, NOT LOOP ITERATIONS". That makes
                         * the cadence a property of presented frames: one wait, one
                         * submit, one source frame.
                         *
                         * Must be paired with haveNew: waiting on a slot we are about
                         * to skip would consume the deadline without a submit, and
                         * the next present would then find an already-expired
                         * deadline and fire immediately. That is exactly the
                         * 1.190 ms mean this fixes.
                         */
                        if (we_own_clock && !g_tear && do_present)
                            wait_until_pres();

                        HRESULT pr = S_OK;
                        if (do_present)
                            pr = g_tear
                                ? IDXGISwapChain_Present(g_swap, 0, DXGI_PRESENT_ALLOW_TEARING)
                                : IDXGISwapChain_Present(g_swap, we_own_clock ? 0 : (UINT)iv, 0);
                        if (pr != DXGI_ERROR_WAS_STILL_DRAWING && pr != S_OK)
                g_bbIndex = (g_bbIndex + 1) % (g_bbCount ? g_bbCount : 1);

                        /*
                         * Record the interval ONLY when we actually presented.
                         * Recording on skipped slots would mix 10 ms panel-slot
                         * iterations into the present-interval statistics and make
                         * the whole timing suite meaningless -- the metrics must
                         * describe presents, because presents are what the panel
                         * shows.
                         */
                        if (do_present) {
                                                    /*
                                                     * ONE TELEMETRY ROW PER PRESENT, NOT PER LOOP ITERATION.
                                                     *
                                                     * Recording here rather than at the top of the loop is
                                                     * required now: the loop is present-gated and spins past
                                                     * slots with no new frame, so a per-iteration record filled
                                                     * the 16384-row ring in ~5 ms and every t_ms came out equal
                                                     * (60.000) -- no time axis, and the timing suite reported
                                                     * nothing. Both the row and the interval must describe
                                                     * presents, because presents are what the panel shows.
                                                     */
                                                    LONG m = (InterlockedIncrement(&g_ivN) - 1) & (LAT_N - 1);
                                                    double tEnd = qpc_now();
                                                    if (lastPres > 0) g_iv[m] = (tEnd - lastPres) * 1000.0;
                                                    lastPres = tEnd;

                                                    if (g_warmupDone)
                                                        wubu_telem_record(g_telem,
                                                            (g_telemT0 > 0.0 ? (tEnd - g_telemT0) * 1000.0 : 0.0),
                                                            lastCapT > 0.0 && g_capIvN > 0
                                                                ? g_capIv[(g_capIvN - 1) & (LAT_N - 1)] : 0.0,
                                                            g_lastPresT > 0.0 ? (tEnd - g_lastPresT) * 1000.0 : 0.0,
                                                            g_lastAge, 1);
                                                    g_lastPresT = tEnd;
                                                }
                    }
                    TickTitle();
                }

    g_running = FALSE;
    WaitForSingleObject(hCap, 2000);
    if (g_reader) IMFSourceReader_Release(g_reader);
    MFShutdown();
    free(g_bufs[0]); free(g_bufs[1]); free(g_bufs[2]);
    CloseHandle(g_frameEvent);
    CoUninitialize();
    return 0;
}
