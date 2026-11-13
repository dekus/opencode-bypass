#include "features/ui/opencode_bypass/opencode_bypass_ui.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#include "features/ui/opencode_bypass/opencode_bypass_api.hpp"

#include <windows.h>

#include "vendor/nuklear/nuklear.h"
#include "vendor/xorstr/xorstr.hpp"

static struct nk_color nk_rgb8(unsigned char r, unsigned char g, unsigned char b) {
	struct nk_color c;
	c.r = r;
	c.g = g;
	c.b = b;
	c.a = 255;
	return c;
}

static void begin_window_drag(HWND hwnd, struct nk_context *ctx, struct nk_rect drag_area) {
	if (!hwnd)
		return;
	if (nk_input_is_mouse_hovering_rect(&ctx->input, drag_area) && nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT)) {
		ReleaseCapture();
		SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
	}
}

static void draw_title_bar(struct nk_context *ctx, HWND hwnd, float inner_w) {
	const float bar_h = 32.0f;
	const float btn_w = 36.0f;
	const float gap = ctx->style.window.spacing.x;
	float title_w = inner_w - (btn_w * 3.0f) - (gap * 3.0f);
	if (title_w < 80.0f)
		title_w = 80.0f;

	struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
	struct nk_rect bounds = nk_window_get_bounds(ctx);
	nk_fill_rect(canvas, nk_rect(bounds.x, bounds.y, bounds.w, bar_h + ctx->style.window.padding.y), 0.0f, nk_rgb8(22, 8, 12));
	nk_stroke_line(canvas, bounds.x, bounds.y + bar_h + ctx->style.window.padding.y, bounds.x + bounds.w,
		bounds.y + bar_h + ctx->style.window.padding.y, 1.0f, nk_rgb8(120, 28, 38));

	nk_layout_row_begin(ctx, NK_STATIC, bar_h, 4);
	nk_layout_row_push(ctx, title_w);
	nk_label(ctx, "Opencode Bypass", NK_TEXT_LEFT);
	begin_window_drag(hwnd, ctx, nk_rect(bounds.x, bounds.y, title_w + ctx->style.window.padding.x, bar_h + ctx->style.window.padding.y));
	nk_layout_row_push(ctx, btn_w);
	if (nk_button_label(ctx, "_"))
		ShowWindow(hwnd, SW_MINIMIZE);
	nk_layout_row_push(ctx, btn_w);
	if (IsZoomed(hwnd)) {
		if (nk_button_label(ctx, "R"))
			ShowWindow(hwnd, SW_RESTORE);
	} else {
		if (nk_button_label(ctx, "[]"))
			ShowWindow(hwnd, SW_MAXIMIZE);
	}
	nk_layout_row_push(ctx, btn_w);
	if (nk_button_label(ctx, "X"))
		PostMessageW(hwnd, WM_CLOSE, 0, 0);
	nk_layout_row_end(ctx);
}

static void set_status(opencode_bypass_snapshot *snap, char *buf, int len, const opencode_bypass_action_result &r) {
	if (!buf || len <= 0)
		return;
	std::snprintf(buf, (size_t)len, "%s", r.message.c_str());
	if (snap)
		opencode_bypass_snapshot_refresh(snap, true);
}

void opencode_bypass_ui_apply_red_black_theme(struct nk_context *ctx) {
	struct nk_style *s = &ctx->style;
	s->window.background = nk_rgb8(12, 8, 10);
	s->window.fixed_background = nk_style_item_color(nk_rgb8(16, 10, 12));
	s->window.border_color = nk_rgb8(92, 24, 34);
	s->window.border = 0.0f;
	s->window.rounding = 0.0f;
	s->window.padding = nk_vec2(14, 12);
	s->window.group_padding = nk_vec2(14, 12);
	s->window.spacing = nk_vec2(8, 8);
	s->window.scrollbar_size = nk_vec2(8, 8);
	s->window.header.normal = nk_style_item_color(nk_rgb8(50, 12, 18));
	s->window.header.hover = nk_style_item_color(nk_rgb8(70, 18, 26));
	s->window.header.active = nk_style_item_color(nk_rgb8(90, 22, 32));
	s->window.header.label_normal = nk_rgb8(240, 220, 222);
	s->button.normal = nk_style_item_color(nk_rgb8(36, 12, 16));
	s->button.hover = nk_style_item_color(nk_rgb8(78, 18, 26));
	s->button.active = nk_style_item_color(nk_rgb8(140, 32, 42));
	s->button.border_color = nk_rgb8(110, 28, 36);
	s->button.rounding = 3.0f;
	s->button.border = 1.0f;
	s->button.padding = nk_vec2(8, 4);
	s->button.text_normal = nk_rgb8(236, 226, 226);
	s->button.text_hover = nk_rgb8(255, 240, 240);
	s->button.text_active = nk_rgb8(255, 255, 255);
	s->text.color = nk_rgb8(220, 210, 212);
	s->text.padding = nk_vec2(0, 2);
}

void opencode_bypass_ui_draw_background(struct nk_context *ctx, float width, float height) {
	if (!nk_begin(ctx, "##bg", nk_rect(0, 0, width, height),
			NK_WINDOW_NO_INPUT | NK_WINDOW_BACKGROUND | NK_WINDOW_NO_SCROLLBAR)) {
		nk_end(ctx);
		return;
	}
	struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
	const int bands = 56;
	float band_w = width / (float)bands;
	for (int i = 0; i < bands; ++i) {
		float t = (float)i / (float)(bands - 1);
		unsigned char r = (unsigned char)(6 + t * 52);
		unsigned char g = (unsigned char)(4 + t * 6);
		unsigned char b = (unsigned char)(6 + t * 10);
		nk_fill_rect(canvas, nk_rect(i * band_w, 0, band_w + 1.0f, height), 0.0f, nk_rgb8(r, g, b));
	}
	float glow_w = width * 0.28f;
	nk_fill_rect(canvas, nk_rect(width - glow_w, 0, glow_w, height), 0.0f, nk_rgba(90, 14, 22, 45));
	nk_end(ctx);
}

static void dot_row(struct nk_context *ctx, const char *label, bool on) {
	char line[160];
	std::snprintf(line, sizeof(line), "%s   %s", on ? "+" : "-", label);
	nk_layout_row_dynamic(ctx, 24, 1);
	nk_label_colored(ctx, line, NK_TEXT_LEFT, on ? nk_rgb8(232, 92, 104) : nk_rgb8(168, 156, 160));
}

static void sidebar_tabs(struct nk_context *ctx, opencode_bypass_ui_state *ui) {
	static const char *names[] = {"Status", "WARP", "Workers", "Lane", "Setup", "Logs"};
	for (int i = 0; i < 6; ++i) {
		nk_layout_row_dynamic(ctx, 32, 1);
		struct nk_style *s = &ctx->style;
		struct nk_style_item saved = s->button.normal;
		if (ui->tab == i)
			s->button.normal = s->button.active;
		if (nk_button_label(ctx, names[i]))
			ui->tab = i;
		s->button.normal = saved;
	}
}

static void panel_status(struct nk_context *ctx, opencode_bypass_snapshot *snap, char *status_buf, int status_len) {
	dot_row(ctx, snap->warp_connected ? xorstr("WARP connected") : xorstr("WARP disconnected"), snap->warp_connected);
	dot_row(ctx, snap->keeper_running ? xorstr("Keeper running") : xorstr("Keeper stopped"), snap->keeper_running);
	dot_row(ctx, snap->guard_running ? xorstr("Guard running") : xorstr("Guard stopped"), snap->guard_running);
	dot_row(ctx, snap->proxy_listening ? xorstr("Service proxy up") : xorstr("Service proxy down"), snap->proxy_listening);
	dot_row(ctx, snap->firewall_installed ? xorstr("Firewall kill-switch on") : xorstr("Firewall kill-switch off"),
		snap->firewall_installed);
	dot_row(ctx, snap->desktop_service_wired ? xorstr("Desktop service proxied") : xorstr("Desktop service direct"),
		snap->desktop_service_wired);
	char line[256];
	std::snprintf(line, sizeof(line), "Egress: %s  CF: %s",
		snap->egress_prefix.empty() ? snap->egress_v6.c_str() : (snap->egress_prefix + "/64").c_str(),
		snap->is_cf ? xorstr("yes") : xorstr("no"));
	nk_label(ctx, line, NK_TEXT_LEFT);
	std::snprintf(line, sizeof(line), "Hops: %d  Blacklist: %d  DB msgs: %d", snap->hops, snap->blacklist_count,
		snap->db.messages);
	nk_label(ctx, line, NK_TEXT_LEFT);
	nk_layout_row_dynamic(ctx, 32, 2);
	if (nk_button_label(ctx, xorstr("Refresh"))) {
		opencode_bypass_snapshot_refresh(snap, true);
		std::snprintf(status_buf, status_len, "%s", "Status refreshed.");
	}
	if (nk_button_label(ctx, xorstr("Copy egress")))
		set_status(snap, status_buf, status_len, opencode_bypass_action_copy_text(snap->egress_v6.c_str()));
}

static void panel_warp(struct nk_context *ctx, opencode_bypass_snapshot *snap, char *status_buf, int status_len) {
	nk_layout_row_dynamic(ctx, 28, 1);
	char line[200];
	std::snprintf(line, sizeof(line), "Status: %s  Mode: %s", snap->warp_status_text.c_str(),
		snap->warp_mode.empty() ? xorstr("?") : snap->warp_mode.c_str());
	nk_label(ctx, line, NK_TEXT_LEFT);
	if (!snap->adapter_v6.empty()) {
		std::snprintf(line, sizeof(line), "Adapter v6: %s", snap->adapter_v6.c_str());
		nk_label(ctx, line, NK_TEXT_LEFT);
	}
	nk_layout_row_dynamic(ctx, 34, 3);
	if (nk_button_label(ctx, xorstr("Hop /64")))
		set_status(snap, status_buf, status_len, opencode_bypass_action_hop());
	if (nk_button_label(ctx, xorstr("WARP on")))
		set_status(snap, status_buf, status_len, opencode_bypass_action_warp_connect());
	if (nk_button_label(ctx, xorstr("WARP off")))
		set_status(snap, status_buf, status_len, opencode_bypass_action_warp_disconnect());
}

static void panel_workers(struct nk_context *ctx, opencode_bypass_snapshot *snap, char *status_buf, int status_len) {
	(void)snap;
	nk_layout_row_dynamic(ctx, 34, 2);
	if (nk_button_label(ctx, xorstr("Start keeper")))
		set_status(snap, status_buf, status_len, opencode_bypass_action_toggle_worker(xorstr("keeper"), true));
	if (nk_button_label(ctx, xorstr("Stop keeper")))
		set_status(snap, status_buf, status_len, opencode_bypass_action_toggle_worker(xorstr("keeper"), false));
	if (nk_button_label(ctx, xorstr("Start guard")))
		set_status(snap, status_buf, status_len, opencode_bypass_action_toggle_worker(xorstr("guard"), true));
	if (nk_button_label(ctx, xorstr("Stop guard")))
		set_status(snap, status_buf, status_len, opencode_bypass_action_toggle_worker(xorstr("guard"), false));
	nk_layout_row_dynamic(ctx, 34, 2);
	if (nk_button_label(ctx, xorstr("Wire desktop service")))
		set_status(snap, status_buf, status_len, opencode_bypass_action_toggle_desktop_service_proxy(true));
	if (nk_button_label(ctx, xorstr("Unwire desktop service")))
		set_status(snap, status_buf, status_len, opencode_bypass_action_toggle_desktop_service_proxy(false));
	nk_layout_row_dynamic(ctx, 34, 2);
	if (nk_button_label(ctx, xorstr("Enable autostart")))
		set_status(snap, status_buf, status_len, opencode_bypass_action_toggle_autostart(true));
	if (nk_button_label(ctx, xorstr("Disable autostart")))
		set_status(snap, status_buf, status_len, opencode_bypass_action_toggle_autostart(false));
	if (nk_button_label(ctx, xorstr("Install firewall")))
		set_status(snap, status_buf, status_len, opencode_bypass_action_toggle_firewall(true));
	if (nk_button_label(ctx, xorstr("Remove firewall")))
		set_status(snap, status_buf, status_len, opencode_bypass_action_toggle_firewall(false));
}

static void panel_lane(struct nk_context *ctx, opencode_bypass_snapshot *snap, opencode_bypass_ui_state *ui,
	char *status_buf, int status_len) {
	char line[220];
	std::snprintf(line, sizeof(line), "Pinned edge: %s",
		snap->pinned_edge.empty() ? xorstr("(none)") : snap->pinned_edge.c_str());
	nk_label(ctx, line, NK_TEXT_LEFT);
	std::snprintf(line, sizeof(line), "Override: %s  Strict: %s", snap->edge_override.c_str(),
		snap->strict_mode ? xorstr("on") : xorstr("off"));
	nk_label(ctx, line, NK_TEXT_LEFT);
	if (ui->edge_buf[0] == '\0' && !snap->edge_override.empty())
		std::snprintf(ui->edge_buf, sizeof(ui->edge_buf), "%s", snap->edge_override.c_str());
	nk_layout_row_dynamic(ctx, 30, 1);
	nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, ui->edge_buf, (int)sizeof(ui->edge_buf), nk_filter_default);
	nk_layout_row_dynamic(ctx, 34, 2);
	if (nk_button_label(ctx, xorstr("Set edge override")))
		set_status(snap, status_buf, status_len, opencode_bypass_action_set_edge_override(ui->edge_buf));
	if (nk_button_label(ctx, xorstr("Re-roll edge (auto)")))
		set_status(snap, status_buf, status_len, opencode_bypass_action_reroll_edge());
	if (nk_button_label(ctx, xorstr("Refresh edge pool")))
		set_status(snap, status_buf, status_len, opencode_bypass_action_refresh_edge_pool());
	if (nk_button_label(ctx, xorstr("Clear blacklist")))
		set_status(snap, status_buf, status_len, opencode_bypass_action_clear_blacklist());
	if (snap->strict_mode) {
		if (nk_button_label(ctx, xorstr("Disable strict mode")))
			set_status(snap, status_buf, status_len, opencode_bypass_action_toggle_strict_mode(false));
	} else {
		if (nk_button_label(ctx, xorstr("Enable strict mode")))
			set_status(snap, status_buf, status_len, opencode_bypass_action_toggle_strict_mode(true));
	}
	std::snprintf(line, sizeof(line), "Pool size: %d", (int)snap->edge_pool.size());
	nk_label(ctx, line, NK_TEXT_LEFT);
	for (size_t i = 0; i < snap->edge_pool.size() && i < 8; ++i) {
		std::snprintf(line, sizeof(line), "  %s", snap->edge_pool[i].c_str());
		nk_label(ctx, line, NK_TEXT_LEFT);
	}
	for (size_t i = 0; i < snap->blacklist.size() && i < 6; ++i) {
		std::snprintf(line, sizeof(line), "BL %s", snap->blacklist[i].c_str());
		if (nk_button_label(ctx, line))
			set_status(snap, status_buf, status_len, opencode_bypass_action_copy_text(snap->blacklist[i].c_str()));
	}
}

static void panel_setup(struct nk_context *ctx, opencode_bypass_snapshot *snap, char *status_buf, int status_len) {
	(void)snap;
	nk_layout_row_dynamic(ctx, 36, 1);
	if (nk_button_label(ctx, xorstr("Install / Repair")))
		set_status(snap, status_buf, status_len, opencode_bypass_action_install_or_repair());
	if (nk_button_label(ctx, xorstr("Uninstall all")))
		set_status(snap, status_buf, status_len, opencode_bypass_action_uninstall_all());
	nk_layout_row_dynamic(ctx, 34, 2);
	if (nk_button_label(ctx, xorstr("Launch Desktop")))
		set_status(snap, status_buf, status_len, opencode_bypass_action_launch_desktop());
	if (nk_button_label(ctx, xorstr("Launch CLI")))
		set_status(snap, status_buf, status_len, opencode_bypass_action_launch_cli());
	if (nk_button_label(ctx, xorstr("Open logs folder")))
		set_status(snap, status_buf, status_len, opencode_bypass_action_open_logs_folder());
}

static void panel_logs(struct nk_context *ctx, opencode_bypass_snapshot *snap, opencode_bypass_ui_state *ui) {
	nk_layout_row_dynamic(ctx, 28, 3);
	if (nk_button_label(ctx, xorstr("Guard")))
		ui->log_src = 0;
	if (nk_button_label(ctx, xorstr("Keeper")))
		ui->log_src = 1;
	if (nk_button_label(ctx, xorstr("OpenCode")))
		ui->log_src = 2;
	const std::vector<std::string> *lines = &snap->log_guard;
	if (ui->log_src == 1)
		lines = &snap->log_keeper;
	else if (ui->log_src == 2)
		lines = &snap->log_opencode;
	char line[512];
	size_t start = lines->size() > 18 ? lines->size() - 18 : 0;
	for (size_t i = start; i < lines->size(); ++i) {
		std::snprintf(line, sizeof(line), "%s", (*lines)[i].c_str());
		if ((int)std::strlen(line) > 90)
			line[90] = '\0';
		nk_label(ctx, line, NK_TEXT_LEFT);
	}
}

void opencode_bypass_ui_frame(struct nk_context *ctx, HWND hwnd, float width, float height, opencode_bypass_snapshot *snap,
	opencode_bypass_ui_state *ui, char *status_buf, int status_buf_len) {
	if (!nk_begin(ctx, "##main", nk_rect(0, 0, width, height), NK_WINDOW_NO_SCROLLBAR)) {
		nk_end(ctx);
		return;
	}

	struct nk_rect region = nk_window_get_content_region(ctx);
	float inner_w = region.w;
	const float gap = ctx->style.window.spacing.x;
	const float sidebar_w = 148.0f;
	float content_w = inner_w - sidebar_w - gap;
	if (content_w < 220.0f)
		content_w = 220.0f;

	draw_title_bar(ctx, hwnd, inner_w);

	float used = 32.0f + ctx->style.window.spacing.y;
	float status_h = (status_buf && status_buf[0]) ? 22.0f + ctx->style.window.spacing.y : 0.0f;
	float body_h = inner_w > 0 ? (height - ctx->style.window.padding.y * 2.0f - used - status_h) : 200.0f;
	if (body_h < 160.0f)
		body_h = 160.0f;

	nk_layout_row_begin(ctx, NK_STATIC, body_h, 2);
	nk_layout_row_push(ctx, sidebar_w);
	if (nk_group_begin(ctx, "nav", NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BORDER)) {
		sidebar_tabs(ctx, ui);
		nk_group_end(ctx);
	}

	nk_layout_row_push(ctx, content_w);
	if (nk_group_begin(ctx, "content", NK_WINDOW_BORDER)) {
		switch (ui->tab) {
		case 0:
			panel_status(ctx, snap, status_buf, status_buf_len);
			break;
		case 1:
			panel_warp(ctx, snap, status_buf, status_buf_len);
			break;
		case 2:
			panel_workers(ctx, snap, status_buf, status_buf_len);
			break;
		case 3:
			panel_lane(ctx, snap, ui, status_buf, status_buf_len);
			break;
		case 4:
			panel_setup(ctx, snap, status_buf, status_buf_len);
			break;
		case 5:
			panel_logs(ctx, snap, ui);
			break;
		default:
			ui->tab = 0;
			break;
		}
		nk_group_end(ctx);
	}
	nk_layout_row_end(ctx);

	if (status_buf && status_buf[0]) {
		nk_layout_row_dynamic(ctx, status_h, 1);
		nk_label_colored(ctx, status_buf, NK_TEXT_LEFT, nk_rgb8(255, 160, 170));
	}

	nk_end(ctx);
}
