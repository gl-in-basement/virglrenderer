// We assume that some WGL context has been created and made current.
#include <windows.h>
#include "vrend_winsys_wgl.h"
#include "vrend_debug.h"
#include <epoxy/wgl.h>
#include <epoxy/gl.h>
struct wgl_tup{
    HDC hdc;
    HGLRC hglrc;
};
struct virgl_wgl{
    HDC compatible_hdc;
};


struct virgl_wgl *virgl_wgl_init(void){
    struct virgl_wgl* wgl = calloc(sizeof(*wgl), 1);
    HDC hdc = wglGetCurrentDC();
    HGLRC hglrc = wglGetCurrentContext();
    if(!hdc){
        vrend_printf( "wglGetCurrentDC() returned NULL. You may want to bind a DC first.\n");
        return NULL;
    }
    if(!hglrc){
        vrend_printf( "wglGetCurrentContext() returned NULL. You may want to bind a GLRC first.\n");
        return NULL;
    }
    wgl->compatible_hdc=CreateCompatibleDC(hdc);
    return wgl;

}
void virgl_wgl_destroy(struct virgl_wgl *vw){
    DeleteDC(vw->compatible_hdc);
    free(vw);
}

struct wingl_context{
    HGLRC hglrc;
};
virgl_renderer_gl_context virgl_wgl_create_context(struct virgl_wgl *vw, struct virgl_gl_ctx_param *vparams){
    int context_attribs[] = {
      WGL_CONTEXT_MAJOR_VERSION_ARB, vparams->major_ver,
      WGL_CONTEXT_MINOR_VERSION_ARB, vparams->minor_ver,
      0
    };
    HGLRC hglrc = wglCreateContextAttribsARB(vw->compatible_hdc, vparams->shared? wglGetCurrentContext() : NULL, context_attribs);
    struct wingl_context* ctx = calloc(sizeof(*ctx), 1);
    ctx->hglrc = hglrc;
    return (virgl_renderer_gl_context)ctx;
}



void virgl_wgl_destroy_context(struct virgl_wgl *vw, virgl_renderer_gl_context virglctx){
    wglDeleteContext(((struct wingl_context*)virglctx)->hglrc);
    free(virglctx);
}
int virgl_wgl_make_context_current(struct virgl_wgl *vw, virgl_renderer_gl_context virglctx){
    return virgl_wgl_make_context_current_with_hdc(vw, virglctx, wglGetCurrentDC());
}

int virgl_wgl_make_context_current_with_hdc(struct virgl_wgl *vw, virgl_renderer_gl_context virglctx, void* hdc){
    return wglMakeCurrent(hdc, ((struct wingl_context*)virglctx)->hglrc);
}