#ifdef _MSC_VER
#pragma warning(disable : 4100 4127 4244 4245 4456 4457 4701 4702 4706 4996 4267 4305 4146)
#endif

#define COBJMACROS
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_UINT_DRAW_INDEX
#define NK_IMPLEMENTATION
#define NK_D3D11_IMPLEMENTATION

#include "vendor/nuklear/nuklear.h"
#include "vendor/nuklear/nuklear_d3d11.h"

void opencode_nuklear_init_fonts(void)
{
	struct nk_font_atlas *atlas;
	nk_d3d11_font_stash_begin(&atlas);
	nk_d3d11_font_stash_end();
}
