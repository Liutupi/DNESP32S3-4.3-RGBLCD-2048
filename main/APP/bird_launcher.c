/**
 ****************************************************************************************************
 * @file        bird_launcher.c
 * @brief       Slingshot physics game with trajectory preview for DNESP32S3
 *              Drag bird to aim, see predicted path, release to launch!
 ****************************************************************************************************
 */

#include "bird_launcher.h"
#include "menu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- layout ---- */
#define SLING_X         155
#define SLING_Y         375
#define GROUND_Y        430
#define BIRD_R          11
#define PIG_R           13
#define BLOCK_W         52
#define BLOCK_H         20
#define PHYSICS_MS      16
#define MAX_PULL        80
#define MAX_BIRDS       4
#define MAX_BLOCKS      24
#define MAX_PIGS        6
#define MAX_TRAIL       30

/* ---- physics tuning ---- */
#define LAUNCH_POWER    6.5f
#define GRAVITY         0.62f
#define DAMP_WALL       0.4f
#define DAMP_BLOCK      0.35f

/* ---- types ---- */
typedef struct {
    lv_obj_t *obj;
    bool alive;
    int  pts;
} bl_ent_t;

typedef enum { ST_IDLE, ST_DRAG, ST_FLY, ST_DONE } bl_state_t;

/* ---- globals ---- */
static lv_obj_t  *g_scr;
static lv_obj_t  *g_bird;
static lv_obj_t  *g_band_l, *g_band_r;
static lv_obj_t  *g_trail[MAX_TRAIL];
static lv_obj_t  *g_score_lbl, *g_birds_lbl, *g_msg_lbl;
static lv_timer_t *g_phys;
static lv_obj_t  *g_touch;

static bl_state_t  g_state;
static float g_bx, g_by, g_bvx, g_bvy;
static int   g_score, g_birds_left, g_pigs_alive;
static lv_coord_t g_drag_x, g_drag_y;
static bool  g_dragging;

static bl_ent_t g_blocks[MAX_BLOCKS];
static bl_ent_t g_pigs[MAX_PIGS];
static int   g_nblocks, g_npigs;

/* ---- helpers ---- */
static void menu_cb(lv_event_t *e) { (void)e; menu_go_back(); }

static bool overlap(lv_coord_t x1,lv_coord_t y1,lv_coord_t w1,lv_coord_t h1,
                    lv_coord_t x2,lv_coord_t y2,lv_coord_t w2,lv_coord_t h2)
{
    if(x1+w1<=x2||x2+w2<=x1) return false;
    if(y1+h1<=y2||y2+h2<=y1) return false;
    return true;
}

static void set_score(void)
{
    char b[32];
    snprintf(b,sizeof(b),"Score: %d",g_score);
    lv_label_set_text(g_score_lbl,b);
    snprintf(b,sizeof(b),"%d",g_birds_left);
    lv_label_set_text(g_birds_lbl,b);
}

static void kill_ent(bl_ent_t *e)
{
    if(!e->alive) return;
    e->alive=false;
    g_score+=e->pts;
    lv_obj_del(e->obj);
    e->obj=NULL;
}

/* ---- trajectory preview ---- */
static void clear_trail(void)
{
    for(int i=0;i<MAX_TRAIL;i++){
        if(g_trail[i]){ lv_obj_del(g_trail[i]); g_trail[i]=NULL; }
    }
}

static void draw_trail(void)
{
    clear_trail();
    float dx=BIRD_R*0.5f+SLING_X-g_drag_x;
    float dy=BIRD_R*0.5f+SLING_Y-g_drag_y;
    float d=sqrtf(dx*dx+dy*dy);
    if(d>MAX_PULL){ dx=dx/d*MAX_PULL; dy=dy/d*MAX_PULL; }
    float vx=dx*LAUNCH_POWER;
    float vy=dy*LAUNCH_POWER;
    float px=(float)g_drag_x;
    float py=(float)g_drag_y;
    for(int i=0;i<MAX_TRAIL;i++){
        px+=vx; py+=vy; vy+=GRAVITY;
        if(px<-20||px>820||py>GROUND_Y+20) break;
        if(i%3!=0) continue;
        int idx=i/3;
        g_trail[idx]=lv_obj_create(g_scr);
        lv_obj_set_size(g_trail[idx],4,4);
        lv_obj_set_pos(g_trail[idx],(int)px-2,(int)py-2);
        lv_obj_set_style_bg_color(g_trail[idx],lv_color_hex(0xFFFFFF),0);
        lv_obj_set_style_radius(g_trail[idx],LV_RADIUS_CIRCLE,0);
        lv_obj_set_style_border_width(g_trail[idx],0,0);
        lv_obj_set_style_bg_opa(g_trail[idx],LV_OPA_70,0);
    }
}

/* ---- physics ---- */
static void check_hits(void)
{
    int bx=(int)g_bx, by=(int)g_by, bs=BIRD_R*2;
    for(int i=0;i<g_nblocks;i++){
        if(!g_blocks[i].alive) continue;
        lv_coord_t px=lv_obj_get_x(g_blocks[i].obj);
        lv_coord_t py=lv_obj_get_y(g_blocks[i].obj);
        if(overlap(bx,by,bs,bs,px,py,BLOCK_W,BLOCK_H)){
            kill_ent(&g_blocks[i]);
            float nx=(float)(bx+bs/2-px-BLOCK_W/2);
            float ny=(float)(by+bs/2-py-BLOCK_H/2);
            float nl=sqrtf(nx*nx+ny*ny)+0.001f;
            g_bvx+=nx/nl*2.5f; g_bvy+=ny/nl*2.5f;
            g_bvx*=DAMP_BLOCK; g_bvy*=DAMP_BLOCK;
            break;
        }
    }
    for(int i=0;i<g_npigs;i++){
        if(!g_pigs[i].alive) continue;
        lv_coord_t px=lv_obj_get_x(g_pigs[i].obj);
        lv_coord_t py=lv_obj_get_y(g_pigs[i].obj);
        if(overlap(bx,by,bs,bs,px,py,PIG_R*2,PIG_R*2)){
            kill_ent(&g_pigs[i]);
            g_pigs_alive--;
            float nx=(float)(bx+bs/2-px-PIG_R);
            float ny=(float)(by+bs/2-py-PIG_R);
            float nl=sqrtf(nx*nx+ny*ny)+0.001f;
            g_bvx+=nx/nl*3.5f; g_bvy+=ny/nl*3.5f;
            g_bvx*=DAMP_BLOCK; g_bvy*=DAMP_BLOCK;
            break;
        }
    }
}

static void phys_tick(lv_timer_t *t)
{
    (void)t;
    g_bx+=g_bvx; g_by+=g_bvy; g_bvy+=GRAVITY;
    if(g_bx<BIRD_R){ g_bx=BIRD_R; g_bvx*=-DAMP_WALL; }
    if(g_bx>800-BIRD_R){ g_bx=800-BIRD_R; g_bvx*=-DAMP_WALL; }
    check_hits();
    lv_obj_set_pos(g_bird,(int)g_bx,(int)g_by);

    if(g_bx<-40||g_bx>840||g_by>GROUND_Y+40){
        lv_timer_del(g_phys); g_phys=NULL;
        lv_obj_add_flag(g_bird,LV_OBJ_FLAG_HIDDEN);
        g_state=ST_DONE;
        if(g_pigs_alive<=0){
            lv_label_set_text(g_msg_lbl,"You Win!");
            lv_obj_clear_flag(g_msg_lbl,LV_OBJ_FLAG_HIDDEN);
        }else if(g_birds_left<=0){
            lv_label_set_text(g_msg_lbl,"Game Over");
            lv_obj_clear_flag(g_msg_lbl,LV_OBJ_FLAG_HIDDEN);
        }else{
            /* next bird */
            g_bx=SLING_X; g_by=SLING_Y;
            g_bvx=0; g_bvy=0; g_state=ST_IDLE;
            lv_obj_set_pos(g_bird,SLING_X,SLING_Y);
            lv_obj_clear_flag(g_bird,LV_OBJ_FLAG_HIDDEN);
        }
        set_score();
    }
}

static void start_phys(void)
{
    if(g_phys) lv_timer_del(g_phys);
    g_phys=lv_timer_create(phys_tick,PHYSICS_MS,NULL);
    lv_timer_set_repeat_count(g_phys,-1);
}

static void launch(void)
{
    float dx=SLING_X-g_drag_x, dy=SLING_Y-g_drag_y;
    float d=sqrtf(dx*dx+dy*dy);
    if(d>MAX_PULL){ dx=dx/d*MAX_PULL; dy=dy/d*MAX_PULL; }
    g_bvx=dx*LAUNCH_POWER; g_bvy=dy*LAUNCH_POWER;
    g_bx=(float)g_drag_x; g_by=(float)g_drag_y;
    g_birds_left--; set_score();
    g_state=ST_FLY; g_dragging=false;
    clear_trail();
    lv_obj_add_flag(g_band_l,LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_band_r,LV_OBJ_FLAG_HIDDEN);
    start_phys();
}

static void update_bands(void)
{
    int fx=SLING_X-8, fy=SLING_Y-12;
    int dx=g_drag_x-fx, dy=g_drag_y-fy;
    int len=(int)sqrtf((float)(dx*dx+dy*dy));
    lv_obj_set_pos(g_band_l,fx,fy);
    lv_obj_set_size(g_band_l,3,len>0?len:1);

    fx=SLING_X+8; fy=SLING_Y-12;
    dx=g_drag_x-fx; dy=g_drag_y-fy;
    len=(int)sqrtf((float)(dx*dx+dy*dy));
    lv_obj_set_pos(g_band_r,fx,fy);
    lv_obj_set_size(g_band_r,3,len>0?len:1);
}

/* ---- touch ---- */
static void touch_cb(lv_event_t *e)
{
    lv_event_code_t c=lv_event_get_code(e);
    lv_indev_t *indev=lv_indev_get_act();
    lv_point_t pt;

    if(g_state==ST_DONE){
        if(c==LV_EVENT_RELEASED) bird_launcher_start();
        return;
    }
    if(g_state!=ST_IDLE&&g_state!=ST_DRAG) return;

    if(c==LV_EVENT_PRESSED){
        lv_indev_get_point(indev,&pt);
        int dx=pt.x-(int)g_bx, dy=pt.y-(int)g_by;
        if(dx*dx+dy*dy<2500){ g_state=ST_DRAG; g_dragging=true; }
    }else if(c==LV_EVENT_PRESSING&&g_dragging){
        lv_indev_get_point(indev,&pt);
        float dx=(float)(SLING_X-pt.x), dy=(float)(SLING_Y-pt.y);
        float d=sqrtf(dx*dx+dy*dy);
        if(d>MAX_PULL){ dx=dx/d*MAX_PULL; dy=dy/d*MAX_PULL; }
        g_drag_x=(lv_coord_t)(SLING_X-dx);
        g_drag_y=(lv_coord_t)(SLING_Y-dy);
        lv_obj_set_pos(g_bird,g_drag_x,g_drag_y);
        lv_obj_clear_flag(g_band_l,LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_band_r,LV_OBJ_FLAG_HIDDEN);
        update_bands();
        draw_trail();
    }else if(c==LV_EVENT_RELEASED&&g_dragging){
        launch();
    }
}

/* ---- build level ---- */
static lv_obj_t *mk_block(lv_obj_t *p,lv_coord_t x,lv_coord_t y,uint32_t color)
{
    lv_obj_t *o=lv_obj_create(p);
    lv_obj_set_size(o,BLOCK_W,BLOCK_H);
    lv_obj_set_pos(o,x,y);
    lv_obj_set_style_bg_color(o,lv_color_hex(color),0);
    lv_obj_set_style_radius(o,3,0);
    lv_obj_set_style_border_width(o,0,0);
    lv_obj_set_style_border_color(o,lv_color_hex(0x3D1C00),0);
    lv_obj_set_style_border_width(o,1,0);
    lv_obj_set_style_border_opa(o,LV_OPA_40,0);
    return o;
}

static lv_obj_t *mk_pig(lv_obj_t *p,lv_coord_t x,lv_coord_t y)
{
    lv_obj_t *o=lv_obj_create(p);
    lv_obj_set_size(o,PIG_R*2,PIG_R*2);
    lv_obj_set_pos(o,x,y);
    lv_obj_set_style_bg_color(o,lv_color_hex(0x4CAF50),0);
    lv_obj_set_style_radius(o,LV_RADIUS_CIRCLE,0);
    lv_obj_set_style_border_width(o,2,0);
    lv_obj_set_style_border_color(o,lv_color_hex(0x2E7D32),0);

    lv_obj_t *ey=lv_obj_create(o);
    lv_obj_set_size(ey,7,7);
    lv_obj_set_pos(ey,PIG_R-9,PIG_R-6);
    lv_obj_set_style_bg_color(ey,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_style_radius(ey,LV_RADIUS_CIRCLE,0);
    lv_obj_set_style_border_width(ey,0,0);

    lv_obj_t *pu=lv_obj_create(o);
    lv_obj_set_size(pu,4,3);
    lv_obj_set_pos(pu,PIG_R-2,PIG_R);
    lv_obj_set_style_bg_color(pu,lv_color_hex(0x388E3C),0);
    lv_obj_set_style_radius(pu,1,0);
    lv_obj_set_style_border_width(pu,0,0);

    return o;
}

static void build_level(void)
{
    int bx=530, by=GROUND_Y-BLOCK_H;
    g_nblocks=0; g_npigs=0;

    /* base: 5 blocks */
    for(int i=0;i<5&&g_nblocks<MAX_BLOCKS;i++)
        g_blocks[g_nblocks++]=(bl_ent_t){mk_block(g_scr,bx+i*BLOCK_W,by,0x8B4513),true,10};

    /* L2: 4 blocks */
    for(int i=0;i<4&&g_nblocks<MAX_BLOCKS;i++)
        g_blocks[g_nblocks++]=(bl_ent_t){mk_block(g_scr,bx+BLOCK_W/2+i*BLOCK_W,by-BLOCK_H,0xA0522D),true,10};

    /* L3: 3 blocks */
    for(int i=0;i<3&&g_nblocks<MAX_BLOCKS;i++)
        g_blocks[g_nblocks++]=(bl_ent_t){mk_block(g_scr,bx+BLOCK_W+i*BLOCK_W,by-BLOCK_H*2,0x6B3410),true,10};

    /* L4: 2 blocks */
    for(int i=0;i<2&&g_nblocks<MAX_BLOCKS;i++)
        g_blocks[g_nblocks++]=(bl_ent_t){mk_block(g_scr,bx+BLOCK_W+BLOCK_W/2+i*BLOCK_W,by-BLOCK_H*3,0x5D2E0C),true,10};

    /* pigs */
    g_pigs[g_npigs++]=(bl_ent_t){mk_pig(g_scr,bx+BLOCK_W+BLOCK_W/2-PIG_R,by-BLOCK_H*4),true,50};
    g_pigs[g_npigs++]=(bl_ent_t){mk_pig(g_scr,bx+BLOCK_W*2-BLOCK_W/4-PIG_R,by-BLOCK_H*2-PIG_R*2+2),true,50};
    g_pigs_alive=g_npigs;
}

/* ---- entry ---- */
void bird_launcher_start(void)
{
    g_scr=lv_obj_create(NULL);
    lv_scr_load(g_scr);
    lv_obj_set_style_bg_color(g_scr,lv_color_hex(0x1B2838),0);

    g_score=0; g_birds_left=MAX_BIRDS; g_phys=NULL; g_dragging=false;
    g_bx=SLING_X; g_by=SLING_Y; g_bvx=0; g_bvy=0;
    memset(g_blocks,0,sizeof(g_blocks));
    memset(g_pigs,0,sizeof(g_pigs));
    memset(g_trail,0,sizeof(g_trail));

    /* ground */
    lv_obj_t *gr=lv_obj_create(g_scr);
    lv_obj_set_size(gr,800,50);
    lv_obj_set_pos(gr,0,GROUND_Y);
    lv_obj_set_style_bg_color(gr,lv_color_hex(0x3E7A2E),0);
    lv_obj_set_style_radius(gr,0,0);
    lv_obj_set_style_border_width(gr,0,0);
    lv_obj_t *gt=lv_obj_create(g_scr);
    lv_obj_set_size(gt,800,6);
    lv_obj_set_pos(gt,0,GROUND_Y);
    lv_obj_set_style_bg_color(gt,lv_color_hex(0x2E5B1E),0);
    lv_obj_set_style_radius(gt,0,0);
    lv_obj_set_style_border_width(gt,0,0);

    /* slingshot fork */
    lv_obj_t *sf=lv_obj_create(g_scr);
    lv_obj_set_size(sf,8,55);
    lv_obj_set_pos(sf,SLING_X-4,SLING_Y-8);
    lv_obj_set_style_bg_color(sf,lv_color_hex(0x5D3A1A),0);
    lv_obj_set_style_radius(sf,3,0);
    lv_obj_set_style_border_width(sf,0,0);
    lv_obj_t *sl=lv_obj_create(g_scr);
    lv_obj_set_size(sl,18,5);
    lv_obj_set_pos(sl,SLING_X-9,SLING_Y-13);
    lv_obj_set_style_bg_color(sl,lv_color_hex(0x5D3A1A),0);
    lv_obj_set_style_radius(sl,2,0);
    lv_obj_set_style_border_width(sl,0,0);

    /* elastic bands (hidden until drag) */
    g_band_l=lv_obj_create(g_scr);
    lv_obj_set_size(g_band_l,3,8);
    lv_obj_set_pos(g_band_l,SLING_X-8,SLING_Y-12);
    lv_obj_set_style_bg_color(g_band_l,lv_color_hex(0x3D1C00),0);
    lv_obj_set_style_radius(g_band_l,1,0);
    lv_obj_set_style_border_width(g_band_l,0,0);
    lv_obj_add_flag(g_band_l,LV_OBJ_FLAG_HIDDEN);

    g_band_r=lv_obj_create(g_scr);
    lv_obj_set_size(g_band_r,3,8);
    lv_obj_set_pos(g_band_r,SLING_X+8,SLING_Y-12);
    lv_obj_set_style_bg_color(g_band_r,lv_color_hex(0x3D1C00),0);
    lv_obj_set_style_radius(g_band_r,1,0);
    lv_obj_set_style_border_width(g_band_r,0,0);
    lv_obj_add_flag(g_band_r,LV_OBJ_FLAG_HIDDEN);

    /* bird */
    g_bird=lv_obj_create(g_scr);
    lv_obj_set_size(g_bird,BIRD_R*2,BIRD_R*2);
    lv_obj_set_pos(g_bird,SLING_X-BIRD_R,SLING_Y-BIRD_R);
    lv_obj_set_style_bg_color(g_bird,lv_color_hex(0xE74C3C),0);
    lv_obj_set_style_radius(g_bird,LV_RADIUS_CIRCLE,0);
    lv_obj_set_style_border_width(g_bird,0,0);
    /* eyes */
    lv_obj_t *eb=lv_obj_create(g_bird);
    lv_obj_set_size(eb,5,5);
    lv_obj_set_pos(eb,BIRD_R-8,BIRD_R-5);
    lv_obj_set_style_bg_color(eb,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_style_radius(eb,LV_RADIUS_CIRCLE,0);
    lv_obj_set_style_border_width(eb,0,0);
    lv_obj_t *eb2=lv_obj_create(g_bird);
    lv_obj_set_size(eb2,5,5);
    lv_obj_set_pos(eb2,BIRD_R,BIRD_R-5);
    lv_obj_set_style_bg_color(eb2,lv_color_hex(0xFFFFFF),0);
    lv_obj_set_style_radius(eb2,LV_RADIUS_CIRCLE,0);
    lv_obj_set_style_border_width(eb2,0,0);
    /* eyebrows (angry) */
    lv_obj_t *brow=lv_obj_create(g_bird);
    lv_obj_set_size(brow,9,2);
    lv_obj_set_pos(brow,BIRD_R-10,BIRD_R-9);
    lv_obj_set_style_bg_color(brow,lv_color_hex(0x922B21),0);
    lv_obj_set_style_radius(brow,1,0);
    lv_obj_set_style_border_width(brow,0,0);
    lv_obj_t *brow2=lv_obj_create(g_bird);
    lv_obj_set_size(brow2,9,2);
    lv_obj_set_pos(brow2,BIRD_R+1,BIRD_R-9);
    lv_obj_set_style_bg_color(brow2,lv_color_hex(0x922B21),0);
    lv_obj_set_style_radius(brow2,1,0);
    lv_obj_set_style_border_width(brow2,0,0);

    build_level();

    /* score HUD */
    lv_obj_t *hud_bg=lv_obj_create(g_scr);
    lv_obj_set_size(hud_bg,200,44);
    lv_obj_set_pos(hud_bg,6,8);
    lv_obj_set_style_bg_color(hud_bg,lv_color_hex(0x0D1B2A),0);
    lv_obj_set_style_bg_opa(hud_bg,LV_OPA_70,0);
    lv_obj_set_style_radius(hud_bg,8,0);
    lv_obj_set_style_border_width(hud_bg,0,0);

    g_score_lbl=lv_label_create(g_scr);
    lv_obj_set_style_text_font(g_score_lbl,&lv_font_montserrat_16,0);
    lv_obj_set_style_text_color(g_score_lbl,lv_color_hex(0xECF0F1),0);
    lv_obj_set_pos(g_score_lbl,14,12);

    /* bird icons */
    g_birds_lbl=lv_label_create(g_scr);
    lv_obj_set_style_text_font(g_birds_lbl,&lv_font_montserrat_16,0);
    lv_obj_set_style_text_color(g_birds_lbl,lv_color_hex(0xE74C3C),0);
    lv_obj_set_pos(g_birds_lbl,14,32);

    set_score();

    /* message */
    g_msg_lbl=lv_label_create(g_scr);
    lv_obj_set_style_text_font(g_msg_lbl,&lv_font_montserrat_28,0);
    lv_obj_set_style_text_color(g_msg_lbl,lv_color_hex(0xF1C40F),0);
    lv_obj_center(g_msg_lbl);
    lv_obj_add_flag(g_msg_lbl,LV_OBJ_FLAG_HIDDEN);

    /* touch layer - MUST be created before menu button so button is on top */
    g_touch=lv_obj_create(g_scr);
    lv_obj_set_size(g_touch,800,480);
    lv_obj_set_pos(g_touch,0,0);
    lv_obj_set_style_bg_opa(g_touch,LV_OPA_0,0);
    lv_obj_set_style_border_width(g_touch,0,0);
    lv_obj_clear_flag(g_touch,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_touch,touch_cb,LV_EVENT_PRESSED,NULL);
    lv_obj_add_event_cb(g_touch,touch_cb,LV_EVENT_PRESSING,NULL);
    lv_obj_add_event_cb(g_touch,touch_cb,LV_EVENT_RELEASED,NULL);

    /* menu button */
    lv_obj_t *mb=lv_btn_create(g_scr);
    lv_obj_set_size(mb,90,32);
    lv_obj_set_pos(mb,700,14);
    lv_obj_set_style_bg_color(mb,lv_color_hex(0x1B4F72),0);
    lv_obj_set_style_radius(mb,6,0);
    lv_obj_add_event_cb(mb,menu_cb,LV_EVENT_RELEASED,NULL);
    lv_obj_t *mbl=lv_label_create(mb);
    lv_label_set_text(mbl,"Menu");
    lv_obj_set_style_text_font(mbl,&lv_font_montserrat_14,0);
    lv_obj_set_style_text_color(mbl,lv_color_hex(0xECF0F1),0);
    lv_obj_center(mbl);
    lv_obj_add_event_cb(mbl,menu_cb,LV_EVENT_RELEASED,NULL);

    g_state=ST_IDLE;
}
