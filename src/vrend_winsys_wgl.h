#ifndef VIRGL_WGL_H
#define VIRGL_WGL_H
#include "vrend_renderer.h"


struct virgl_wgl;


struct virgl_wgl *virgl_wgl_init(void);
void virgl_wgl_destroy(struct virgl_wgl *ve);

virgl_renderer_gl_context virgl_wgl_create_context(struct virgl_wgl *ve, struct virgl_gl_ctx_param *vparams);
void virgl_wgl_destroy_context(struct virgl_wgl *ve, virgl_renderer_gl_context virglctx);
int virgl_wgl_make_context_current(struct virgl_wgl *ve, virgl_renderer_gl_context virglctx);
int virgl_wgl_make_context_current_with_hdc(struct virgl_wgl *vw, virgl_renderer_gl_context virglctx, void* hdc);

#endif