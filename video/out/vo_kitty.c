/*
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

#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <libswscale/swscale.h>

#include <unistd.h>
#include <sys/mman.h>

#include "config.h"
#include "options/m_config.h"
#include "osdep/terminal.h"
#include "sub/osd.h"
#include "vo.h"
#include "video/mp_image.h"
#include "video/sws_utils.h"

#define TERMINAL_FALLBACK_PX_WIDTH  320
#define TERMINAL_FALLBACK_PX_HEIGHT 240
#define SHM_NAME "/kitty_img"
#define SHM_NAME_BASE64 "a2l0dHlfaW1n"

#define ESC_GOTOXY                  "\033[%d;%df"
#define ESC_HIDE_CURSOR             "\033[?25l"
#define ESC_RESTORE_CURSOR          "\033[?25h"
#define ESC_CLEAR_SCREEN            "\033[2J"

struct priv {
    // User specified options
    int opt_width;
    int opt_height;
    int opt_top;
    int opt_left;
    int opt_clear;

    // Internal data
    int             fmt;
    int             mmap_fd;
    uint8_t        *buffer;
    bool            skip_frame_draw;

    int left, top;  // image origin cell (1 based)
    int width, height;  // actual image px size - always reflects dst_rect.
    int num_cols, num_rows;  // terminal size in cells
    int canvas_ok;  // whether canvas vo->dwidth and vo->dheight are positive

    struct mp_rect src_rect;
    struct mp_rect dst_rect;
    struct mp_osd_res osd;
    struct mp_image *frame;
    struct mp_sws_context *sws;
};

static const unsigned int depth = 4;

static void dealloc_buffers(struct vo *vo)
{
    struct priv *priv = vo->priv;

    if (priv->buffer) {
        munmap(priv->buffer, priv->width * priv->height * depth);
        priv->buffer = NULL;
    }

    if (priv->mmap_fd != -1) {
        close(priv->mmap_fd);
        priv->mmap_fd = -1;
    }

    if (priv->frame) {
        talloc_free(priv->frame);
        priv->frame = NULL;
    }
}


static void update_canvas_dimensions(struct vo *vo)
{
    struct priv *priv   = vo->priv;
    int num_rows        = 0;
    int num_cols        = 0;
    int total_px_width  = 0;
    int total_px_height = 0;

    terminal_get_size2(&num_rows, &num_cols, &total_px_width, &total_px_height);

    if (priv->opt_width > 0) {
        total_px_width = priv->opt_width;
    } else if (total_px_width <= 0) {
       // ioctl failed to read terminal width
       total_px_width = TERMINAL_FALLBACK_PX_WIDTH;
    }

    if (priv->opt_height > 0) {
        total_px_height = priv->opt_height;
    } else if (total_px_height <= 0) {
        total_px_height = TERMINAL_FALLBACK_PX_HEIGHT;
    }

    vo->dheight = total_px_height;
    vo->dwidth  = total_px_width;

    priv->num_rows = num_rows;
    priv->num_cols = num_cols;

    priv->canvas_ok = vo->dwidth > 0 && vo->dheight > 0;
}

static void set_output_parameters(struct vo *vo)
{
    struct priv *priv = vo->priv;

    vo_get_src_dst_rects(vo, &priv->src_rect, &priv->dst_rect, &priv->osd);

    priv->width  = priv->dst_rect.x1 - priv->dst_rect.x0;
    priv->height = priv->dst_rect.y1 - priv->dst_rect.y0;

    priv->top  = (priv->opt_top  > 0) ?  priv->opt_top :
                  priv->num_rows * priv->dst_rect.y0 / vo->dheight + 1;
    priv->left = (priv->opt_left > 0) ?  priv->opt_left :
                  priv->num_cols * priv->dst_rect.x0 / vo->dwidth  + 1;
}

static int update_params(struct vo *vo, struct mp_image_params *params)
{
    struct priv *priv = vo->priv;
    priv->sws->src = *params;
    priv->sws->src.w = mp_rect_w(priv->src_rect);
    priv->sws->src.h = mp_rect_h(priv->src_rect);
    priv->sws->dst = (struct mp_image_params) {
        .imgfmt = priv->fmt,
        .w = priv->width,
        .h = priv->height,
        .p_w = 1,
        .p_h = 1,
    };

    dealloc_buffers(vo);

    priv->frame = mp_image_alloc(IMGFMT_RGBA, priv->width, priv->height);
    if (!priv->frame)
        return -1;

    if (mp_sws_reinit(priv->sws) < 0)
        return -1;

    return 0;
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct priv *priv = vo->priv;
    int ret = 0;
    update_canvas_dimensions(vo);

    if (priv->canvas_ok) {  // if too small - succeed but skip the rendering
        set_output_parameters(vo);
        ret = update_params(vo, params);
    }

    printf(ESC_CLEAR_SCREEN);
    vo->want_redraw = true;
    return ret;
}


static void draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *priv = vo->priv;
    struct mp_image *mpi = NULL;
    int  prev_height = vo->dheight;
    int  prev_width  = vo->dwidth;
    bool resized     = false;

    update_canvas_dimensions(vo);
    if (!priv->canvas_ok)
        return;

    if (prev_width != vo->dwidth || prev_height != vo->dheight) {
        set_output_parameters(vo);
        // Not checking for vo->config_ok because draw_frame is never called
        // with a failed reconfig.
        update_params(vo, vo->params);

        printf(ESC_CLEAR_SCREEN);
        resized = true;
    }

    if (frame->repeat && !frame->redraw && !resized) {
        // Frame is repeated, and no need to update OSD either
        priv->skip_frame_draw = true;
        return;
    } else {
        // Either frame is new, or OSD has to be redrawn
        priv->skip_frame_draw = false;
    }

    // Normal case where we have to draw the frame and the image is not NULL
    if (frame->current) {
        mpi = mp_image_new_ref(frame->current);
        struct mp_rect src_rc = priv->src_rect;
        src_rc.x0 = MP_ALIGN_DOWN(src_rc.x0, mpi->fmt.align_x);
        src_rc.y0 = MP_ALIGN_DOWN(src_rc.y0, mpi->fmt.align_y);
        mp_image_crop_rc(mpi, src_rc);

        // scale/pan to our dest rect
        mp_sws_scale(priv->sws, priv->frame, mpi);
    } else {
        // Image is NULL, so need to clear image and draw OSD
        mp_image_clear(priv->frame, 0, 0, priv->width, priv->height);
    }

    struct mp_osd_res dim = {
        .w = priv->width,
        .h = priv->height
    };
    osd_draw_on_image(vo->osd, dim, mpi ? mpi->pts : 0, 0, priv->frame);


    priv->mmap_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (priv->mmap_fd == -1) {
        MP_WARN(vo, "Failed to create SHM object");
        return;
    }

    int size = priv->width * priv->height * depth;

    if (ftruncate(priv->mmap_fd, size) == -1) {
        MP_WARN(vo, "Failed to truncate SHM object");
        shm_unlink(SHM_NAME);
        close(priv->mmap_fd);
        return;
    }

    priv->buffer = mmap(NULL, size,
      PROT_READ | PROT_WRITE, MAP_SHARED, priv->mmap_fd, 0);
    if (priv->buffer == MAP_FAILED) {
        MP_WARN(vo, "Failed to mmap SHM object");
        shm_unlink(SHM_NAME);
        close(priv->mmap_fd);
        return;
    }

    memcpy_pic(priv->buffer, priv->frame->planes[0], priv->width * depth,
               priv->height, priv->width * depth, priv->frame->stride[0]);

    if (mpi)
        talloc_free(mpi);
}

static void flip_page(struct vo *vo)
{
    struct priv* priv = vo->priv;
    if (!priv->canvas_ok)
        return;

    if (priv->skip_frame_draw)
        return;

    printf(ESC_GOTOXY, priv->top, priv->left);
    printf("\033_Ga=T,f=32,t=s,s=%u,v=%u;%s\033\\", priv->width, priv->height, SHM_NAME_BASE64);
    fflush(stdout);

    // SHM_NAME will be unlinked by kitty, so not unlinking in mpv
    munmap(priv->buffer, priv->width * priv->height * depth);
    priv->buffer = NULL;
    close(priv->mmap_fd);
    priv->mmap_fd = -1;
}

static int preinit(struct vo *vo)
{
    struct priv *priv = vo->priv;
    printf(ESC_HIDE_CURSOR);
    priv->mmap_fd = -1;

    priv->sws = mp_sws_alloc(vo);
    priv->sws->log = vo->log;
    mp_sws_enable_cmdline_opts(priv->sws, vo->global);

    return 0;
}

static int query_format(struct vo *vo, int format)
{
    struct priv *p = vo->priv;
    if (mp_sws_supports_formats(p->sws, IMGFMT_RGB0, format)){
        p->fmt = format;
        return 1;
    }
    return 0;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    if (request == VOCTRL_SET_PANSCAN)
        return (vo->config_ok && !reconfig(vo, vo->params)) ? VO_TRUE : VO_FALSE;
    return VO_NOTIMPL;
}

static void uninit(struct vo *vo)
{
    struct priv *priv = vo->priv;

    printf(ESC_RESTORE_CURSOR);

    if (priv->opt_clear) {
        printf(ESC_CLEAR_SCREEN);
        printf(ESC_GOTOXY, 1, 1);
    }
    fflush(stdout);

    dealloc_buffers(vo);
}

#define OPT_BASE_STRUCT struct priv

const struct vo_driver video_out_kitty = {
    .name = "kitty",
    .description = "terminal graphics using kitty protocol",
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
    .priv_defaults = &(const struct priv) {
        .opt_width = 0,
        .opt_height = 0,
        .opt_top = 0,
        .opt_left = 0,
        .opt_clear = 1,
    },
    .options = (const m_option_t[]) {
        {"width", OPT_INT(opt_width)},
        {"height", OPT_INT(opt_height)},
        {"top", OPT_INT(opt_top)},
        {"left", OPT_INT(opt_left)},
        {"exit-clear", OPT_FLAG(opt_clear), },
        {0}
    },
    .options_prefix = "vo-kitty",
};
