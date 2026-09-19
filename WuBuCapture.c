/*
 * WuBuCapture v2 — Zero-lag + no-tear for Magewell USB Capture
 *
 * Research-backed 3-thread architecture:
 *   - Capture thread: MF SourceReader → YUY2 → lock-free buf swap → event
 *   - Render thread: event → upload YUY2 → GPU shader → Present(1,0)
 *     (Dedicated thread, never processes window messages → no DXGI deadlock)
 *   - Main thread: window + message pump only
 *
 * Display: Present(1,0) = vsync = no tear
 *   Render thread blocks on Present until vsync (~16ms).
 *   After vsync, checks for new frame, renders, presents again.
 *   If no new frame, re-render last frame and present again.
 *
 * GPU YUV→RGB: YUY2 uploaded as R8G8B8A8 texture, pixel shader decodes.
 *   Uses Load() for exact texel fetch, no filtering artifacts.
 *
 * Build:
 *   gcc -O2 -std=c11 -o WuBuCapture.exe WuBuCapture.c \
 *     -lmfplat -lmf -lmfreadwrite -lmfuuid \
 *     -ld3d11 -ldxgi -ld3dcompiler \
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
static ID3D11Texture2D *g_tex;
static ID3D11ShaderResourceView *g_srv;
static ID3D11VertexShader *g_vs;
static ID3D11PixelShader *g_ps;
static ID3D11InputLayout *g_il;
static ID3D11Buffer *g_vb;
static HWND g_hwnd;
static volatile BOOL g_running = TRUE;
static int g_winW = 1280, g_winH = 720;
static const int CAP_W = 1920, CAP_H = 1080;

static uint8_t *g_bufs[3];
static volatile LONG g_wrIdx, g_rdIdx, g_frIdx;
static HANDLE g_frameEvent;
static IMFSourceReader *g_reader;

static IMFActivate **g_devices;
static UINT32 g_devCount;
static WCHAR **g_devNames;

static HANDLE g_hCap = NULL;
static volatile int g_needResize = 0;
static volatile int g_stopCap = 0;  /* signal capture thread to stop */
static volatile int g_switching = 0;

/* === PIPELINE TIMING === */
static FILE *g_tlog = NULL;
static volatile LONG g_tCapCount = 0, g_tRenderCount = 0;
static LARGE_INTEGER g_tQP = {0};
static volatile LONGLONG g_t0 = 0, g_t1 = 0;

static inline LONGLONG QPnow(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static double QPms(LONGLONG s, LONGLONG e) { return (double)(e-s)*1000.0/(double)g_tQP.QuadPart; }

static void InitTiming(void) {
    QueryPerformanceFrequency(&g_tQP);
    g_tlog = fopen("pipeline_timing.csv", "w");
    if (g_tlog) fprintf(g_tlog, "frame,t0_cap,t1_recv,t2_render,t3_draw,t4_present,cap_ms,rend_ms,pipeline_ms,upload_ms,present_ms\n");
}

static void LogFrame(LONGLONG t0, LONGLONG t1, LONGLONG t2, LONGLONG t3, LONGLONG t4) {
    static LONGLONG lastT0=0, lastT4=0;
    static LONG id=0;
    LONG n = InterlockedIncrement(&id);
    double ci = lastT0?QPms(lastT0,t0):0;
    double ri = lastT4?QPms(lastT4,t4):0;
    lastT0=t0; lastT4=t4;
    if (g_tlog && n<=3000)
        fprintf(g_tlog,"%ld,%lld,%lld,%lld,%lld,%lld,%.2f,%.2f,%.2f,%.3f,%.2f\n",
            n,t0,t1,t2,t3,t4,ci,ri,QPms(t0,t4),QPms(t2,t3),QPms(t3,t4));
    if (n%120==0) {
        fprintf(stderr,"[TIMING] cap_int=%.1fms rend_int=%.1fms pipeline=%.1fms upload=%.2fms present_wait=%.1fms\n",
            ci,ri,QPms(t0,t4),QPms(t2,t3),QPms(t3,t4));
        if (g_tlog) fflush(g_tlog);
    }
}
/* === END TIMING === */

static const float g_verts[] = {
    -1, 1, 0, 0,  1, 1, 1, 0,  -1,-1, 0, 1,  1,-1, 1, 1
};

static const char *g_vsSrc =
    "struct VIN{float2 p:POSITION;float2 u:TEXCOORD;};"
    "struct VOUT{float4 p:SV_POSITION;float2 u:TEXCOORD;};"
    "VOUT main(VIN i){VOUT o;o.p=float4(i.p,0,1);o.u=i.u;return o;}";

/* YUY2 → RGB. Texture R8G8B8A8, width=CAP_W/2. Load() for exact fetch. */
static const char *g_psSrc =
    "Texture2D tex:register(t0);"
    "float3 YUVtoRGB(float3 yuv){"
    "  float u=yuv.y-0.5;"
    "  float v=yuv.z-0.5;"
    "  return clamp(float3("
    "    yuv.x+1.5748*v,"
    "    yuv.x-0.1873*u-0.4681*v,"
    "    yuv.x+1.8556*u),0.0,1.0);"
    "}"
    "float4 main(float4 pos:SV_POSITION, float2 uv:TEXCOORD):SV_TARGET{"
    "  float2 src=float2(uv.x*1920.0,uv.y*1080.0);"
    "  int tx=int(src.x*0.5);"
    "  int ty=int(src.y);"
    "  int sub=int(src.x)-tx*2;"
    "  float4 yuy2=tex.Load(int3(tx,ty,0));"
    "  float y=(sub==0)?yuy2.r:yuy2.b;"
    "  return float4(YUVtoRGB(float3(y,yuy2.g,yuy2.a)),1.0);"
    "}";

static HRESULT InitD3D11(void) {
    HRESULT hr;
    ID3DBlob *vsB, *psB;
    hr = D3DCompile(g_vsSrc, strlen(g_vsSrc), 0, 0, 0, "main", "vs_4_0", 0, 0, &vsB, 0);
    if (FAILED(hr)) return hr;
    hr = D3DCompile(g_psSrc, strlen(g_psSrc), 0, 0, 0, "main", "ps_4_0", 0, 0, &psB, 0);
    if (FAILED(hr)) return hr;

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

    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = CAP_W / 2;
    td.Height = CAP_H;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateTexture2D(g_dev, &td, 0, &g_tex);
    ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource*)g_tex, 0, &g_srv);

    ID3D10Blob_Release(vsB); ID3D10Blob_Release(psB);
    IDXGIFactory2_Release(f); IDXGIAdapter_Release(ad); IDXGIDevice1_Release(dd);
    return S_OK;
}

static void DoRender(uint8_t *yuy2) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource*)g_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        for (int y = 0; y < CAP_H; y++) {
            memcpy((uint8_t*)m.pData + y * m.RowPitch, yuy2 + (size_t)y * CAP_W * 2, CAP_W * 2);
        }
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource*)g_tex, 0);
    }

    float c[4] = {0,0,0,1};
    ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_rtv, c);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, g_il);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    UINT st=16, of=0;
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &g_vb, &st, &of);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, 0, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_ps, 0, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &g_srv);
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtv, 0);
    D3D11_VIEWPORT vp = {0, 0, (float)g_winW, (float)g_winH, 0, 1};
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext_Draw(g_ctx, 4, 0);

    /* VSync present — blocks until vsync, no tear */
    IDXGISwapChain_Present(g_swap, 1, 0);
}

/* Render thread — dedicated, never touches window messages */
static DWORD WINAPI RenderThread(void *arg) {
    (void)arg;
    fprintf(stderr, "RenderThread started\n");
    LONG lastRd = -1;
    while (g_running) {
        if (g_needResize) {
            int rw = g_winW, rh = g_winH;
            if (rw > 0 && rh > 0) {
                ID3D11DeviceContext_ClearState(g_ctx);
                if (g_rtv) { ID3D11RenderTargetView_Release(g_rtv); g_rtv = NULL; }
                HRESULT hr = IDXGISwapChain_ResizeBuffers(g_swap, 0, rw, rh, DXGI_FORMAT_UNKNOWN, 0);
                if (SUCCEEDED(hr)) {
                    ID3D11Texture2D *bb;
                    IDXGISwapChain1_GetBuffer(g_swap, 0, &IID_ID3D11Texture2D, (void**)&bb);
                    ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource*)bb, NULL, &g_rtv);
                    ID3D11Texture2D_Release(bb);
                    fprintf(stderr, "Resized: %dx%d\n", rw, rh);
                }
                g_needResize = 0;
            }
        }

        /* Wait for new frame signal (with timeout for resize/g_running checks) */
        DWORD w = WaitForSingleObject(g_frameEvent, 16);

        /* Always render latest frame and present */
        LONG ri = g_rdIdx;
        if (ri != lastRd || w == WAIT_OBJECT_0) {
            LONGLONG t2 = QPnow();
            DoRender(g_bufs[ri]);
            LONGLONG t4 = QPnow();
            LogFrame(g_t0, g_t1, t2, t2, t4);  /* t3≈t2 (draw is fast) */
            InterlockedIncrement(&g_tRenderCount);
            lastRd = ri;
        }
    }
    return 0;
}

static DWORD WINAPI CaptureThread(void *arg) {
    (void)arg;
    fprintf(stderr, "CaptureThread started\n");
    LONG fc = 0;
    while (!g_stopCap) {
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
            LONGLONG t0 = QPnow();
            int wi = g_wrIdx;
            memcpy(g_bufs[wi], d, (size_t)CAP_W * CAP_H * 2);
            LONG ri = g_rdIdx, fi = g_frIdx;
            InterlockedExchange(&g_rdIdx, wi);
            InterlockedExchange(&g_wrIdx, fi);
            InterlockedExchange(&g_frIdx, ri);
            g_t0 = t0;
            g_t1 = QPnow();
            InterlockedIncrement(&g_tCapCount);
            SetEvent(g_frameEvent);
        }
        IMFMediaBuffer_Unlock(mb);
        IMFMediaBuffer_Release(mb);
    }
    return 0;
}

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
    fprintf(stderr, "Press 0-%u to switch device\n\n", g_devCount - 1);
}

static HRESULT InitMF(int devIdx) {
    if (devIdx < 0 || (UINT32)devIdx >= g_devCount) return E_INVALIDARG;

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) return hr;

    IMFMediaSource *src = NULL;
    hr = IMFActivate_ActivateObject(g_devices[devIdx], &IID_IMFMediaSource, (void**)&src);
    if (FAILED(hr) || !src) return E_FAIL;

    if (g_devNames[devIdx])
        fwprintf(stderr, L"Selected: [%d] %s\n", devIdx, g_devNames[devIdx]);
    else
        fprintf(stderr, "Selected: [%d]\n", devIdx);

    hr = MFCreateSourceReaderFromMediaSource(src, NULL, &g_reader);
    IMFMediaSource_Release(src);
    if (FAILED(hr)) return hr;

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

static void StopCapture(void) {
    if (g_hCap) {
        g_stopCap = 1;
        WaitForSingleObject(g_hCap, 3000);
        CloseHandle(g_hCap);
        g_hCap = NULL;
        g_stopCap = 0;
    }
}

static void StartCapture(void) {
    if (!g_hCap && g_reader) {
        g_hCap = CreateThread(0, 0, CaptureThread, 0, 0, 0);
        SetThreadPriority(g_hCap, THREAD_PRIORITY_TIME_CRITICAL);
    }
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
        g_winW = w; g_winH = h;
        g_needResize = 1;
    } else {
        SetWindowLong(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW|WS_VISIBLE);
        SetWindowPlacement(g_hwnd, &wp);
        RECT rc; GetClientRect(g_hwnd, &rc);
        g_winW = rc.right; g_winH = rc.bottom;
        g_needResize = 1;
    }
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { g_running = FALSE; PostQuitMessage(0); return 0; }
    if (m == WM_KEYDOWN) {
        if (w == VK_ESCAPE) { g_running = FALSE; PostQuitMessage(0); return 0; }
        if (w == 'F') { ToggleFS(); return 0; }
        if (w >= '0' && w <= '9') {
            int idx = w - '0';
            if (idx < (int)g_devCount && !g_switching) {
                g_switching = 1;
                fprintf(stderr, "Switch to device %d\n", idx);
                StopCapture();
                if (g_reader) { IMFSourceReader_Release(g_reader); g_reader = NULL; }
                InitMF(idx);
                StartCapture();
                g_switching = 0;
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
    HRESULT hr;

    WNDCLASSEX wc = {sizeof(wc)};
    wc.style = CS_HREDRAW|CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.lpszClassName = TEXT("WBC5");
    RegisterClassEx(&wc);

    g_hwnd = CreateWindowEx(0, TEXT("WBC5"), TEXT("WuBuCapture — F fs, 0-9 device, ESC quit"),
        WS_OVERLAPPEDWINDOW|WS_VISIBLE, -1440, -512, 1280, 720,
        0, 0, hi, 0);

    CoInitializeEx(0, COINIT_APARTMENTTHREADED);
    EnumDevices();

    int defaultDev = -1, usbIdx = 0;
    for (UINT32 i = 0; i < g_devCount; i++) {
        if (g_devNames[i] && wcsstr(g_devNames[i], L"USB3 Video")) {
            if (usbIdx++ == 1) { defaultDev = i; break; }
        }
    }
    if (defaultDev < 0) defaultDev = (g_devCount > 0) ? 0 : -1;

    if (FAILED(InitD3D11())) { fprintf(stderr,"D3D11\n"); return 1; }
    if (defaultDev >= 0) InitMF(defaultDev);
    else { fprintf(stderr,"No device\n"); return 1; }

    InitTiming();

    /* Start capture thread */
    StartCapture();

    /* Start render thread (dedicated, never processes window messages) */
    HANDLE hRender = CreateThread(0, 0, RenderThread, 0, 0, 0);
    SetThreadPriority(hRender, THREAD_PRIORITY_HIGHEST);

    /* Main thread: message pump only */
    MSG msg;
    while (GetMessage(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_running = FALSE;
    WaitForSingleObject(hRender, 2000);
    CloseHandle(hRender);
    StopCapture();
    if (g_reader) IMFSourceReader_Release(g_reader);
    MFShutdown();
    free(g_bufs[0]); free(g_bufs[1]); free(g_bufs[2]);
    CloseHandle(g_frameEvent);
    CoUninitialize();
    return 0;
}
