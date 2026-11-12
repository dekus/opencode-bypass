#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;
struct nk_context;
struct nk_font_atlas;
enum nk_anti_aliasing;

#ifdef __cplusplus
extern "C" {
#endif

struct nk_context *nk_d3d11_init(ID3D11Device *device, int width, int height, unsigned int max_vertex_buffer,
	unsigned int max_index_buffer);
void nk_d3d11_font_stash_begin(struct nk_font_atlas **atlas);
void nk_d3d11_font_stash_end(void);
int nk_d3d11_handle_event(void *hwnd, unsigned int msg, unsigned long long wparam, long long lparam);
void nk_d3d11_render(ID3D11DeviceContext *context, enum nk_anti_aliasing aa);
void nk_d3d11_resize(ID3D11DeviceContext *context, int width, int height);
void nk_d3d11_shutdown(void);
void opencode_nuklear_init_fonts(void);

#ifdef __cplusplus
}
#endif
