#include "splash.h"
#include "logo_img.h"
#include "lvgl.h"

static void (*s_done_cb)(void) = NULL;

static void opa_anim_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void on_fade_out_done(lv_anim_t *a)
{
    lv_obj_del((lv_obj_t *)a->var);
    if (s_done_cb) {
        s_done_cb();
        s_done_cb = NULL;
    }
}

static void on_fade_in_done(lv_anim_t *a)
{
    lv_anim_t out;
    lv_anim_init(&out);
    lv_anim_set_var(&out, a->var);
    lv_anim_set_exec_cb(&out, opa_anim_cb);
    lv_anim_set_values(&out, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&out, 1000);
    lv_anim_set_delay(&out, 1000);
    lv_anim_set_ready_cb(&out, on_fade_out_done);
    lv_anim_start(&out);
}

void splash_show(void (*done_cb)(void))
{
    s_done_cb = done_cb;

    lv_obj_t *img = lv_img_create(lv_scr_act());
    lv_img_set_src(img, &logo_img);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_opa(img, LV_OPA_TRANSP, 0);

    lv_anim_t in;
    lv_anim_init(&in);
    lv_anim_set_var(&in, img);
    lv_anim_set_exec_cb(&in, opa_anim_cb);
    lv_anim_set_values(&in, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&in, 1000);
    lv_anim_set_ready_cb(&in, on_fade_in_done);
    lv_anim_start(&in);
}
