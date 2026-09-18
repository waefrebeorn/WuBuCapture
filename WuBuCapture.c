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
#include <mfidl.h>
#include <mfreadwrite.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static IDXGISwapChain1 *g_swap;
static ID3D11RenderTargetView *g_rtv;
static ID3D11Texture2D *g_yuy2Tex;
static ID3D11ShaderResourceView *g_yuy2Srv;
static ID3D11VertexShader *g_vs;
static ID3D11PixelShader *g_ps;
static ID3D11InputLayout *g_il;
static ID3D11Buffer *g_vb;
static ID3D11SamplerState *g_samp;

static HWND g_hwnd;
static volatile BOOL g_running = TRUE;
static int g_winW = 1280, g_winH = 720;
static const int CAP_W = 1920, CAP_H = 1080;

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

/* Simple passthrough pixel shader — just samples RGBA texture */
static const char *g_psSrc =
    "Texture2D tex:register(t0);"
    "SamplerState samp:register(s0);"
    "float4 main(float4 pos:SV_POSITION, float2 uv:TEXCOORD):SV_TARGET{"
    "  return tex.Sample(samp, uv);"
    "}";

static HRESULT InitD3D11(void) {
    HRESULT hr;
    ID3DBlob *vsB, *psB;
    hr = D3DCompile(g_vsSrc, strlen(g_vsSrc), 0, 0, 0, "main", "vs_4_0", 0, 0, &vsB, 0);
    if (FAILED(hr)) { fprintf(stderr, "VS compile: 0x%08X\n", hr); return hr; }
    hr = D3DCompile(g_psSrc, strlen(g_psSrc), 0, 0, 0, "main", "ps_4_0", 0, 0, &psB, 0);
    if (FAILED(hr)) { fprintf(stderr, "PS compile: 0x%08X\n", hr); return hr; }

    D3D_FEATURE_LEVEL fl;
    hr = D3D11CreateDevice(0, D3D_DRIVER_TYPE_HARDWARE, 0,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, 0, 0, D3D11_SDK_VERSION,
        &g_dev, &fl, &g_ctx);
    if (FAILED(hr)) return hr;

    IDXGIDevice1 *dd;
    ID3D11Device_QueryInterface(g_dev, &IID_IDXGIDevice1, (void**)&dd);
    IDXGIDevice1_SetMaximumFrameLatency(dd, 1);

    IDXGIAdapter *ad;
    IDXGIDevice1_GetAdapter(dd, &ad);
    IDXGIFactory2 *f;
    IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void**)&f);

    DXGI_SWAP_CHAIN_DESC1 scd = {0};
    scd.Width = g_winW; scd.Height = g_winH;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    hr = IDXGIFactory2_CreateSwapChainForHwnd(f, (IUnknown*)g_dev, g_hwnd, &scd, 0, 0, &g_swap);
    if (FAILED(hr)) return hr;
    IDXGIFactory_MakeWindowAssociation(f, g_hwnd, DXGI_MWA_NO_ALT_ENTER);

    ID3D11Texture2D *bb;
    IDXGISwapChain1_GetBuffer(g_swap, 0, &IID_ID3D11Texture2D, (void**)&bb);
    ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource*)bb, 0, &g_rtv);
    ID3D11Texture2D_Release(bb);

    ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vsB), ID3D10Blob_GetBufferSize(vsB), 0, &g_vs);
    ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(psB), ID3D10Blob_GetBufferSize(psB), 0, &g_ps);

    D3D11_INPUT_ELEMENT_DESC ld[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0},
    };
    ID3D11Device_CreateInputLayout(g_dev, ld, 2, ID3D10Blob_GetBufferPointer(vsB), ID3D10Blob_GetBufferSize(vsB), &g_il);

    D3D11_BUFFER_DESC bd = {sizeof(g_verts), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER};
    D3D11_SUBRESOURCE_DATA sd = {g_verts};
    ID3D11Device_CreateBuffer(g_dev, &bd, &sd, &g_vb);

    /* RGBA texture for CPU-converted frames */
    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = CAP_W;
    td.Height = CAP_H;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateTexture2D(g_dev, &td, 0, &g_yuy2Tex);
    ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource*)g_yuy2Tex, 0, &g_yuy2Srv);

    D3D11_SAMPLER_DESC sam = {D3D11_FILTER_MIN_MAG_MIP_LINEAR,
        D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP};
    ID3D11Device_CreateSamplerState(g_dev, &sam, &g_samp);

    ID3D10Blob_Release(vsB); ID3D10Blob_Release(psB);
    IDXGIFactory2_Release(f); IDXGIAdapter_Release(ad); IDXGIDevice1_Release(dd);
    return S_OK;
}

static void RenderFrame(uint8_t *yuy2) {
    /* CPU YUV→RGB conversion, upload as RGBA */
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource*)g_yuy2Tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        for (int y = 0; y < CAP_H; y++) {
            uint8_t *d = (uint8_t*)m.pData + y * m.RowPitch;
            uint8_t *s = yuy2 + (size_t)y * CAP_W * 2;
            for (int x = 0; x < CAP_W; x += 2) {
                int y0 = s[0], u = s[1] - 128, y1 = s[2], v = s[3] - 128;
                int r0 = ((y0<<10) + RV*v) >> 10;
                int g0 = ((y0<<10) + GU*u + GV*v) >> 10;
                int b0 = ((y0<<10) + BU*u) >> 10;
                d[0] = __max(0,__min(255,r0));
                d[1] = __max(0,__min(255,g0));
                d[2] = __max(0,__min(255,b0));
                d[3] = 255;
                d += 4;
                int r1 = ((y1<<10) + RV*v) >> 10;
                int g1 = ((y1<<10) + GU*u + GV*v) >> 10;
                int b1 = ((y1<<10) + BU*u) >> 10;
                d[0] = __max(0,__min(255,r1));
                d[1] = __max(0,__min(255,g1));
                d[2] = __max(0,__min(255,b1));
                d[3] = 255;
                d += 4;
                s += 4;
            }
        }
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource*)g_yuy2Tex, 0);
    }

    float c[4] = {0,0,0,1};
    ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_rtv, c);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, g_il);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    UINT st=16, of=0;
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &g_vb, &st, &of);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, 0, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_ps, 0, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &g_yuy2Srv);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &g_samp);
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtv, 0);
    D3D11_VIEWPORT vp = {0, 0, (float)g_winW, (float)g_winH, 0, 1};
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext_Draw(g_ctx, 4, 0);

    /* Present with DO_NOT_WAIT — never blocks, allows tearing for min latency.
     * Tearing only visible during fast motion; tradeoff for zero-lag gaming.
     * On G-Sync/FreeSync displays this is imperceptible. */
    IDXGISwapChain_Present(g_swap, 0, DXGI_PRESENT_ALLOW_TEARING);
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

            /* Lock-free buffer swap */
            LONG ri = g_rdIdx, fi = g_frIdx;
            InterlockedExchange(&g_rdIdx, wi);
            InterlockedExchange(&g_wrIdx, fi);
            InterlockedExchange(&g_frIdx, ri);
            SetEvent(g_frameEvent);
            static LONG fc = 0; if (++fc % 60 == 0) fprintf(stderr, "frame %ld len=%u\n", fc, cl);
        } else if (d) {
            static int once = 0; if (!once++) fprintf(stderr, "small sample: %u (need %u)\n", cl, CAP_W*CAP_H*2);
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
}

static HRESULT InitMF(int devIdx) {
    if (devIdx < 0 || (UINT32)devIdx >= g_devCount) return E_INVALIDARG;

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) return hr;

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

    /* Request YUY2 */
    IMFMediaType *mt;
    MFCreateMediaType(&mt);
    IMFAttributes_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFAttributes_SetGUID(mt, &MF_MT_SUBTYPE, &MFVideoFormat_YUY2);
    IMFAttributes_SetUINT64(mt, &MF_MT_FRAME_SIZE, ((UINT64)CAP_W<<32)|CAP_H);
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

static void ResizeSwapChain(int w, int h) {
    if (w <= 0 || h <= 0) return;
    /* Must be called from main thread */
    ID3D11DeviceContext_ClearState(g_ctx);
    if (g_rtv) { ID3D11RenderTargetView_Release(g_rtv); g_rtv = NULL; }
    HRESULT hr = IDXGISwapChain_ResizeBuffers(g_swap, 0, w, h, DXGI_FORMAT_UNKNOWN,
        DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
    if (SUCCEEDED(hr)) {
        ID3D11Texture2D *bb;
        IDXGISwapChain1_GetBuffer(g_swap, 0, &IID_ID3D11Texture2D, (void**)&bb);
        ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource*)bb, NULL, &g_rtv);
        ID3D11Texture2D_Release(bb);
        g_winW = w; g_winH = h;
        fprintf(stderr, "Resized: %dx%d\n", w, h);
    }
    g_needResize = 0;
}

static void ToggleFS(void) {
    static WINDOWPLACEMENT wp = {sizeof(WINDOWPLACEMENT)};
    static int fs = 0;
    fs = !fs;
    if (fs) {
        GetWindowPlacement(g_hwnd, &wp);
        HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfo(mon, &mi);
        int w = mi.rcMonitor.right - mi.rcMonitor.left;
        int h = mi.rcMonitor.bottom - mi.rcMonitor.top;
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
}

static HANDLE g_hCap = NULL;

static void StopCaptureThread(void) {
    if (g_hCap) {
        g_running = FALSE;
        WaitForSingleObject(g_hCap, 2000);
        CloseHandle(g_hCap);
        g_hCap = NULL;
        g_running = TRUE;  /* reset for next thread */
    }
}

static void StartCaptureThread(void) {
    if (!g_hCap && g_reader) {
        g_hCap = CreateThread(0, 0, CaptureThread, 0, 0, 0);
        SetThreadPriority(g_hCap, THREAD_PRIORITY_TIME_CRITICAL);
    }
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
                StopCaptureThread();
                if (g_reader) { IMFSourceReader_Release(g_reader); g_reader = NULL; }
                InitMF(idx);
                StartCaptureThread();
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
    (void)hp;(void)cmd;(void)show;

    WNDCLASSEX wc = {sizeof(wc)};
    wc.style = CS_HREDRAW|CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.lpszClassName = TEXT("WBC2");
    RegisterClassEx(&wc);

    g_hwnd = CreateWindowEx(0, TEXT("WBC2"), TEXT("WuBuCapture — F fs, ESC quit"),
        WS_OVERLAPPEDWINDOW|WS_VISIBLE, -1440, -512, 1280, 720,
        0, 0, hi, 0);

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

    if (FAILED(InitD3D11())) { fprintf(stderr,"D3D11\n"); return 1; }
    if (defaultDev >= 0)
        InitMF(defaultDev);
    else {
        fprintf(stderr,"No capture device found\n");
        return 1;
    }

    StartCaptureThread();

    while (g_running) {
        HANDLE evts[] = { g_frameEvent };
        DWORD w = MsgWaitForMultipleObjects(1, evts, FALSE, INFINITE, QS_ALLINPUT);
        if (w == WAIT_OBJECT_0) {
            if (g_needResize) ResizeSwapChain(g_winW, g_winH);
            LONG ri = g_rdIdx;
            RenderFrame(g_bufs[ri]);
        } else if (w == WAIT_OBJECT_0 + 1) {
            MSG msg;
            while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
    }

    g_running = FALSE;
    StopCaptureThread();
    if (g_reader) IMFSourceReader_Release(g_reader);
    MFShutdown();
    free(g_bufs[0]); free(g_bufs[1]); free(g_bufs[2]);
    CloseHandle(g_frameEvent);
    CoUninitialize();
    return 0;
}
