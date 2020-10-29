#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#include <retro_timers.h>

#include "libretro.h"
#include "remotepad.h"
#include "charset.h"

#define DESC_NUM_PORTS(desc) ((desc)->port_max - (desc)->port_min + 1)
#define DESC_NUM_INDICES(desc) ((desc)->index_max - (desc)->index_min + 1)
#define DESC_NUM_IDS(desc) ((desc)->id_max - (desc)->id_min + 1)

#define DESC_OFFSET(desc, port, index, id) ( \
		port * ((desc)->index_max - (desc)->index_min + 1) * ((desc)->id_max - (desc)->id_min + 1) + \
		index * ((desc)->id_max - (desc)->id_min + 1) + \
		id \
		)

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#ifndef NO_SSH
FILE *ssh;
#endif

uint8_t category = 0;
uint8_t category_old = 20;
bool en_sw;

struct descriptor {
	int device;
	int port_min;
	int port_max;
	int index_min;
	int index_max;
	int id_min;
	int id_max;
	uint16_t *value;
};

static struct retro_log_callback logger;

static retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

static uint16_t *frame_buf;

static struct descriptor joypad = {
	.device = RETRO_DEVICE_JOYPAD,
	.port_min = 0,
	.port_max = 0,
	.index_min = 0,
	.index_max = 0,
	.id_min = RETRO_DEVICE_ID_JOYPAD_B,
	.id_max = RETRO_DEVICE_ID_JOYPAD_R3
};

static struct descriptor analog = {
	.device = RETRO_DEVICE_ANALOG,
	.port_min = 0,
	.port_max = 0,
	.index_min = RETRO_DEVICE_INDEX_ANALOG_LEFT,
	.index_max = RETRO_DEVICE_INDEX_ANALOG_RIGHT,
	.id_min = RETRO_DEVICE_ID_ANALOG_X,
	.id_max = RETRO_DEVICE_ID_ANALOG_Y
};

static struct descriptor *descriptors[] = {
	&joypad,
	&analog
};



void draw_pixel(int x, int y)
{
	frame_buf[x + y * 320] = 0xffff;
}

void draw_box(int sx, int sy, int ex, int ey)
{
	int savey = sy;
	for (; sx <= ex; sx++) 
		for (sy = savey; sy <= ey; sy++)
			draw_pixel(sx, sy);
}

void draw_char(int x, int y, char c)
{
	int savey = y;
	for (int i = 0; i <= 5; i++)
		for (int j = 0; j <= 5; j++)
			if (char_a[i][j] == 1)
				draw_pixel(x + i, y + j);
}

void draw_pad(void)
{
	uint16_t *pixel = frame_buf + 49 * 320 + 32;

	for (unsigned rle = 0; rle < sizeof(body); )
	{
		uint16_t color = 0;

		for (unsigned runs = body[rle++]; runs > 0; runs--)
		{
			for (unsigned count = body[rle++]; count > 0; count--)
			{
				*pixel++ = color;
			}

			color = 0x4208 - color;
		}

		pixel += 65;
	}
}

void retro_init(void)
{
	struct descriptor *desc;
	int size;

	frame_buf = (uint16_t*)calloc(320 * 240, sizeof(uint16_t));

#ifndef NO_SSH
	ssh = popen("ssh -o StrictHostKeyChecking=no macc24@192.168.0.51", "w");
	fprintf(ssh, "~/uinput-joystick-demo/fw-input\n");
#endif
	if (frame_buf)
	{
		draw_pad();
	}

	/* Allocate descriptor values */
	for (long unsigned int i = 0; i < ARRAY_SIZE(descriptors); i++) {
		desc = descriptors[i];
		size = DESC_NUM_PORTS(desc) * DESC_NUM_INDICES(desc) * DESC_NUM_IDS(desc);
		descriptors[i]->value = (uint16_t*)calloc(size, sizeof(uint16_t));
	}

	charset['a'] = &char_a;
}

void retro_deinit(void)
{
	if (frame_buf)
		free(frame_buf);
	frame_buf = NULL;
#ifndef NO_SSH
	fprintf(ssh, "4096\n");
	pclose(ssh);
#endif
	/* Free descriptor values */
	for (long unsigned int i = 0; i < ARRAY_SIZE(descriptors); i++) {
		free(descriptors[i]->value);
		descriptors[i]->value = NULL;
	}
#ifdef KILL_RETROARCH
	system("pkill retroarch");
#endif
}

unsigned retro_api_version(void)
{
	return RETRO_API_VERSION;
}

void retro_set_controller_port_device(
		unsigned port, unsigned device)
{
	(void)port;
	(void)device;
}

void retro_get_system_info(
		struct retro_system_info *info)
{
	memset(info, 0, sizeof(*info));
	info->library_name	  = "SSH Gamepad";
	info->library_version  = "0.01";
	info->need_fullpath	  = false;
	info->valid_extensions = "";
}

void retro_get_system_av_info(
		struct retro_system_av_info *info)
{
	info->timing.fps = 60.0;
	info->timing.sample_rate = 0.0;

	info->geometry.base_width	= 320;
	info->geometry.base_height = 240;
	info->geometry.max_width	= 320;
	info->geometry.max_height	= 240;
	info->geometry.aspect_ratio = 4.0 / 3.0;
}

void retro_set_environment(retro_environment_t cb)
{
	static const struct retro_variable vars[] = {
		{ NULL, NULL },
	};
	enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
	cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);


	environ_cb = cb;
	bool no_content = true;
	cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_content);

	environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);

	if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logger))
		log_cb = logger.log;
}

static void netretropad_check_variables(void)
{
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
	audio_cb = cb;
}

void retro_set_audio_sample_batch(
		retro_audio_sample_batch_t cb)
{
	audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
	input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
	input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
	video_cb = cb;
}

void retro_reset(void)
{}

static void retropad_update_input(void)
{
	struct descriptor *desc;
	uint16_t state;
	uint16_t old;
	int offset;
	int port;
	int index;
	int id;
	long unsigned int i;

	/* Poll input */
	input_poll_cb();

	/* Parse descriptors */
	for (i = 0; i < ARRAY_SIZE(descriptors); i++) {
		/* Get current descriptor */
		desc = descriptors[i];

		/* Go through range of ports/indices/IDs */
		for (port = desc->port_min; port <= desc->port_max; port++)
			for (index = desc->index_min; index <= desc->index_max; index++)
				for (id = desc->id_min; id <= desc->id_max; id++) {
					/* Compute offset into array */
					offset = DESC_OFFSET(desc, port, index, id);

					/* Get old state */
					old = desc->value[offset];

					/* Get new state */
					state = input_state_cb(
							port,
							desc->device,
							index,
							id);

					/* Continue if state is unchanged */
					if (state == old)
						continue;

					/* Update state */
					desc->value[offset] = state;
				}
	}
}

void retro_run(void)
{
	unsigned rle, runs;
	uint16_t *pixel		= NULL;
	unsigned input_state = 0;
	int offset;
	int i;

	retropad_update_input();

	/* Combine RetroPad input states into one value */
	for (i = joypad.id_min; i <= joypad.id_max; i++)
	{
		offset = DESC_OFFSET(&joypad, 0, 0, i);
		if (joypad.value[offset])
			input_state |= 1 << i;
	}
	printf("%d         \r", category);
	fflush(stdout);

#ifndef NO_SSH
	fprintf(ssh, "%i\n", input_state);
	fflush(ssh);
#endif

	// hotkeys
	if ((input_state & 4096) && en_sw) // L2
	{
		for (int y = 1; y < 239; y++)
			for (int x = 1; x < 319; x++)
				frame_buf[x + y * 320] = 0x000000;
		category--;
		en_sw = false;
	}
	if ((input_state & 8192) && en_sw) // R2
	{
		for (int y = 1; y < 239; y++)
			for (int x = 1; x < 319; x++)
				frame_buf[x + y * 320] = 0x000000;
		category++;
		en_sw = false;
	}

	if (!en_sw) // ran everytime category has changed
	{
		switch(category)
		{
			case 0: draw_pad(); break;
			default: draw_box(10, 10, 50 + category*10, 50); break;
		}
	}

	draw_box(10, 10, 50, 50);
	draw_char(60, 60, 'a');

	if (!((input_state & 4096) || (input_state & 8192)))
		en_sw = true;

	pixel = frame_buf + 49 * 320 + 32;

	if (category == 0)
	{
		for (rle = 0; rle < sizeof(retropad_buttons); )
		{
			char paint = 0;

			for (runs = retropad_buttons[rle++]; runs > 0; runs--)
			{
				unsigned button = paint ? 1 << retropad_buttons[rle++] : 0;

				if (paint)
				{
					unsigned count;
					uint16_t color = (input_state & button) ? 0x0500 : 0xffff;

					for (count = retropad_buttons[rle++]; count > 0; count--)
						*pixel++ = color;
				}
				else
					pixel += retropad_buttons[rle++];

				paint = !paint;
			}

			pixel += 65;
		}
	}


	video_cb(frame_buf, 320, 240, 640);

	retro_sleep(4);
}

bool retro_load_game(const struct retro_game_info *info)
{
	netretropad_check_variables();

	return true;
}

void retro_unload_game(void)
{}

unsigned retro_get_region(void)
{
	return RETRO_REGION_NTSC;
}

bool retro_load_game_special(unsigned type,
		const struct retro_game_info *info, size_t num)
{
	(void)type;
	(void)info;
	(void)num;
	return false;
}

size_t retro_serialize_size(void)
{
	return 0;
}

bool retro_serialize(void *data, size_t size)
{
	(void)data;
	(void)size;
	return false;
}

bool retro_unserialize(const void *data,
		size_t size)
{
	(void)data;
	(void)size;
	return false;
}

void *retro_get_memory_data(unsigned id)
{
	(void)id;
	return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
	(void)id;
	return 0;
}

void retro_cheat_reset(void)
{}

void retro_cheat_set(unsigned idx,
		bool enabled, const char *code)
{
	(void)idx;
	(void)enabled;
	(void)code;
}
