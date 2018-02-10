/*
 * (C) notaz, 2012,2014,2015
 *
 * This work is licensed under the terms of the GNU GPLv2 or later.
 * See the COPYING file in the top-level directory.
 */

#define _GNU_SOURCE 1 // strcasestr
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef __MACH__
#include <unistd.h>
#include <sys/syscall.h>
#endif

#include "../libpcsxcore/misc.h"
#include "../libpcsxcore/psxcounters.h"
#include "../libpcsxcore/psxmem_map.h"
#include "../libpcsxcore/cdrom.h"
#include "../libpcsxcore/cdriso.h"
#include "../libpcsxcore/cheat.h"
#include "../plugins/dfsound/out.h"
#include "../plugins/dfsound/spu_config.h"
#include "../plugins/dfinput/externals.h"
#include "cspace.h"
#include "main.h"
#include "plugin.h"


#include "../glsym/glsym.h"
#include "../glsym/glsym_gl.h"
#include "../glsm/glsm.h"
#include "../plugins/peopsxgl/gl_ext.h"
#include "../plugins/peopsxgl/externals.h"
//#include "plugin_lib.h"
#include "arm_features.h"
#include "revision.h"
#include "libretro.h"



#define RARCH_GL_FRAMEBUFFER GL_FRAMEBUFFER
#define RARCH_GL_FRAMEBUFFER_COMPLETE GL_FRAMEBUFFER_COMPLETE
#define RARCH_GL_COLOR_ATTACHMENT0 GL_COLOR_ATTACHMENT0

#define PORTS_NUMBER 8

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define ISHEXDEC ((buf[cursor]>='0') && (buf[cursor]<='9')) || ((buf[cursor]>='a') && (buf[cursor]<='f')) || ((buf[cursor]>='A') && (buf[cursor]<='F'))

//hack to prevent retroarch freezing when reseting in the menu but not while running with the hot key
static int rebootemu = 0;

static struct retro_hw_render_callback hw_render;

static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_environment_t environ_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static struct retro_rumble_interface rumble;

static void *vout_buf;
static void * vout_buf_ptr;
static int vout_width = 640;
static int vout_height = 480;
static int vout_doffs_old, vout_fb_dirty;
static bool vout_can_dupe;
static bool duping_enable;
static bool found_bios;

static int plugins_opened;
static int is_pal_mode;

/* memory card data */
extern char Mcd1Data[MCD_SIZE];
extern char Mcd2Data[MCD_SIZE];
extern char McdDisable[2];

/* PCSX ReARMed core calls and stuff */
int in_type[8] =  { PSE_PAD_TYPE_NONE, PSE_PAD_TYPE_NONE,
                  PSE_PAD_TYPE_NONE, PSE_PAD_TYPE_NONE,
                  PSE_PAD_TYPE_NONE, PSE_PAD_TYPE_NONE,
                  PSE_PAD_TYPE_NONE, PSE_PAD_TYPE_NONE };
int in_analog_left[8][2] = {{ 127, 127 },{ 127, 127 },{ 127, 127 },{ 127, 127 },{ 127, 127 },{ 127, 127 },{ 127, 127 },{ 127, 127 }};
int in_analog_right[8][2] = {{ 127, 127 },{ 127, 127 },{ 127, 127 },{ 127, 127 },{ 127, 127 },{ 127, 127 },{ 127, 127 },{ 127, 127 }};
unsigned short in_keystate[PORTS_NUMBER];
int multitap1 = 0;
int multitap2 = 0;
int in_enable_vibration = 1;

/* PSX max resolution is 640x512, but with enhancement it's 1024x512 */
#define VOUT_MAX_WIDTH 1024
#define VOUT_MAX_HEIGHT 512

//Dummy functions
bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info){return false;}
void retro_unload_game(void){}
static int vout_open(void){
fprintf(stderr, "vout_open\n");
return 0;
}
static void vout_close(void){
fprintf(stderr, "vout_close\n");
}
static int snd_init(void){return 0;}
static void snd_finish(void){}
static int snd_busy(void){return 0;}

static void init_memcard(char *mcd_data)
{
	unsigned off = 0;
	unsigned i;

	memset(mcd_data, 0, MCD_SIZE);

	mcd_data[off++] = 'M';
	mcd_data[off++] = 'C';
	off += 0x7d;
	mcd_data[off++] = 0x0e;

	for (i = 0; i < 15; i++) {
		mcd_data[off++] = 0xa0;
		off += 0x07;
		mcd_data[off++] = 0xff;
		mcd_data[off++] = 0xff;
		off += 0x75;
		mcd_data[off++] = 0xa0;
	}

	for (i = 0; i < 20; i++) {
		mcd_data[off++] = 0xff;
		mcd_data[off++] = 0xff;
		mcd_data[off++] = 0xff;
		mcd_data[off++] = 0xff;
		off += 0x04;
		mcd_data[off++] = 0xff;
		mcd_data[off++] = 0xff;
		off += 0x76;
	}
}

static void vout_set_mode(int w, int h, int raw_w, int raw_h, int bpp)
{
	vout_width = w;
	vout_height = h;

  struct retro_framebuffer fb = {0};

  fb.width           = vout_width;
  fb.height          = vout_height;
  fb.access_flags    = RETRO_MEMORY_ACCESS_WRITE;

  vout_buf_ptr = vout_buf;

  if (environ_cb(RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER, &fb) && fb.format == RETRO_PIXEL_FORMAT_RGB565)
  {
     vout_buf_ptr  = (uint16_t*)fb.data;
  }
  

    // vout_buf_ptr = hw_render.get_current_framebuffer();
}

#ifndef FRONTEND_SUPPORTS_RGB565
static void convert(void *buf, size_t bytes)
{
	unsigned int i, v, *p = buf;

	for (i = 0; i < bytes / 4; i++) {
		v = p[i];
		p[i] = (v & 0x001f001f) | ((v >> 1) & 0x7fe07fe0);
	}
}
#endif

static void vout_flip(const void *vram, int stride, int bgr24, int w, int h)
{
	unsigned short *dest = vout_buf_ptr;
	const unsigned short *src = vram;
	int dstride = vout_width, h1 = h;
	int doffs;

	if (vram == NULL) {
		// blanking
		memset(vout_buf_ptr, 0, dstride * h * 2);
		goto out;
	}

	doffs = (vout_height - h) * dstride;
	doffs += (dstride - w) / 2 & ~1;
	if (doffs != vout_doffs_old) {
		// clear borders
		memset(vout_buf_ptr, 0, dstride * h * 2);
		vout_doffs_old = doffs;
	}
	dest += doffs;

	if (bgr24)
	{
		// XXX: could we switch to RETRO_PIXEL_FORMAT_XRGB8888 here?
		for (; h1-- > 0; dest += dstride, src += stride)
		{
			bgr888_to_rgb565(dest, src, w * 3);
		}
	}
	else
	{
		for (; h1-- > 0; dest += dstride, src += stride)
		{
			bgr555_to_rgb565(dest, src, w * 2);
		}
	}

out:
#ifndef FRONTEND_SUPPORTS_RGB565
	convert(vout_buf_ptr, vout_width * vout_height * 2);
#endif
	vout_fb_dirty = 1;
	pl_rearmed_cbs.flip_cnt++;
}

static void *pl_mmap(unsigned int size)
{
	return psxMap(0, size, 0, MAP_TAG_VRAM);
}

static void pl_munmap(void *ptr, unsigned int size)
{
	psxUnmap(ptr, size, MAP_TAG_VRAM);
}

struct rearmed_cbs pl_rearmed_cbs = {
	.pl_vout_open = vout_open,
	.pl_vout_set_mode = vout_set_mode,
	.pl_vout_flip = vout_flip,
	.pl_vout_close = vout_close,
	.mmap = pl_mmap,
	.munmap = pl_munmap,
	/* from psxcounters */
	.gpu_hcnt = &hSyncCount,
	.gpu_frame_count = &frame_counter,
};

void pl_frame_limit(void)
{
	/* called once per frame, make psxCpu->Execute() above return */
	stop = 1;
}

void pl_timing_prepare(int is_pal)
{
	is_pal_mode = is_pal;
}

void plat_trigger_vibrate(int pad, int low, int high)
{
	if(in_enable_vibration)
	{
    	rumble.set_rumble_state(pad, RETRO_RUMBLE_STRONG, high << 8);
    	rumble.set_rumble_state(pad, RETRO_RUMBLE_WEAK, low ? 0xffff : 0x0);
    }
}

void pl_update_gun(int *xn, int *yn, int *xres, int *yres, int *in)
{
}

/* sound calls */
static void snd_feed(void *buf, int bytes)
{
	if (audio_batch_cb != NULL)
		audio_batch_cb(buf, bytes / 4);
}

void out_register_libretro(struct out_driver *drv)
{
	drv->name = "libretro";
	drv->init = snd_init;
	drv->finish = snd_finish;
	drv->busy = snd_busy;
	drv->feed = snd_feed;
}

static void context_reset(void);
static void context_destroy(void);

static bool retro_init_hw_context(void)
{
   hw_render.context_type = RETRO_HW_CONTEXT_OPENGL;
   hw_render.context_reset = context_reset;
   hw_render.context_destroy = context_destroy;
   hw_render.depth = true;
   hw_render.bottom_left_origin = true;

   if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
      return false;

   return true;
}

/* libretro */
void retro_set_environment(retro_environment_t cb)
{

   static const struct retro_variable vars[] = {
      { "pcsx_rearmed_frameskip", "Frameskip; 0|1|2|3" },
      { "pcsx_rearmed_region", "Region; Auto|NTSC|PAL" },
      { "pcsx_rearmed_pad1type", "Pad 1 Type; default|none|standard|analog|negcon" },
      { "pcsx_rearmed_pad2type", "Pad 2 Type; default|none|standard|analog|negcon" },
      { "pcsx_rearmed_pad3type", "Pad 3 Type; default|none|standard|analog|negcon" },
      { "pcsx_rearmed_pad4type", "Pad 4 Type; default|none|standard|analog|negcon" },
      { "pcsx_rearmed_pad5type", "Pad 5 Type; default|none|standard|analog|negcon" },
      { "pcsx_rearmed_pad6type", "Pad 6 Type; default|none|standard|analog|negcon" },
      { "pcsx_rearmed_pad7type", "Pad 7 Type; default|none|standard|analog|negcon" },
      { "pcsx_rearmed_pad8type", "Pad 8 Type; default|none|standard|analog|negcon" },
      { "pcsx_rearmed_multitap1", "Multitap 1; auto|disabled|enabled" },
      { "pcsx_rearmed_multitap2", "Multitap 2; auto|disabled|enabled" },
      { "pcsx_rearmed_vibration", "Enable Vibration; enabled|disabled" },
      { "pcsx_rearmed_dithering", "Enable Dithering; enabled|disabled" },
#ifndef DRC_DISABLE
      { "pcsx_rearmed_drc", "Dynamic recompiler; enabled|disabled" },
#endif
#ifdef __ARM_NEON__
      { "pcsx_rearmed_neon_interlace_enable", "Enable interlacing mode(s); disabled|enabled" },
      { "pcsx_rearmed_neon_enhancement_enable", "Enhanced resolution (slow); disabled|enabled" },
      { "pcsx_rearmed_neon_enhancement_no_main", "Enhanced resolution speed hack; disabled|enabled" },
#endif
      { "pcsx_rearmed_duping_enable", "Frame duping; enabled|disabled" },
      { "pcsx_rearmed_show_bios_bootlogo", "Show Bios Bootlogo(Breaks some games); disabled|enabled" },
      { "pcsx_rearmed_spu_reverb", "Sound: Reverb; enabled|disabled" },
      { "pcsx_rearmed_spu_interpolation", "Sound: Interpolation; simple|gaussian|cubic|off" },
      { "pcsx_rearmed_pe2_fix", "Parasite Eve 2/Vandal Hearts 1/2 Fix; disabled|enabled" },
      { "pcsx_rearmed_inuyasha_fix", "InuYasha Sengoku Battle Fix; disabled|enabled" },
      { NULL, NULL },
   };

   environ_cb = cb;

   cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
}

static const unsigned int gpu_ctl_def[] = {
	0x00000000, 0x01000000, 0x03000000, 0x04000000,
	0x05000800, 0x06c60260, 0x0703fc10, 0x08000027,
};

static const unsigned int gpu_data_def[] = {
	0xe100360b, 0xe2000000, 0xe3000800, 0xe4077e7f,
	0xe5001000, 0xe6000000,
	0x02000000, 0x00000000, 0x01ff03ff,
};

static void fake_bios_gpu_setup(void)
{
	int i;

	for (i = 0; i < sizeof(gpu_ctl_def) / sizeof(gpu_ctl_def[0]); i++)
		GPU_writeStatus(gpu_ctl_def[i]);

	for (i = 0; i < sizeof(gpu_data_def) / sizeof(gpu_data_def[0]); i++)
		GPU_writeData(gpu_data_def[i]);
}

static bool fb_ready = false;
static bool init_program_now = true;

static void context_reset(void)
{
   printf("context_reset.\n");
   glsm_ctl(GLSM_CTL_STATE_CONTEXT_RESET, NULL);
   //rglgen_resolve_symbols(hw_render.get_proc_address);

   memset(&pl_rearmed_cbs.gpu_peopsgl, 0, sizeof(pl_rearmed_cbs.gpu_peopsgl));

   if (!glsm_ctl(GLSM_CTL_STATE_SETUP, NULL))
      return;
   fb_ready = true;
   init_program_now = true;


    glsm_ctl(GLSM_CTL_STATE_BIND, NULL);

	        if (GPU_open != NULL && GPU_close != NULL) {
		printf("gpu_open\n");
	        GPU_close();
	        GPU_open(&gpuDisp, "PCSX", NULL);
}
    printf(GetConfigInfos(0));

    fake_bios_gpu_setup();

}

static void context_destroy(void)
{
   glsm_ctl(GLSM_CTL_STATE_CONTEXT_DESTROY, NULL);
}

static bool context_framebuffer_lock(void *data)
{
   if (fb_ready)
      return false;
   return true;
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

unsigned retro_api_version(void)
{
	return RETRO_API_VERSION;
}

static int controller_port_variable(unsigned port, struct retro_variable *var)
{
	if (port >= PORTS_NUMBER)
		return 0;

	if (!environ_cb)
		return 0;

	var->value = NULL;
	switch (port) {
	case 0:
		var->key = "pcsx_rearmed_pad1type";
		break;
	case 1:
		var->key = "pcsx_rearmed_pad2type";
		break;
	case 2:
		var->key = "pcsx_rearmed_pad3type";
		break;
	case 3:
		var->key = "pcsx_rearmed_pad4type";
		break;
	case 4:
		var->key = "pcsx_rearmed_pad5type";
		break;
	case 5:
		var->key = "pcsx_rearmed_pad6type";
		break;
	case 6:
		var->key = "pcsx_rearmed_pad7type";
		break;
	case 7:
		var->key = "pcsx_rearmed_pad8type";
		break;
	}

	return environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, var) || var->value;
}

static void update_controller_port_variable(unsigned port)
{
	if (port >= PORTS_NUMBER)
		return;

	struct retro_variable var;

	if (controller_port_variable(port, &var))
	{
		if (strcmp(var.value, "standard") == 0)
			in_type[port] = PSE_PAD_TYPE_STANDARD;
		else if (strcmp(var.value, "analog") == 0)
			in_type[port] = PSE_PAD_TYPE_ANALOGPAD;
		else if (strcmp(var.value, "negcon") == 0)
			in_type[port] = PSE_PAD_TYPE_NEGCON;
		else if (strcmp(var.value, "none") == 0)
			in_type[port] = PSE_PAD_TYPE_NONE;
		// else 'default' case, do nothing
	}
}

static void update_controller_port_device(unsigned port, unsigned device)
{
	if (port >= PORTS_NUMBER)
		return;

	struct retro_variable var;

	if (!controller_port_variable(port, &var))
		return;

	if (strcmp(var.value, "default") != 0)
		return;

	switch (device)
	{
	case RETRO_DEVICE_JOYPAD:
		in_type[port] = PSE_PAD_TYPE_STANDARD;
		break;
	case RETRO_DEVICE_ANALOG:
		in_type[port] = PSE_PAD_TYPE_ANALOGPAD;
		break;
	case RETRO_DEVICE_MOUSE:
		in_type[port] = PSE_PAD_TYPE_MOUSE;
		break;
	case RETRO_DEVICE_LIGHTGUN:
		in_type[port] = PSE_PAD_TYPE_GUN;
		break;
	case RETRO_DEVICE_NONE:
	default:
		in_type[port] = PSE_PAD_TYPE_NONE;
	}
}

static void update_multitap()
{
	struct retro_variable var;
	int auto_case, port;

	var.value = NULL;
	var.key = "pcsx_rearmed_multitap1";
	auto_case = 0;
	if (environ_cb && (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value))
	{
		if (strcmp(var.value, "enabled") == 0)
			multitap1 = 1;
		else if (strcmp(var.value, "disabled") == 0)
			multitap1 = 0;
		else // 'auto' case
			auto_case = 1;
	}
	else
		auto_case = 1;

	if (auto_case)
	{
		// If a gamepad is plugged after port 2, we need a first multitap.
		multitap1 = 0;
		for (port = 2; port < PORTS_NUMBER; port++)
			multitap1 |= in_type[port] != PSE_PAD_TYPE_NONE;
	}

	var.value = NULL;
	var.key = "pcsx_rearmed_multitap2";
	auto_case = 0;
	if (environ_cb && (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value))
	{
		if (strcmp(var.value, "enabled") == 0)
			multitap2 = 1;
		else if (strcmp(var.value, "disabled") == 0)
			multitap2 = 0;
		else // 'auto' case
			auto_case = 1;
	}
	else
		auto_case = 1;

	if (auto_case)
	{
		// If a gamepad is plugged after port 4, we need a second multitap.
		multitap2 = 0;
		for (port = 4; port < PORTS_NUMBER; port++)
			multitap2 |= in_type[port] != PSE_PAD_TYPE_NONE;
	}
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
	SysPrintf("port %u  device %u",port,device);

	if (port >= PORTS_NUMBER)
		return;

	update_controller_port_device(port, device);
	update_multitap();
}

void retro_get_system_info(struct retro_system_info *info)
{
	memset(info, 0, sizeof(*info));
	info->library_name = "PCSX-ReARMed";
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
	info->library_version = "r22" GIT_VERSION;
	info->valid_extensions = "bin|cue|img|mdf|pbp|toc|cbn|m3u";
	info->need_fullpath = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
	memset(info, 0, sizeof(*info));
	info->timing.fps            = is_pal_mode ? 50 : 60;
	info->timing.sample_rate    = 44100;
	info->geometry.base_width   = 320;
	info->geometry.base_height  = 240;
	info->geometry.max_width    = VOUT_MAX_WIDTH;
	info->geometry.max_height   = VOUT_MAX_HEIGHT;
	info->geometry.aspect_ratio = 4.0 / 3.0;
}

/* savestates */
size_t retro_serialize_size(void)
{
	// it's currently 4380651-4397047 bytes,
	// but have some reserved for future
	return 0x440000;
}

struct save_fp {
	char *buf;
	size_t pos;
	int is_write;
};

static void *save_open(const char *name, const char *mode)
{
	struct save_fp *fp;

	if (name == NULL || mode == NULL)
		return NULL;

	fp = malloc(sizeof(*fp));
	if (fp == NULL)
		return NULL;

	fp->buf = (char *)name;
	fp->pos = 0;
	fp->is_write = (mode[0] == 'w' || mode[1] == 'w');

	return fp;
}

static int save_read(void *file, void *buf, u32 len)
{
	struct save_fp *fp = file;
	if (fp == NULL || buf == NULL)
		return -1;

	memcpy(buf, fp->buf + fp->pos, len);
	fp->pos += len;
	return len;
}

static int save_write(void *file, const void *buf, u32 len)
{
	struct save_fp *fp = file;
	if (fp == NULL || buf == NULL)
		return -1;

	memcpy(fp->buf + fp->pos, buf, len);
	fp->pos += len;
	return len;
}

static long save_seek(void *file, long offs, int whence)
{
	struct save_fp *fp = file;
	if (fp == NULL)
		return -1;

	switch (whence) {
	case SEEK_CUR:
		fp->pos += offs;
		return fp->pos;
	case SEEK_SET:
		fp->pos = offs;
		return fp->pos;
	default:
		return -1;
	}
}

static void save_close(void *file)
{
	struct save_fp *fp = file;
	size_t r_size = retro_serialize_size();
	if (fp == NULL)
		return;

	if (fp->pos > r_size)
		SysPrintf("ERROR: save buffer overflow detected\n");
	else if (fp->is_write && fp->pos < r_size)
		// make sure we don't save trash in leftover space
		memset(fp->buf + fp->pos, 0, r_size - fp->pos);
	free(fp);
}

bool retro_serialize(void *data, size_t size)
{
	int ret = SaveState(data);
	return ret == 0 ? true : false;
}

bool retro_unserialize(const void *data, size_t size)
{
	int ret = LoadState(data);
	return ret == 0 ? true : false;
}

/* cheats */
void retro_cheat_reset(void)
{
	ClearAllCheats();
}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
	char buf[256];
	int ret;

	// cheat funcs are destructive, need a copy..
	strncpy(buf, code, sizeof(buf));
	buf[sizeof(buf) - 1] = 0;
	
	//Prepare buffered cheat for PCSX's AddCheat fucntion.
	int cursor=0;
	int nonhexdec=0;
	while (buf[cursor]){
		if (!(ISHEXDEC)){
			if (++nonhexdec%2){
				buf[cursor]=' ';
			} else {
				buf[cursor]='\n';
			}
		}
		cursor++;
	}
	

	if (index < NumCheats)
		ret = EditCheat(index, "", buf);
	else
		ret = AddCheat("", buf);

	if (ret != 0)
		SysPrintf("Failed to set cheat %#u\n", index);
	else if (index < NumCheats)
		Cheats[index].Enabled = enabled;
}

/* multidisk support */
static bool disk_ejected;
static unsigned int disk_current_index;
static unsigned int disk_count;
static struct disks_state {
	char *fname;
	int internal_index; // for multidisk eboots
} disks[8];

static bool disk_set_eject_state(bool ejected)
{
	// weird PCSX API..
	SetCdOpenCaseTime(ejected ? -1 : (time(NULL) + 2));
	LidInterrupt();

	disk_ejected = ejected;
	return true;
}

static bool disk_get_eject_state(void)
{
	/* can't be controlled by emulated software */
	return disk_ejected;
}

static unsigned int disk_get_image_index(void)
{
	return disk_current_index;
}

static bool disk_set_image_index(unsigned int index)
{
	if (index >= sizeof(disks) / sizeof(disks[0]))
		return false;

	CdromId[0] = '\0';
	CdromLabel[0] = '\0';

	if (disks[index].fname == NULL) {
		SysPrintf("missing disk #%u\n", index);
		CDR_shutdown();

		// RetroArch specifies "no disk" with index == count,
		// so don't fail here..
		disk_current_index = index;
		return true;
	}

	SysPrintf("switching to disk %u: \"%s\" #%d\n", index,
		disks[index].fname, disks[index].internal_index);

	cdrIsoMultidiskSelect = disks[index].internal_index;
	set_cd_image(disks[index].fname);
	if (ReloadCdromPlugin() < 0) {
		SysPrintf("failed to load cdr plugin\n");
		return false;
	}
	if (CDR_open() < 0) {
		SysPrintf("failed to open cdr plugin\n");
		return false;
	}

	if (!disk_ejected) {
		SetCdOpenCaseTime(time(NULL) + 2);
		LidInterrupt();
	}

	disk_current_index = index;
	return true;
}

static unsigned int disk_get_num_images(void)
{
	return disk_count;
}

static bool disk_replace_image_index(unsigned index,
	const struct retro_game_info *info)
{
	char *old_fname;
	bool ret = true;

	if (index >= sizeof(disks) / sizeof(disks[0]))
		return false;

	old_fname = disks[index].fname;
	disks[index].fname = NULL;
	disks[index].internal_index = 0;

	if (info != NULL) {
		disks[index].fname = strdup(info->path);
		if (index == disk_current_index)
			ret = disk_set_image_index(index);
	}

	if (old_fname != NULL)
		free(old_fname);

	return ret;
}

static bool disk_add_image_index(void)
{
	if (disk_count >= 8)
		return false;

	disk_count++;
	return true;
}

static struct retro_disk_control_callback disk_control = {
	.set_eject_state = disk_set_eject_state,
	.get_eject_state = disk_get_eject_state,
	.get_image_index = disk_get_image_index,
	.set_image_index = disk_set_image_index,
	.get_num_images = disk_get_num_images,
	.replace_image_index = disk_replace_image_index,
	.add_image_index = disk_add_image_index,
};

// just in case, maybe a win-rt port in the future?
#ifdef _WIN32
#define SLASH '\\'
#else
#define SLASH '/'
#endif

#ifndef PATH_MAX
#define PATH_MAX  4096
#endif

static char base_dir[PATH_MAX];

static bool read_m3u(const char *file)
{
	char line[PATH_MAX];
	char name[PATH_MAX];
	FILE *f = fopen(file, "r");
	if (!f)
		return false;

	while (fgets(line, sizeof(line), f) && disk_count < sizeof(disks) / sizeof(disks[0])) {
		if (line[0] == '#')
			continue;
		char *carrige_return = strchr(line, '\r');
		if (carrige_return)
			*carrige_return = '\0';
		char *newline = strchr(line, '\n');
		if (newline)
			*newline = '\0';

		if (line[0] != '\0')
		{
			snprintf(name, sizeof(name), "%s%c%s", base_dir, SLASH, line);
			disks[disk_count++].fname = strdup(name);
		}
	}

	fclose(f);
	return (disk_count != 0);
}

static void extract_directory(char *buf, const char *path, size_t size)
{
   char *base;
   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   base = strrchr(buf, '/');
   if (!base)
      base = strrchr(buf, '\\');

   if (base)
      *base = '\0';
   else
   {
      buf[0] = '.';
      buf[1] = '\0';
   }
}

#if defined(__QNX__) || defined(_WIN32)
/* Blackberry QNX doesn't have strcasestr */

/*
 * Find the first occurrence of find in s, ignore case.
 */
char *
strcasestr(const char *s, const char*find)
{
	char c, sc;
	size_t len;

	if ((c = *find++) != 0) {
		c = tolower((unsigned char)c);
		len = strlen(find);
		do {
			do {
				if ((sc = *s++) == 0)
					return (NULL);
			} while ((char)tolower((unsigned char)sc) != c);
		} while (strncasecmp(s, find, len) != 0);
		s--;
	}
	return ((char *)s);
}
#endif

bool retro_load_game(const struct retro_game_info *info)
{
	size_t i;
	bool is_m3u = (strcasestr(info->path, ".m3u") != NULL);

  enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      fprintf(stderr, "XRGB8888 is not supported.\n");
      return false;
   }
#if 1
   glsm_ctx_params_t params = {0};
   params.context_reset         = context_reset;
   params.context_destroy       = context_destroy;
   params.environ_cb            = environ_cb;
   params.stencil               = false;
   params.imm_vbo_draw          = NULL;
   params.imm_vbo_disable       = NULL;
   params.framebuffer_lock      = context_framebuffer_lock;

   if (!glsm_ctl(GLSM_CTL_STATE_CONTEXT_INIT, &params))
      return false;
#else


   if (!retro_init_hw_context())
   {
      fprintf(stderr, "HW Context could not be initialized, exiting...\n");
      return false;
   }
#endif

   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },


      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 2, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 2, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 2, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 2, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 3, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 3, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 3, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 3, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 4, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 4, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 4, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 4, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 5, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 5, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 5, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 5, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 6, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 6, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 6, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 6, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 7, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 7, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 7, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 7, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 0 },
   };

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

#if 0 //def FRONTEND_SUPPORTS_RGB565
	enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
	if (environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
		SysPrintf("RGB565 supported, using it\n");
	}
#endif

	if (info == NULL || info->path == NULL) {
		SysPrintf("info->path required\n");
		return false;
	}

	if (plugins_opened) {
		ClosePlugins();
		plugins_opened = 0;
	}

	for (i = 0; i < sizeof(disks) / sizeof(disks[0]); i++) {
		if (disks[i].fname != NULL) {
			free(disks[i].fname);
			disks[i].fname = NULL;
		}
		disks[i].internal_index = 0;
	}

	disk_current_index = 0;
	extract_directory(base_dir, info->path, sizeof(base_dir));

	if (is_m3u) {
		if (!read_m3u(info->path)) {
			SysPrintf("failed to read m3u file\n");
			return false;
		}
	} else {
		disk_count = 1;
		disks[0].fname = strdup(info->path);
	}

	set_cd_image(disks[0].fname);

	/* have to reload after set_cd_image for correct cdr plugin */
	if (LoadPlugins() == -1) {
		SysPrintf("failed to load plugins\n");
		return false;
	}

	plugins_opened = 1;
	NetOpened = 0;

	if (OpenPlugins() == -1) {
		SysPrintf("failed to open plugins\n");
		return false;
	}

	plugin_call_rearmed_cbs();
	dfinput_activate();

	Config.PsxAuto = 1;
	if (CheckCdrom() == -1) {
		SysPrintf("unsupported/invalid CD image: %s\n", info->path);
		return false;
	}

	SysReset();
	if (LoadCdrom() == -1) {
		SysPrintf("could not load CD-ROM!\n");
		return false;
	}
	emu_on_new_cd(0);

	// multidisk images
	if (!is_m3u) {
		disk_count = cdrIsoMultidiskCount < 8 ? cdrIsoMultidiskCount : 8;
		for (i = 1; i < sizeof(disks) / sizeof(disks[0]) && i < cdrIsoMultidiskCount; i++) {
			disks[i].fname = strdup(info->path);
			disks[i].internal_index = i;
		}
	}

	return true;
}

unsigned retro_get_region(void)
{
	return is_pal_mode ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

void *retro_get_memory_data(unsigned id)
{
	if (id == RETRO_MEMORY_SAVE_RAM)
		return Mcd1Data;
	else
		return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
	if (id == RETRO_MEMORY_SAVE_RAM)
		return MCD_SIZE;
	else
		return 0;
}

void retro_reset(void)
{
   //hack to prevent retroarch freezing when reseting in the menu but not while running with the hot key
   rebootemu = 1;
	//SysReset();
}

static const unsigned short retro_psx_map[] = {
	[RETRO_DEVICE_ID_JOYPAD_B]	= 1 << DKEY_CROSS,
	[RETRO_DEVICE_ID_JOYPAD_Y]	= 1 << DKEY_SQUARE,
	[RETRO_DEVICE_ID_JOYPAD_SELECT]	= 1 << DKEY_SELECT,
	[RETRO_DEVICE_ID_JOYPAD_START]	= 1 << DKEY_START,
	[RETRO_DEVICE_ID_JOYPAD_UP]	= 1 << DKEY_UP,
	[RETRO_DEVICE_ID_JOYPAD_DOWN]	= 1 << DKEY_DOWN,
	[RETRO_DEVICE_ID_JOYPAD_LEFT]	= 1 << DKEY_LEFT,
	[RETRO_DEVICE_ID_JOYPAD_RIGHT]	= 1 << DKEY_RIGHT,
	[RETRO_DEVICE_ID_JOYPAD_A]	= 1 << DKEY_CIRCLE,
	[RETRO_DEVICE_ID_JOYPAD_X]	= 1 << DKEY_TRIANGLE,
	[RETRO_DEVICE_ID_JOYPAD_L]	= 1 << DKEY_L1,
	[RETRO_DEVICE_ID_JOYPAD_R]	= 1 << DKEY_R1,
	[RETRO_DEVICE_ID_JOYPAD_L2]	= 1 << DKEY_L2,
	[RETRO_DEVICE_ID_JOYPAD_R2]	= 1 << DKEY_R2,
	[RETRO_DEVICE_ID_JOYPAD_L3]	= 1 << DKEY_L3,
	[RETRO_DEVICE_ID_JOYPAD_R3]	= 1 << DKEY_R3,
};
#define RETRO_PSX_MAP_LEN (sizeof(retro_psx_map) / sizeof(retro_psx_map[0]))

static void update_variables(bool in_flight)
{
   struct retro_variable var;
   int i;

   var.value = NULL;
   var.key = "pcsx_rearmed_frameskip";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
      pl_rearmed_cbs.frameskip = atoi(var.value);

   var.value = NULL;
   var.key = "pcsx_rearmed_region";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      Config.PsxAuto = 0;
      if (strcmp(var.value, "Automatic") == 0)
         Config.PsxAuto = 1;
      else if (strcmp(var.value, "NTSC") == 0)
         Config.PsxType = 0;
      else if (strcmp(var.value, "PAL") == 0)
         Config.PsxType = 1;
   }

   for (i = 0; i < PORTS_NUMBER; i++)
      update_controller_port_variable(i);

   update_multitap();

   var.value = NULL;
   var.key = "pcsx_rearmed_vibration";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         in_enable_vibration = 0;
      else if (strcmp(var.value, "enabled") == 0)
         in_enable_vibration = 1;
   }

   var.value = NULL;
   var.key = "pcsx_rearmed_dithering";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0) {
         pl_rearmed_cbs.gpu_peops.iUseDither = 0;
         pl_rearmed_cbs.gpu_peopsgl.bDrawDither = 0;
#ifdef __ARM_NEON__
         pl_rearmed_cbs.gpu_neon.allow_dithering = 0;
#endif
      }
      else if (strcmp(var.value, "enabled") == 0) {
         pl_rearmed_cbs.gpu_peops.iUseDither = 1;
         pl_rearmed_cbs.gpu_peopsgl.bDrawDither = 1;
#ifdef __ARM_NEON__
         pl_rearmed_cbs.gpu_neon.allow_dithering = 1;
#endif
      }
   }

#ifdef __ARM_NEON__
   var.value = "NULL";
   var.key = "pcsx_rearmed_neon_interlace_enable";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         pl_rearmed_cbs.gpu_neon.allow_interlace = 0;
      else if (strcmp(var.value, "enabled") == 0)
         pl_rearmed_cbs.gpu_neon.allow_interlace = 1;
   }

   var.value = NULL;
   var.key = "pcsx_rearmed_neon_enhancement_enable";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         pl_rearmed_cbs.gpu_neon.enhancement_enable = 0;
      else if (strcmp(var.value, "enabled") == 0)
         pl_rearmed_cbs.gpu_neon.enhancement_enable = 1;
   }

   var.value = NULL;
   var.key = "pcsx_rearmed_neon_enhancement_no_main";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         pl_rearmed_cbs.gpu_neon.enhancement_no_main = 0;
      else if (strcmp(var.value, "enabled") == 0)
         pl_rearmed_cbs.gpu_neon.enhancement_no_main = 1;
   }
#endif

   var.value = "NULL";
   var.key = "pcsx_rearmed_duping_enable";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         duping_enable = false;
      else if (strcmp(var.value, "enabled") == 0)
         duping_enable = true;
   }

#ifndef DRC_DISABLE
   var.value = NULL;
   var.key = "pcsx_rearmed_drc";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      R3000Acpu *prev_cpu = psxCpu;

#ifdef _3DS
      if(!__ctr_svchax)
         Config.Cpu = CPU_INTERPRETER;
      else
#endif
      if (strcmp(var.value, "disabled") == 0)
         Config.Cpu = CPU_INTERPRETER;
      else if (strcmp(var.value, "enabled") == 0)
         Config.Cpu = CPU_DYNAREC;

      psxCpu = (Config.Cpu == CPU_INTERPRETER) ? &psxInt : &psxRec;
      if (psxCpu != prev_cpu) {
         prev_cpu->Shutdown();
         psxCpu->Init();
         psxCpu->Reset(); // not really a reset..
      }
   }
#endif

   var.value = "NULL";
   var.key = "pcsx_rearmed_spu_reverb";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         spu_config.iUseReverb = false;
      else if (strcmp(var.value, "enabled") == 0)
         spu_config.iUseReverb = true;
   }

   var.value = "NULL";
   var.key = "pcsx_rearmed_spu_interpolation";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "simple") == 0)
         spu_config.iUseInterpolation = 1;
      else if (strcmp(var.value, "gaussian") == 0)
         spu_config.iUseInterpolation = 2;
      else if (strcmp(var.value, "cubic") == 0)
         spu_config.iUseInterpolation = 3;
      else if (strcmp(var.value, "off") == 0)
         spu_config.iUseInterpolation = 0;
   }

   var.value = "NULL";
   var.key = "pcsx_rearmed_pe2_fix";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         Config.RCntFix = 0;
      else if (strcmp(var.value, "enabled") == 0)
         Config.RCntFix = 1;
   }

   var.value = "NULL";
   var.key = "pcsx_rearmed_inuyasha_fix";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         Config.VSyncWA = 0;
      else if (strcmp(var.value, "enabled") == 0)
         Config.VSyncWA = 1;
   }

   if (in_flight) {
      // inform core things about possible config changes
#if 0
      plugin_call_rearmed_cbs();
	SysPrintf("do GPU_open\n\n");
      if (GPU_open != NULL && GPU_close != NULL) {
         GPU_close();
         GPU_open(&gpuDisp, "PCSX", NULL);
      }
#endif

      dfinput_activate();
   }
   else{
      //not yet running
      
      //bootlogo display hack
      if (found_bios) {
         var.value = "NULL";
         var.key = "pcsx_rearmed_show_bios_bootlogo";
         if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
         {
            if (strcmp(var.value, "enabled") == 0)
               rebootemu = 1;
         }
      }
   }
}

uint32_t frame_count=0;
int first_time = 1;

void flip_cb(void)
{
#if 0
if (fb_ready) {
       glsm_ctl(GLSM_CTL_STATE_UNBIND, NULL);
       video_cb(RETRO_HW_FRAME_BUFFER_VALID, vout_width, vout_height, 0);
       glsm_ctl(GLSM_CTL_STATE_BIND, NULL);
}
#endif
}

static retro_hw_get_proc_address_t *retroGetProcAddress(const char *name)
{
printf("name: %s\n", name); //Debug
return hw_render.get_proc_address(name);
}

void retro_run(void)
{
#if 1
    if(fb_ready && (first_time==1))
	{
	        //glBindFramebuffer(RARCH_GL_FRAMEBUFFER, hw_render.get_current_framebuffer());
	        if (GPU_open != NULL && GPU_close != NULL) {
		printf("gpu_open\n");
	        GPU_close();
	        GPU_open(&gpuDisp, "PCSX", NULL);
		fake_bios_gpu_setup();
		video_cb(NULL, vout_width, vout_height, 0);
#if 1
	int i;

	for (i = 0; i < sizeof(gpu_ctl_def) / sizeof(gpu_ctl_def[0]); i++)
		GPU_writeStatus(gpu_ctl_def[i]);

	for (i = 0; i < sizeof(gpu_data_def) / sizeof(gpu_data_def[0]); i++)
		GPU_writeData(gpu_data_def[i]);
#endif

		first_time=0;
	        }

      }
else if (!fb_ready) return;
#endif

    int i;
    //SysReset must be run while core is running,Not in menu (Locks up Retroarch)
    if(rebootemu != 0){
      rebootemu = 0;
      SysReset();
    }

	input_poll_cb();

	bool updated = false;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
		SysPrintf("update_variables inflight\n\n");
		update_variables(true);
	}

	// reset all keystate, query libretro for keystate
	int j;
	for(i = 0; i < PORTS_NUMBER; i++) {
		in_keystate[i] = 0;

		if (in_type[i] == PSE_PAD_TYPE_NONE)
			continue;

		// query libretro for keystate
		for (j = 0; j < RETRO_PSX_MAP_LEN; j++)
			if (input_state_cb(i, RETRO_DEVICE_JOYPAD, 0, j))
				in_keystate[i] |= retro_psx_map[j];

		if (in_type[i] == PSE_PAD_TYPE_ANALOGPAD)
		{
			in_analog_left[i][0] = MIN((input_state_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X) / 255) + 128, 255);
			in_analog_left[i][1] = MIN((input_state_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y) / 255) + 128, 255);
			in_analog_right[i][0] = MIN((input_state_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X) / 255) + 128, 255);
			in_analog_right[i][1] = MIN((input_state_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y) / 255) + 128, 255);
		}
	}

       //glsm_ctl(GLSM_CTL_STATE_BIND, NULL);
       //GLinitialize();

	//glViewport(0,0,640,480);                               
	//glScissor(0, 0, 640,480);                        // init clipping (fullscreen)
	//glEnable(GL_SCISSOR_TEST);
	//SetAspectRatio();

	glMatrixMode(GL_PROJECTION);                          // init projection with psx resolution
	glLoadIdentity();

	if ((PSXDisplay.DisplayMode.x!=0) && (PSXDisplay.DisplayMode.y!=0))
	glOrtho(0,PSXDisplay.DisplayMode.x,
	          PSXDisplay.DisplayMode.y, 0, -1, 1);



#if 0	
	glLineWidth(2);
	glPolygonMode(GL_FRONT, GL_LINE);
	glPolygonMode(GL_BACK, GL_LINE);
#endif


#if 0
        //glBindFramebuffer(RARCH_GL_FRAMEBUFFER, hw_render.get_current_framebuffer());

#if 1
	glViewport(0,0,640,480);                               
	glScissor(0, 0, 640, 480);                        // init clipping (fullscreen)
	glEnable(GL_SCISSOR_TEST);
	SetAspectRatio();
#endif                       

#if 0
	glMatrixMode(GL_TEXTURE);                             // init psx tex sow and tow if not "ownscale"
	glLoadIdentity();
	//glScalef(1.0f/255.99f,1.0f/255.99f,1.0f);

#endif 

#if 1
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	//glScalef(1.0f/255.99f,1.0f/255.99f,1.0f);             
                          				
#endif

	glOrtho(0,320,
          240, 0, -1, 1);

	glEnable(GL_ALPHA_TEST);

	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);                 // first buffer clear

	if(iZBufferDepth)
	{
		glEnable(GL_DEPTH_TEST);  
		glDepthFunc(GL_ALWAYS);
		glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
	}
	else {
		glDisable(GL_DEPTH_TEST);
		glDepthFunc(GL_LESS);
		glClear(GL_COLOR_BUFFER_BIT);
	}

	if (bUseLines)
//	if (true)
	{
	glPolygonMode(GL_FRONT, GL_LINE);
	glPolygonMode(GL_BACK, GL_LINE); 
	}
	else
	{
	glPolygonMode(GL_FRONT, GL_FILL);
	glPolygonMode(GL_BACK, GL_FILL);
	}
#if 0
	if(bBlendEnable)	glEnable(GL_BLEND);
	if(bDrawDither)		glEnable(GL_DITHER);
	if(bTexEnabled)		glEnable(GL_TEXTURE_2D);
#else
	if(bBlendEnable)	{glDisable(GL_BLEND);bBlendEnable=false;}
	if(bDrawDither)		{glDisable(GL_DITHER);bDrawDither=false;}
	if(bTexEnabled)		{glDisable(GL_TEXTURE_2D);bTexEnabled=false;}
	
#endif

#if 1		
	glPixelTransferi(GL_RED_SCALE, 1);                    // to be sure:
	glPixelTransferi(GL_RED_BIAS, 0);                     // init more OGL vals
	glPixelTransferi(GL_GREEN_SCALE, 1);
	glPixelTransferi(GL_GREEN_BIAS, 0);
	glPixelTransferi(GL_BLUE_SCALE, 1);
	glPixelTransferi(GL_BLUE_BIAS, 0);
	glPixelTransferi(GL_ALPHA_SCALE, 1);
	glPixelTransferi(GL_ALPHA_BIAS, 0);  
#endif

	//DisplaySet = false;
	//bDisplayNotSet = true;
#endif

	stop = 0;
	psxCpu->Execute();
	//glFlush();
	//glFinish();

      //  glsm_ctl(GLSM_CTL_STATE_UNBIND, NULL);
	video_cb(RETRO_HW_FRAME_BUFFER_VALID, vout_width, vout_height, 0);
	vout_fb_dirty = 0;

	//glPolygonMode(GL_FRONT, GL_FILL);
	//glPolygonMode(GL_BACK, GL_FILL);

}

static bool try_use_bios(const char *path)
{
	FILE *f;
	long size;
	const char *name;

	f = fopen(path, "rb");
	if (f == NULL)
		return false;

	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fclose(f);

	if (size != 512 * 1024)
		return false;

	name = strrchr(path, SLASH);
	if (name++ == NULL)
		name = path;
	snprintf(Config.Bios, sizeof(Config.Bios), "%s", name);
	return true;
}

#ifndef VITA
#include <sys/types.h>
#include <dirent.h>

static bool find_any_bios(const char *dirpath, char *path, size_t path_size)
{
	DIR *dir;
	struct dirent *ent;
	bool ret = false;

	dir = opendir(dirpath);
	if (dir == NULL)
		return false;

	while ((ent = readdir(dir))) {
		if (strncasecmp(ent->d_name, "scph", 4) != 0)
			continue;

		snprintf(path, path_size, "%s/%s", dirpath, ent->d_name);
		ret = try_use_bios(path);
		if (ret)
			break;
	}
	closedir(dir);
	return ret;
}
#else
#define find_any_bios(...) false
#endif

static void check_system_specs(void)
{
   unsigned level = 6;
   environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);
}

void retro_init(void)
{
	const char *bios[] = { "SCPH101", "SCPH7001", "SCPH5501", "SCPH1001" };
	const char *dir;
	char path[256];
	int i, ret;
   
   found_bios = false;

#ifdef __MACH__
	// magic sauce to make the dynarec work on iOS
	syscall(SYS_ptrace, 0 /*PTRACE_TRACEME*/, 0, 0, 0);
#endif

#ifdef _3DS
   psxMapHook = pl_3ds_mmap;
   psxUnmapHook = pl_3ds_munmap;
#endif
#ifdef VITA
   if(init_vita_mmap()<0)
      abort();
   psxMapHook = pl_vita_mmap;
   psxUnmapHook = pl_vita_munmap;
#endif
	ret = emu_core_preinit();
#ifdef _3DS 
   /* emu_core_preinit sets the cpu to dynarec */
   if(!__ctr_svchax)
      Config.Cpu = CPU_INTERPRETER;
#endif

	ret |= emu_core_init();
	if (ret != 0) {
		SysPrintf("PCSX init failed.\n");
		exit(1);
	}


	posix_memalign(&vout_buf, 16, VOUT_MAX_WIDTH * VOUT_MAX_HEIGHT * 2);

	//vout_buf = malloc(VOUT_MAX_WIDTH * VOUT_MAX_HEIGHT * 2);

  
  vout_buf_ptr = vout_buf;
  
	if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
	{
		snprintf(Config.BiosDir, sizeof(Config.BiosDir), "%s", dir);

		for (i = 0; i < sizeof(bios) / sizeof(bios[0]); i++) {
			snprintf(path, sizeof(path), "%s/%s.bin", dir, bios[i]);
			found_bios = try_use_bios(path);
			if (found_bios)
				break;
		}

		if (!found_bios)
			found_bios = find_any_bios(dir, path, sizeof(path));
	}
	if (found_bios) {
		SysPrintf("found BIOS file: %s\n", Config.Bios);
	}
	else
	{
		SysPrintf("no BIOS files found.\n");
		struct retro_message msg =
		{
			"No PlayStation BIOS file found - add for better compatibility",
			180
		};
		environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, (void*)&msg);
	}

	environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &vout_can_dupe);
	environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, &disk_control);
	environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble);

	/* Set how much slower PSX CPU runs * 100 (so that 200 is 2 times)
	 * we have to do this because cache misses and some IO penalties
	 * are not emulated. Warning: changing this may break compatibility. */
	cycle_multiplier = 175;
#ifdef HAVE_PRE_ARMV7
	cycle_multiplier = 200;
#endif
	pl_rearmed_cbs.gpu_peops.iUseDither = 1;
        pl_rearmed_cbs.gpu_peopsgl.flip_cb = flip_cb;
	pl_rearmed_cbs.gpu_peopsgl.retroGetProcAddress = retroGetProcAddress;
	spu_config.iUseFixedUpdates = 1;

	McdDisable[0] = 0;
	McdDisable[1] = 1;
	init_memcard(Mcd1Data);
   init_memcard(Mcd2Data);

	SaveFuncs.open = save_open;
	SaveFuncs.read = save_read;
	SaveFuncs.write = save_write;
	SaveFuncs.seek = save_seek;
	SaveFuncs.close = save_close;

	update_variables(false);
	check_system_specs();
}

void retro_deinit(void)
{
	SysClose();
#ifdef _3DS
   linearFree(vout_buf);
#else
	free(vout_buf);
#endif
	vout_buf = NULL;

#ifdef VITA
  deinit_vita_mmap();
#endif
}

#ifdef VITA
#include <psp2/kernel/threadmgr.h>
int usleep (unsigned long us)
{
   sceKernelDelayThread(us);
}
#endif
