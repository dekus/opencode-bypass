#ifndef OPENCODE_BYPASS_UI_HPP
#define OPENCODE_BYPASS_UI_HPP

#include <windows.h>

struct nk_context;
struct opencode_bypass_snapshot;

struct opencode_bypass_ui_state {
	int tab = 0;
	int log_src = 0;
	char edge_buf[192];
};

void opencode_bypass_ui_apply_red_black_theme(struct nk_context *ctx);
void opencode_bypass_ui_draw_background(struct nk_context *ctx, float width, float height);
void opencode_bypass_ui_frame(struct nk_context *ctx, HWND hwnd, float width, float height, opencode_bypass_snapshot *snap,
	opencode_bypass_ui_state *ui, char *status_buf, int status_buf_len);

#endif
