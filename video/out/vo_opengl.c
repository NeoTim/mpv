/*
 * Based on vo_gl.c by Reimar Doeffinger.
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <assert.h>

#include <libavutil/common.h>

#include "config.h"

#include "mpv_talloc.h"
#include "common/common.h"
#include "misc/bstr.h"
#include "common/msg.h"
#include "common/global.h"
#include "options/m_config.h"
#include "vo.h"
#include "video/mp_image.h"
#include "sub/osd.h"

#include "opengl/context.h"
#include "opengl/utils.h"
#include "opengl/hwdec.h"
#include "opengl/osd.h"
#include "filter_kernels.h"
#include "video/hwdec.h"
#include "opengl/video.h"

#define NUM_VSYNC_FENCES 10

struct gl_priv {
    struct vo *vo;
    struct mp_log *log;
    MPGLContext *glctx;
    GL *gl;

    struct gl_video *renderer;

    struct gl_hwdec *hwdec;

    int events;

    void *original_opts;

    // Options
    struct gl_video_opts *renderer_opts;
    int use_glFinish;
    int waitvsync;
    int use_gl_debug;
    int allow_sw;
    int swap_interval;
    int dwm_flush;
    int allow_direct_composition;
    int opt_vsync_fences;

    char *backend;
    int es;

    int frames_rendered;
    unsigned int prev_sgi_sync_count;

    // check-pattern sub-option; for testing/debugging
    int opt_pattern[2];
    int last_pattern;
    int matches, mismatches;

    GLsync vsync_fences[NUM_VSYNC_FENCES];
    int num_vsync_fences;
};

static void resize(struct gl_priv *p)
{
    struct vo *vo = p->vo;

    MP_VERBOSE(vo, "Resize: %dx%d\n", vo->dwidth, vo->dheight);

    struct mp_rect src, dst;
    struct mp_osd_res osd;
    vo_get_src_dst_rects(vo, &src, &dst, &osd);

    int height = p->glctx->flip_v ? vo->dheight : -vo->dheight;
    gl_video_resize(p->renderer, vo->dwidth, height, &src, &dst, &osd);

    vo->want_redraw = true;
}

static void check_pattern(struct vo *vo, int item)
{
    struct gl_priv *p = vo->priv;
    int expected = p->opt_pattern[p->last_pattern];
    if (item == expected) {
        p->last_pattern++;
        if (p->last_pattern >= 2)
            p->last_pattern = 0;
        p->matches++;
    } else {
        p->mismatches++;
        MP_WARN(vo, "wrong pattern, expected %d got %d (hit: %d, mis: %d)\n",
                expected, item, p->matches, p->mismatches);
    }
}

static void draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct gl_priv *p = vo->priv;
    GL *gl = p->gl;

    if (gl->FenceSync && p->num_vsync_fences < p->opt_vsync_fences) {
        GLsync fence = gl->FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);;
        if (fence)
            p->vsync_fences[p->num_vsync_fences++] = fence;
    }

    gl_video_render_frame(p->renderer, frame, gl->main_fb);

    if (p->use_glFinish)
        gl->Finish();
}

static void flip_page(struct vo *vo)
{
    struct gl_priv *p = vo->priv;
    GL *gl = p->gl;

    mpgl_swap_buffers(p->glctx);

    p->frames_rendered++;
    if (p->frames_rendered > 5 && !p->use_gl_debug)
        gl_video_set_debug(p->renderer, false);

    if (p->use_glFinish)
        gl->Finish();

    if (p->waitvsync || p->opt_pattern[0]) {
        if (gl->GetVideoSync) {
            unsigned int n1 = 0, n2 = 0;
            gl->GetVideoSync(&n1);
            if (p->waitvsync)
                gl->WaitVideoSync(2, (n1 + 1) % 2, &n2);
            int step = n1 - p->prev_sgi_sync_count;
            p->prev_sgi_sync_count = n1;
            MP_DBG(vo, "Flip counts: %u->%u, step=%d\n", n1, n2, step);
            if (p->opt_pattern[0])
                check_pattern(vo, step);
        } else {
            MP_WARN(vo, "GLX_SGI_video_sync not available, disabling.\n");
            p->waitvsync = 0;
            p->opt_pattern[0] = 0;
        }
    }
    while (p->opt_vsync_fences > 0 && p->num_vsync_fences >= p->opt_vsync_fences) {
        gl->ClientWaitSync(p->vsync_fences[0], GL_SYNC_FLUSH_COMMANDS_BIT, 1e9);
        gl->DeleteSync(p->vsync_fences[0]);
        MP_TARRAY_REMOVE_AT(p->vsync_fences, p->num_vsync_fences, 0);
    }
}

static int query_format(struct vo *vo, int format)
{
    struct gl_priv *p = vo->priv;
    if (!gl_video_check_format(p->renderer, format))
        return 0;
    return 1;
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct gl_priv *p = vo->priv;

    if (mpgl_reconfig_window(p->glctx) < 0)
        return -1;

    resize(p);

    gl_video_config(p->renderer, params);

    return 0;
}

static void request_hwdec_api(struct vo *vo, void *api)
{
    struct gl_priv *p = vo->priv;

    if (p->hwdec)
        return;

    p->hwdec = gl_hwdec_load_api(p->vo->log, p->gl, p->vo->global,
                                 vo->hwdec_devs, (intptr_t)api);
    gl_video_set_hwdec(p->renderer, p->hwdec);
}

static void call_request_hwdec_api(void *ctx, enum hwdec_type type)
{
    // Roundabout way to run hwdec loading on the VO thread.
    // Redirects to request_hwdec_api().
    vo_control(ctx, VOCTRL_LOAD_HWDEC_API, (void *)(intptr_t)type);
}

static void get_and_update_icc_profile(struct gl_priv *p)
{
    if (gl_video_icc_auto_enabled(p->renderer)) {
        MP_VERBOSE(p, "Querying ICC profile...\n");
        bstr icc = bstr0(NULL);
        int r = mpgl_control(p->glctx, &p->events, VOCTRL_GET_ICC_PROFILE, &icc);

        if (r != VO_NOTAVAIL) {
            if (r == VO_FALSE) {
                MP_WARN(p, "Could not retrieve an ICC profile.\n");
            } else if (r == VO_NOTIMPL) {
                MP_ERR(p, "icc-profile-auto not implemented on this platform.\n");
            }

            gl_video_set_icc_profile(p->renderer, icc);
        }
    }
}

static void get_and_update_ambient_lighting(struct gl_priv *p)
{
    int lux;
    int r = mpgl_control(p->glctx, &p->events, VOCTRL_GET_AMBIENT_LUX, &lux);
    if (r == VO_TRUE) {
        gl_video_set_ambient_lux(p->renderer, lux);
    }
    if (r != VO_TRUE && gl_video_gamma_auto_enabled(p->renderer)) {
        MP_ERR(p, "gamma_auto option provided, but querying for ambient"
                  " lighting is not supported on this platform\n");
    }
}

static const struct m_option options[];

static const struct m_sub_options opengl_conf = {
    .opts = options,
    .size = sizeof(struct gl_priv),
};

static bool reparse_cmdline(struct vo *vo, char *args)
{
    struct gl_priv *p = vo->priv;
    int r = 0;

    struct gl_priv *opts = p;

    if (strcmp(args, "-") == 0) {
        opts = p->original_opts;
    } else {
        r = m_config_parse_suboptions(vo->config, "opengl", args);
    }

    gl_video_set_options(p->renderer, opts->renderer_opts);
    get_and_update_icc_profile(p);
    gl_video_configure_queue(p->renderer, p->vo);
    p->vo->want_redraw = true;

    return r >= 0;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct gl_priv *p = vo->priv;

    switch (request) {
    case VOCTRL_GET_PANSCAN:
        return VO_TRUE;
    case VOCTRL_SET_PANSCAN:
        resize(p);
        return VO_TRUE;
    case VOCTRL_GET_EQUALIZER: {
        struct voctrl_get_equalizer_args *args = data;
        struct mp_csp_equalizer *eq = gl_video_eq_ptr(p->renderer);
        bool r = mp_csp_equalizer_get(eq, args->name, args->valueptr) >= 0;
        return r ? VO_TRUE : VO_NOTIMPL;
    }
    case VOCTRL_SET_EQUALIZER: {
        struct voctrl_set_equalizer_args *args = data;
        struct mp_csp_equalizer *eq = gl_video_eq_ptr(p->renderer);
        if (mp_csp_equalizer_set(eq, args->name, args->value) >= 0) {
            gl_video_eq_update(p->renderer);
            vo->want_redraw = true;
            return VO_TRUE;
        }
        return VO_NOTIMPL;
    }
    case VOCTRL_SCREENSHOT_WIN: {
        struct mp_image *screen = gl_read_window_contents(p->gl);
        // set image parameters according to the display, if possible
        if (screen) {
            screen->params.color = gl_video_get_output_colorspace(p->renderer);
            if (p->glctx->flip_v)
                mp_image_vflip(screen);
        }
        *(struct mp_image **)data = screen;
        return true;
    }
    case VOCTRL_LOAD_HWDEC_API:
        request_hwdec_api(vo, data);
        return true;
    case VOCTRL_SET_COMMAND_LINE: {
        char *arg = data;
        return reparse_cmdline(vo, arg);
    }
    case VOCTRL_RESET:
        gl_video_reset(p->renderer);
        return true;
    case VOCTRL_PAUSE:
        if (gl_video_showing_interpolated_frame(p->renderer)) {
            vo->want_redraw = true;
            vo_wakeup(vo);
        }
        return true;
    case VOCTRL_PERFORMANCE_DATA:
        *(struct voctrl_performance_data *)data = gl_video_perfdata(p->renderer);
        return true;
    }

    int events = 0;
    int r = mpgl_control(p->glctx, &events, request, data);
    if (events & VO_EVENT_ICC_PROFILE_CHANGED) {
        get_and_update_icc_profile(p);
        vo->want_redraw = true;
    }
    if (events & VO_EVENT_AMBIENT_LIGHTING_CHANGED) {
        get_and_update_ambient_lighting(p);
        vo->want_redraw = true;
    }
    events |= p->events;
    p->events = 0;
    if (events & VO_EVENT_RESIZE)
        resize(p);
    if (events & VO_EVENT_EXPOSE)
        vo->want_redraw = true;
    vo_event(vo, events);

    return r;
}

static void wakeup(struct vo *vo)
{
    struct gl_priv *p = vo->priv;
    if (p->glctx && p->glctx->driver->wakeup)
        p->glctx->driver->wakeup(p->glctx);
}

static void wait_events(struct vo *vo, int64_t until_time_us)
{
    struct gl_priv *p = vo->priv;
    if (p->glctx->driver->wait_events) {
        p->glctx->driver->wait_events(p->glctx, until_time_us);
    } else {
        vo_wait_default(vo, until_time_us);
    }
}

static void uninit(struct vo *vo)
{
    struct gl_priv *p = vo->priv;

    gl_video_uninit(p->renderer);
    gl_hwdec_uninit(p->hwdec);
    if (vo->hwdec_devs) {
        hwdec_devices_set_loader(vo->hwdec_devs, NULL, NULL);
        hwdec_devices_destroy(vo->hwdec_devs);
    }
    mpgl_uninit(p->glctx);
}

static int preinit(struct vo *vo)
{
    struct gl_priv *p = vo->priv;
    p->vo = vo;
    p->log = vo->log;

    int vo_flags = 0;

    if (p->renderer_opts->alpha_mode == 1)
        vo_flags |= VOFLAG_ALPHA;

    if (p->use_gl_debug)
        vo_flags |= VOFLAG_GL_DEBUG;

    if (p->es == 1)
        vo_flags |= VOFLAG_GLES;
    if (p->es == -1)
        vo_flags |= VOFLAG_NO_GLES;

    if (p->allow_sw)
        vo_flags |= VOFLAG_SW;

    if (p->allow_direct_composition)
        vo_flags |= VOFLAG_ANGLE_DCOMP;

    p->glctx = mpgl_init(vo, p->backend, vo_flags);
    if (!p->glctx)
        goto err_out;
    p->gl = p->glctx->gl;

    p->glctx->dwm_flush_opt = p->dwm_flush;

    if (p->gl->SwapInterval) {
        p->gl->SwapInterval(p->swap_interval);
    } else {
        MP_VERBOSE(vo, "swap_control extension missing.\n");
    }

    p->renderer = gl_video_init(p->gl, vo->log, vo->global);
    if (!p->renderer)
        goto err_out;
    gl_video_set_osd_source(p->renderer, vo->osd);
    gl_video_set_options(p->renderer, p->renderer_opts);
    gl_video_configure_queue(p->renderer, vo);

    get_and_update_icc_profile(p);

    vo->hwdec_devs = hwdec_devices_create();

    hwdec_devices_set_loader(vo->hwdec_devs, call_request_hwdec_api, vo);

    int hwdec = vo->opts->hwdec_preload_api;
    if (hwdec == HWDEC_NONE)
        hwdec = vo->global->opts->hwdec_api;
    if (hwdec != HWDEC_NONE) {
        p->hwdec = gl_hwdec_load_api(p->vo->log, p->gl, vo->global,
                                     vo->hwdec_devs, hwdec);
        gl_video_set_hwdec(p->renderer, p->hwdec);
    }

    p->original_opts = m_sub_options_copy(p, &opengl_conf, p);

    return 0;

err_out:
    uninit(vo);
    return -1;
}

#define OPT_BASE_STRUCT struct gl_priv
static const struct m_option options[] = {
    OPT_FLAG("glfinish", use_glFinish, 0),
    OPT_FLAG("waitvsync", waitvsync, 0),
    OPT_INT("swapinterval", swap_interval, 0, OPTDEF_INT(1)),
    OPT_CHOICE("dwmflush", dwm_flush, 0,
               ({"no", -1}, {"auto", 0}, {"windowed", 1}, {"yes", 2})),
    OPT_FLAG("dcomposition", allow_direct_composition, 0, OPTDEF_INT(1)),
    OPT_FLAG("debug", use_gl_debug, 0),
    OPT_STRING_VALIDATE("backend", backend, 0, mpgl_validate_backend_opt),
    OPT_FLAG("sw", allow_sw, 0),
    OPT_CHOICE("es", es, 0, ({"no", -1}, {"auto", 0}, {"yes", 1})),
    OPT_INTPAIR("check-pattern", opt_pattern, 0),
    OPT_INTRANGE("vsync-fences", opt_vsync_fences, 0, 0, NUM_VSYNC_FENCES),

    OPT_SUBSTRUCT("", renderer_opts, gl_video_conf, 0),
    {0},
};

#define CAPS VO_CAP_ROTATE90

const struct vo_driver video_out_opengl = {
    .description = "Extended OpenGL Renderer",
    .name = "opengl",
    .caps = CAPS,
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .wait_events = wait_events,
    .wakeup = wakeup,
    .uninit = uninit,
    .priv_size = sizeof(struct gl_priv),
    .options = options,
};

const struct vo_driver video_out_opengl_hq = {
    .description = "Extended OpenGL Renderer (high quality rendering preset)",
    .name = "opengl-hq",
    .caps = CAPS,
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .wait_events = wait_events,
    .wakeup = wakeup,
    .uninit = uninit,
    .priv_size = sizeof(struct gl_priv),
    .priv_defaults = &(const struct gl_priv){
        .renderer_opts = (struct gl_video_opts *)&gl_video_opts_hq_def,
    },
    .options = options,
};
