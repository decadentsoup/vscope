// Copyright (C) 2018-2019 Megan Ruggiero. All rights reserved.
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdlib.h>

#include <pulse/pulseaudio.h>

#include <SDL.h>

#include <GL/gl.h>

#if defined(__GNUC__) || defined(__clang__)
#define UNUSED __attribute__((unused))
#else
#define UNUSED // empty
#endif

#define BUFFER_SIZE 1024
#define DEFAULT_WIDTH 480
#define DEFAULT_HEIGHT 480

#define errpax(message) (errx(EXIT_FAILURE, "[pulse] %s", message))
#define errpa(message) (errx(EXIT_FAILURE, "[pulse] %s: %s", message, pa_strerror(pa_context_errno(pa.context))))

static const char HELP_NOTICE[] =
"Displays a vectorscope based on audio from the specified PulseAudio sink.\n"
"If no sink is specified, the default sink will be used.\n"
"\n"
"Options:\n"
"  --help        display this help message\n"
"  --version     display the version of this program\n"
"  --geometry    set window size to WIDTHxHEIGHT, position to +X+Y, or both to\n"
"                WIDTHxHEIGHT+X+Y; for negative positions, use - in place of +\n"
"  --opacity     set window opacity to somewhere from 0.0 to 1.0\n"
"  --foreground  set foreground color (see below); set to rainbow for a variety\n"
"\n"
"Colors:\n"
"  Colors can be specified in hexadecimal red-green-blue format, with or without\n"
"  a preceding pound sign (#). For example, half-brightness red would be 7F0000.\n"
"  Colors are case-insensitive.\n"
"\n"
"Report bugs to: <https://github.com/decadentsoup/vscope/issues>\n"
"Vectorscope home page: <https://github.com/decadentsoup/vscope>";

static struct { int x, y, w, h; } geometry = {SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, DEFAULT_WIDTH, DEFAULT_HEIGHT};
static float opacity = 1;
static struct { float r, g, b; } color = {1, 1, 1};
static bool rainbow;

static struct {
	char *sink;
	pa_mainloop *mainloop;
	pa_context *context;
	pa_stream *stream;
} pa;

static SDL_Window *window;
static SDL_GLContext context;

static int16_t buffer[BUFFER_SIZE];

static void handle_exit(void);
static void parse_args(int, char **);
static bool parse_geometry(void);
static bool parse_foreground(void);
static void draw_buffer(void);
static void set_hue(float);
static void init_pulse(void);
static void handle_context_state(pa_context *, void *);
static void handle_server_info(pa_context *, const pa_server_info *, void *);
static void handle_stream_state(pa_stream *, void *);
static void handle_stream_read(pa_stream *, size_t, void *);

int
main(int argc, char **argv)
{
	unsigned int i, last_time;
	SDL_Event event;

	if (atexit(handle_exit))
		err(EXIT_FAILURE, "failed to register exit callback");

	parse_args(argc, argv);
	init_pulse();

	if (SDL_Init(SDL_INIT_VIDEO) < 0)
		errx(EXIT_FAILURE, "failed to initialize SDL: %s", SDL_GetError());

	if (!(window = SDL_CreateWindow("Vectorscope", geometry.x, geometry.y, geometry.w, geometry.h, SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE)))
		errx(EXIT_FAILURE, "failed to create window: %s", SDL_GetError());

	if (SDL_SetWindowOpacity(window, opacity))
		warnx("failed to set window opacity: %s", SDL_GetError());

	if (!(context = SDL_GL_CreateContext(window)))
		errx(EXIT_FAILURE, "failed to create context: %s", SDL_GetError());

	last_time = 0;

	for (;;) {
		if (pa_mainloop_iterate(pa.mainloop, false, NULL) < 0)
			errpax("failed to iterate mainloop");

		if ((i = SDL_GetTicks()) > last_time + 16) {
			SDL_GL_SwapWindow(window);
			glClear(GL_COLOR_BUFFER_BIT);
			last_time = i;
		}

		draw_buffer();

		while (SDL_PollEvent(&event))
			if (event.type == SDL_QUIT)
				return 0;
			else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_RESIZED)
				glViewport(0, 0, event.window.data1, event.window.data2);
	}
}

static void
handle_exit()
{
	SDL_GL_DeleteContext(context);
	SDL_DestroyWindow(window);
	SDL_Quit();

	if (pa.stream)
		pa_stream_unref(pa.stream);

	if (pa.context) {
		pa_context_disconnect(pa.context);
		pa_context_unref(pa.context);
	}

	if (pa.mainloop)
		pa_mainloop_free(pa.mainloop);

	free(pa.sink);
}

static void
parse_args(int argc, char **argv)
{
	static const struct option options[] = {
		{"help", no_argument, 0, 0},
		{"version", no_argument, 0, 0},
		{"geometry", required_argument, 0, 0},
		{"opacity", required_argument, 0, 0},
		{"foreground", required_argument, 0, 0},
		{0, 0, 0, 0}
	};

	int x, option_index;
	bool fail;

	for (fail = false, x = 0; x != -1;) {
		x = getopt_long(argc, argv, "", options, &option_index);
		if (x == 0) {
			switch (option_index) {
			case 0:
				printf("Usage: %s [sink]\n\n", basename(argv[0]));
				puts(HELP_NOTICE);
				exit(0);
			case 1:
				puts("Vectorscope " VERSION);
				exit(0);
			case 2:
				if (parse_geometry()) fail = true;
				break;
			case 3:
				if (sscanf(optarg, "%f", &opacity) == 1) {
					// nothing more to do
				} else {
					warnx("invalid window opacity");
					fail = true;
				}
				break;
			case 4:
				if (parse_foreground()) fail = true;
				break;
			}
		} else if (x == '?') {
			fail = true;
		}
	}

	optopt = argc - optind;
	if (optopt == 1) {
		if (!(pa.sink = strdup(argv[optind])))
			err(EXIT_FAILURE, "failure in strdup()");
	} else if (optopt > 0) {
		warnx("too many arguments");
		fail = true;
	}

	if (fail)
		errx(EXIT_FAILURE, "see --help for more information");
}

static bool
parse_geometry()
{
	if (sscanf(optarg, "%ix%i%i%i", &geometry.w, &geometry.h, &geometry.x, &geometry.y) == 4) {
		// nothing more to do
	} else if (sscanf(optarg, "%ix%i", &geometry.w, &geometry.h) == 2) {
		geometry.x = SDL_WINDOWPOS_UNDEFINED;
		geometry.y = SDL_WINDOWPOS_UNDEFINED;
	} else if (sscanf(optarg, "%i%i", &geometry.x, &geometry.y) == 2) {
		geometry.w = DEFAULT_WIDTH;
		geometry.h = DEFAULT_HEIGHT;
	} else {
		warnx("invalid geometry argument");
		return true;
	}

	return false;
}

static bool
parse_foreground()
{
	unsigned int r, g, b;

	if (!strcmp(optarg, "rainbow")) {
		rainbow = true;
	} else if (sscanf(optarg, "%2x%2x%2x", &r, &g, &b) == 3 || sscanf(optarg, "#%2x%2x%2x", &r, &g, &b)) {
		color.r = r / 255.0;
		color.g = g / 255.0;
		color.b = b / 255.0;
	} else {
		warnx("invalid color format");
		return true;
	}

	return false;
}

static void
draw_buffer()
{
	float x, y;
	int i;

	glBegin(GL_POINTS);

	for (i = 0; i < BUFFER_SIZE; i += 2) {
		x = buffer[i] / 30000.0;
		y = buffer[i + 1] / 30000.0;

		if (rainbow)
			set_hue(sqrtf(x * x + y * y) * 360.0);
		else
			glColor3f(color.r, color.g, color.b);

		glVertex2f(x, y);
	}

	glEnd();
}

static void
set_hue(float hue)
{
	float q, f;
	long i;

	if (hue >= 360.0)
		hue = 0;

	hue /= 60.0;

	i = (long)hue;
	f = hue - i;
	q = 1.0 - f;

	switch(i) {
	case 0: glColor3f(1, f, 0); break;
	case 1: glColor3f(q, 1, 0); break;
	case 2: glColor3f(0, 1, f); break;
	case 3: glColor3f(0, q, 1); break;
	case 4: glColor3f(f, 0, 1); break;
	case 5: glColor3f(1, 0, q); break;
	}
}

static void
init_pulse()
{
	pa_mainloop_api *api;

	if (!(pa.mainloop = pa_mainloop_new()))
		errpax("failed to create mainloop");

	api = pa_mainloop_get_api(pa.mainloop);

	if (!(pa.context = pa_context_new(api, "Vectorscope")))
		errpax("failed to create context");

	pa_context_set_state_callback(pa.context, handle_context_state, NULL);

	if (pa_context_connect(pa.context, NULL, 0, NULL) < 0)
		errpa("failed to connect");
}

static void
handle_context_state(pa_context *ctx, void *userdata UNUSED)
{
	switch (pa_context_get_state(ctx)) {
	case PA_CONTEXT_READY:
		pa_operation_unref(
			pa_context_get_server_info(ctx, handle_server_info, NULL)
		);
		break;
	case PA_CONTEXT_FAILED:
		errpa("failure in context");
	default:
		break;
	}
}

static void
handle_server_info(pa_context *ctx, const pa_server_info *info UNUSED, void *userdata UNUSED)
{
	static const pa_sample_spec ss = {
		.format = PA_SAMPLE_S16NE,
		.channels = 2,
		.rate = 44100
	};

	static const pa_buffer_attr ba = {
		.maxlength = BUFFER_SIZE,
		.fragsize = BUFFER_SIZE
	};

	if (!pa.sink && asprintf(&pa.sink, "%s.monitor", info->default_sink_name) < 0)
		err(EXIT_FAILURE, "failure in asprintf()");

	warnx("using sink %s", pa.sink);

	if (!(pa.stream = pa_stream_new(ctx, "Input", &ss, NULL)))
		errpa("failed to create stream");

	pa_stream_set_state_callback(pa.stream, handle_stream_state, NULL);
	pa_stream_set_read_callback(pa.stream, handle_stream_read, NULL);

	if (pa_stream_connect_record(pa.stream, pa.sink, &ba, PA_STREAM_ADJUST_LATENCY) < 0)
		errpa("failed to connect input stream");
}

static void
handle_stream_state(pa_stream *stream, void *userdata UNUSED)
{
	if (pa_stream_get_state(stream) == PA_STREAM_FAILED)
		errpa("failure in input stream");
}

static void
handle_stream_read(pa_stream *stream, size_t length, void *userdata UNUSED)
{
	static size_t buffer_index = 0;

	const void *data;

	if (pa_stream_peek(stream, &data, &length) < 0)
		errpa("failed to read fragment");

	if (data)
		for (size_t i = 0; i < length / 2; i++) {
			buffer[buffer_index] = ((int16_t *)data)[i];

			buffer_index++;

			if (buffer_index == BUFFER_SIZE)
				buffer_index = 0;
		}

	if (length > 0 && pa_stream_drop(stream))
		errpa("failed to drop fragment");
}