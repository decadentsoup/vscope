// Copyright (C) 2018 Megan Ruggiero. All rights reserved.
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

#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>

#include <pulse/error.h>
#include <pulse/simple.h>

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

static const char HELP_NOTICE[] =
"Displays a vectorscope based on audio from the specified PulseAudio sink.\n"
"If no sink is specified, the default sink will be used.\n"
"\n"
"Options:\n"
"  --help      display this help message\n"
"  --version   display the version of this program\n"
"  --geometry  set window size to WIDTHxHEIGHT, position to +X+Y, or both to\n"
"              WIDTHxHEIGHT+X+Y; for negative positions, use - in place of +\n"
"\n"
"Report bugs to: <https://github.com/decadentsoup/vscope/issues>\n"
"Vectorscope home page: <https://github.com/decadentsoup/vscope>";

static struct { int x, y, w, h; } geometry = {SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, DEFAULT_WIDTH, DEFAULT_HEIGHT};
static const char *sink;
static SDL_Window *window;
static SDL_GLContext context;
static pthread_t thread;
static int16_t buffer[BUFFER_SIZE];
static bool run = true;

static void handle_exit(void);
static void parse_args(int, char **);
static void *sample(void *);

int
main(int argc, char **argv)
{
	unsigned int i, last_time;
	SDL_Event event;

	if (atexit(handle_exit))
		err(EXIT_FAILURE, "failed to register exit callback");

	parse_args(argc, argv);

	if (sink)
		warnx("using sink %s", sink);
	else
		warnx("using default sink");

	if (SDL_Init(SDL_INIT_VIDEO) < 0)
		errx(EXIT_FAILURE, "failed to initialize SDL: %s", SDL_GetError());

	if (!(window = SDL_CreateWindow("Vectorscope", geometry.x, geometry.y, geometry.w, geometry.h, SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE)))
		errx(EXIT_FAILURE, "failed to create window: %s", SDL_GetError());

	if (!(context = SDL_GL_CreateContext(window)))
		errx(EXIT_FAILURE, "failed to create context: %s", SDL_GetError());

	if ((errno = pthread_create(&thread, NULL, sample, NULL)))
		err(EXIT_FAILURE, "failed to create sampling thread");

	last_time = 0;

	for (;;) {
		if ((i = SDL_GetTicks()) > last_time + 16) {
			SDL_GL_SwapWindow(window);
			glClear(GL_COLOR_BUFFER_BIT);
			last_time = i;
		}

		glBegin(GL_POINTS);
		for (i = 0; i < BUFFER_SIZE; i += 2)
			glVertex2f(buffer[i] / 30000.0, buffer[i + 1] / 30000.0);
		glEnd();

		while (SDL_PollEvent(&event))
			if (event.type == SDL_QUIT) {
				run = false;
				pthread_join(thread, NULL);
				return 0;
			} else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_RESIZED) {
				glViewport(0, 0, event.window.data1, event.window.data2);
			}
	}
}

static void
handle_exit()
{
	SDL_GL_DeleteContext(context);
	SDL_DestroyWindow(window);
	SDL_Quit();
}

static void
parse_args(int argc, char **argv)
{
	static const struct option options[] = {
		{"help", no_argument, 0, 0},
		{"version", no_argument, 0, 0},
		{"geometry", required_argument, 0, 0},
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
				if (sscanf(optarg, "%ix%i%i%i", &geometry.w, &geometry.h, &geometry.x, &geometry.y) == 4) {
					// nothing more to do
				} else if (sscanf(optarg, "%ix%i", &geometry.w, &geometry.h) == 2) {
					geometry.x = SDL_WINDOWPOS_UNDEFINED;
					geometry.y = SDL_WINDOWPOS_UNDEFINED;
				} else if (sscanf(optarg, "%i%i", &geometry.x, &geometry.y) == 2) {
					geometry.w = DEFAULT_WIDTH;
					geometry.h = DEFAULT_HEIGHT;
				} else {
					warnx("invalid geometry argument (see --help)");
					fail = true;
				}
				break;
			}
		} else if (x == '?') {
			fail = true;
		}
	}

	optopt = argc - optind;
	if (optopt == 1) {
		sink = argv[optind];
	} else if (optopt > 0) {
		warnx("too many arguments");
		fail = true;
	}

	if (fail)
		errx(EXIT_FAILURE, "see --help for more information");
}

static void *
sample(void *argument UNUSED)
{
	static const pa_sample_spec sample_specification = {
		.format = PA_SAMPLE_S16NE,
		.channels = 2,
		.rate = 44100
	};

	static const pa_buffer_attr buffer_attributes = {
		.fragsize = BUFFER_SIZE / 2,
		.maxlength = BUFFER_SIZE
	};

	int error;
	pa_simple *pulse;

	pulse = pa_simple_new(
		NULL, // default server
		"Vectorscope",
		PA_STREAM_RECORD,
		sink,
		"Input",
		&sample_specification,
		NULL, // default channel map
		&buffer_attributes,
		&error
	);

	if (!pulse) {
		warn("failed to open default PulseAudio sink: %s", pa_strerror(error));
		return NULL;
	}

	while (run)
		if (pa_simple_read(pulse, buffer, sizeof(buffer), &error) < 0) {
			warn("failed to read from PulseAudio sink: %s", pa_strerror(error));
			pa_simple_free(pulse);
			return NULL;
		}

	if (pulse)
		pa_simple_free(pulse);

	return NULL;
}
