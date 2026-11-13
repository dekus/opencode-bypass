#include "features/ui/opencode_bypass/opencode_bypass.hpp"

#include "features/ui/opencode_bypass/opencode_bypass_api.hpp"
#include "features/ui/opencode_bypass/opencode_bypass_ui.hpp"
#include "ouc-common/ouc-common.hpp"

#include "vendor/nuklear/nuklear.h"
#include "features/ui/opencode_bypass/opencode_bypass_nuklear_decl.h"

#include <cstdio>

#include <d3d11.h>
#include <dxgi.h>

static const wchar_t *k_window_title = L"Opencode Bypass";

static const int k_max_vertex = 512 * 1024;
static const int k_max_index = 128 * 1024;

struct d3d_app {
	IDXGISwapChain *swap_chain = nullptr;
	ID3D11Device *device = nullptr;
	ID3D11DeviceContext *context = nullptr;
	ID3D11RenderTargetView *rt_view = nullptr;
};

static d3d_app g_d3d;
static struct nk_context *g_nk_ctx = nullptr;
static int g_resize_w = 0;
static int g_resize_h = 0;

static void set_swap_chain_size(int width, int height) {
	if (width <= 0 || height <= 0 || !g_d3d.swap_chain || !g_d3d.context)
		return;
	if (g_d3d.rt_view) {
		g_d3d.rt_view->Release();
		g_d3d.rt_view = nullptr;
	}
	g_d3d.context->OMSetRenderTargets(0, nullptr, nullptr);
	if (FAILED(g_d3d.swap_chain->ResizeBuffers(0, (UINT)width, (UINT)height, DXGI_FORMAT_UNKNOWN, 0)))
		return;

	ID3D11Texture2D *back = nullptr;
	if (FAILED(g_d3d.swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back)))
		return;

	D3D11_RENDER_TARGET_VIEW_DESC desc = {};
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	if (FAILED(g_d3d.device->CreateRenderTargetView(back, &desc, &g_d3d.rt_view))) {
		back->Release();
		return;
	}
	back->Release();
}

static void apply_pending_resize() {
	if (g_resize_w <= 0 || g_resize_h <= 0 || !g_d3d.swap_chain)
		return;
	const int w = g_resize_w;
	const int h = g_resize_h;
	g_resize_w = 0;
	g_resize_h = 0;
	set_swap_chain_size(w, h);
	if (g_nk_ctx)
		nk_d3d11_resize(g_d3d.context, w, h);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	if (msg == WM_CLOSE) {
		DestroyWindow(hwnd);
		return 0;
	}
	if (msg == WM_DESTROY) {
		PostQuitMessage(0);
		return 0;
	}
	if (msg == WM_GETMINMAXINFO) {
		auto *mi = reinterpret_cast<MINMAXINFO *>(lp);
		mi->ptMinTrackSize.x = 880;
		mi->ptMinTrackSize.y = 520;
		return 0;
	}
	if (msg == WM_SIZE && wp != SIZE_MINIMIZED) {
		int w = LOWORD(lp);
		int h = HIWORD(lp);
		if (w > 0 && h > 0) {
			g_resize_w = w;
			g_resize_h = h;
		}
	}
	if (g_nk_ctx && nk_d3d11_handle_event(hwnd, msg, wp, lp))
		return 0;
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool d3d_init(HWND hwnd, int width, int height) {
	DXGI_SWAP_CHAIN_DESC sc = {};
	sc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sc.BufferDesc.Width = (UINT)width;
	sc.BufferDesc.Height = (UINT)height;
	sc.SampleDesc.Count = 1;
	sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sc.BufferCount = 2;
	sc.OutputWindow = hwnd;
	sc.Windowed = TRUE;
	sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

	D3D_FEATURE_LEVEL level;
	HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
		D3D11_SDK_VERSION, &sc, &g_d3d.swap_chain, &g_d3d.device, &level, &g_d3d.context);
	if (FAILED(hr))
		hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sc,
			&g_d3d.swap_chain, &g_d3d.device, &level, &g_d3d.context);
	if (FAILED(hr))
		return false;

	set_swap_chain_size(width, height);
	return g_d3d.rt_view != nullptr;
}

int opencode_bypass_main(HINSTANCE instance) {
	ouc_ensure_dirs();

	const int width = 980;
	const int height = 540;

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = wnd_proc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(IDC_ARROW));
	wc.lpszClassName = L"OpencodeBypassNk";
	RegisterClassExW(&wc);

	const int sw = GetSystemMetrics(SM_CXSCREEN);
	const int sh = GetSystemMetrics(SM_CYSCREEN);
	const int pos_x = (sw - width) > 0 ? (sw - width) / 2 : 0;
	const int pos_y = (sh - height) > 0 ? (sh - height) / 2 : 0;
	const DWORD style = WS_POPUP;
	HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, k_window_title, style, pos_x, pos_y, width, height,
		nullptr, nullptr, instance, nullptr);
	if (!hwnd)
		return 1;

	ShowWindow(hwnd, SW_SHOW);

	RECT client {};
	GetClientRect(hwnd, &client);
	int cw = client.right - client.left;
	int ch = client.bottom - client.top;
	if (cw < 64)
		cw = width;
	if (ch < 64)
		ch = height;

	if (!d3d_init(hwnd, cw, ch)) {
		MessageBoxW(hwnd, L"D3D11 init failed.", k_window_title, MB_ICONERROR);
		return 1;
	}

	g_nk_ctx = nk_d3d11_init(g_d3d.device, cw, ch, k_max_vertex, k_max_index);
	if (!g_nk_ctx) {
		MessageBoxW(hwnd, L"UI init failed.", k_window_title, MB_ICONERROR);
		return 1;
	}
	opencode_nuklear_init_fonts();
	opencode_bypass_ui_apply_red_black_theme(g_nk_ctx);

	opencode_bypass_snapshot snap = {};
	opencode_bypass_ui_state ui = {};
	std::snprintf(ui.edge_buf, sizeof(ui.edge_buf), "auto");
	char status[384] = {};
	DWORD last_refresh = GetTickCount();
	DWORD last_tick = last_refresh;
	bool booted = false;
	bool boot_pending = true;

	for (;;) {
		MSG msg;
		nk_input_begin(g_nk_ctx);
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				nk_input_end(g_nk_ctx);
				goto done;
			}
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		nk_input_end(g_nk_ctx);

		DWORD now = GetTickCount();
		float dt = (float)(now - last_tick) / 1000.0f;
		if (dt <= 0.0f || dt > 0.25f)
			dt = 1.0f / 60.0f;
		last_tick = now;
		g_nk_ctx->delta_time_seconds = dt;

		(void)booted;
		(void)last_refresh;

		apply_pending_resize();

		GetClientRect(hwnd, &client);
		float fw = (float)(client.right - client.left);
		float fh = (float)(client.bottom - client.top);
		if (fw < 1.0f || fh < 1.0f || !g_d3d.rt_view)
			continue;

		opencode_bypass_ui_draw_background(g_nk_ctx, fw, fh);
		opencode_bypass_ui_frame(g_nk_ctx, hwnd, fw, fh, &snap, &ui, status, (int)sizeof(status));

		const float clear[4] = {0.02f, 0.02f, 0.025f, 1.0f};
		g_d3d.context->OMSetRenderTargets(1, &g_d3d.rt_view, nullptr);
		g_d3d.context->ClearRenderTargetView(g_d3d.rt_view, clear);
		nk_d3d11_render(g_d3d.context, NK_ANTI_ALIASING_ON);
		g_d3d.swap_chain->Present(1, 0);

		if (boot_pending) {
			booted = true;
			boot_pending = false;
			last_refresh = now;
		}
	}

done:
	g_nk_ctx = nullptr;
	nk_d3d11_shutdown();
	if (g_d3d.rt_view)
		g_d3d.rt_view->Release();
	if (g_d3d.context)
		g_d3d.context->Release();
	if (g_d3d.device)
		g_d3d.device->Release();
	if (g_d3d.swap_chain)
		g_d3d.swap_chain->Release();
	UnregisterClassW(wc.lpszClassName, instance);
	return 0;
}
