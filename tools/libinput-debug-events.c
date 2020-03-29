#include "config.h"

#include <errno.h>
#include <inttypes.h>
#include <getopt.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "linux/input.h"

#include <libinput.h>
#include <libevdev/libevdev.h>

#include "libinput-version.h"
#include "util-strings.h"
#include "util-macros.h"
#include "shared.h"

static uint32_t start_time;
static const uint32_t screen_width = 100;
static const uint32_t screen_height = 100;
static struct tools_options options;
static bool show_keycodes;
static volatile sig_atomic_t stop = 0;
static bool be_quiet = false;

struct TouchCoord
{
    double x, y;
};
static struct TouchCoord touch_buffer[5];

#define printq(...) ({ if (!be_quiet)  printf(__VA_ARGS__); })

static void
print_touch_event_with_coords(struct libinput_event *ev)
{
	struct libinput_event_touch *t = libinput_event_get_touch_event(ev);
	int32_t nb = libinput_event_touch_get_slot(t);
	double x = libinput_event_touch_get_x_transformed(t, screen_width);
	double y = libinput_event_touch_get_y_transformed(t, screen_height);

	if (nb < 0 || nb >= 5)
		return;

	touch_buffer[nb].x = x;
	touch_buffer[nb].y = y;

	for (int i = 0; i <= nb; i++)
		printq("[%d] %5.2fx%5.2f ", i, touch_buffer[i].x, touch_buffer[i].y);

	printq("\n");
}

static int
handle_and_print_events(struct libinput *li)
{
	int rc = -1;
	struct libinput_event *ev;

	libinput_dispatch(li);
	while ((ev = libinput_get_event(li))) {
		if (libinput_event_get_type(ev) == LIBINPUT_EVENT_TOUCH_DOWN) {
			print_touch_event_with_coords(ev);
		}
		
		libinput_event_destroy(ev);
		libinput_dispatch(li);
		rc = 0;
	}
	return rc;
}

static void
sighandler(int signal, siginfo_t *siginfo, void *userdata)
{
	stop = 1;
}

static void
mainloop(struct libinput *li)
{
	struct pollfd fds;

	fds.fd = libinput_get_fd(li);
	fds.events = POLLIN;
	fds.revents = 0;

	/* Handle already-pending device added events */
	if (handle_and_print_events(li))
		fprintf(stderr, "Expected device added events on startup but got none. "
				"Maybe you don't have the right permissions?\n");

	/* time offset starts with our first received event */
	if (poll(&fds, 1, -1) > -1) {
		struct timespec tp;

		clock_gettime(CLOCK_MONOTONIC, &tp);
		start_time = tp.tv_sec * 1000 + tp.tv_nsec / 1000000;
		do {
			handle_and_print_events(li);
		} while (!stop && poll(&fds, 1, -1) > -1);
	}

	printf("\n");
}

static void
usage(void) {
	printf("Usage: libinput debug-events [options] [--udev <seat>|--device /dev/input/event0 ...]\n");
}

int
main(int argc, char **argv)
{
	struct libinput *li;
	enum tools_backend backend = BACKEND_NONE;
	const char *seat_or_devices[60] = {NULL};
	size_t ndevices = 0;
	bool grab = false;
	bool verbose = false;
	struct sigaction act;

	tools_init_options(&options);

	while (1) {
		int c;
		int option_index = 0;
		enum {
			OPT_DEVICE = 1,
			OPT_UDEV,
			OPT_GRAB,
			OPT_VERBOSE,
			OPT_SHOW_KEYCODES,
			OPT_QUIET,
		};
		static struct option opts[] = {
			CONFIGURATION_OPTIONS,
			{ "help",                      no_argument,       0, 'h' },
			{ "show-keycodes",             no_argument,       0, OPT_SHOW_KEYCODES },
			{ "device",                    required_argument, 0, OPT_DEVICE },
			{ "udev",                      required_argument, 0, OPT_UDEV },
			{ "grab",                      no_argument,       0, OPT_GRAB },
			{ "verbose",                   no_argument,       0, OPT_VERBOSE },
			{ "quiet",                     no_argument,       0, OPT_QUIET },
			{ 0, 0, 0, 0}
		};

		c = getopt_long(argc, argv, "h", opts, &option_index);
		if (c == -1)
			break;

		switch(c) {
		case '?':
			exit(EXIT_INVALID_USAGE);
			break;
		case 'h':
			usage();
			exit(EXIT_SUCCESS);
			break;
		case OPT_SHOW_KEYCODES:
			show_keycodes = true;
			break;
		case OPT_QUIET:
			be_quiet = true;
			break;
		case OPT_DEVICE:
			if (backend == BACKEND_UDEV ||
			    ndevices >= ARRAY_LENGTH(seat_or_devices)) {
				usage();
				return EXIT_INVALID_USAGE;

			}
			backend = BACKEND_DEVICE;
			seat_or_devices[ndevices++] = optarg;
			break;
		case OPT_UDEV:
			if (backend == BACKEND_DEVICE ||
			    ndevices >= ARRAY_LENGTH(seat_or_devices)) {
				usage();
				return EXIT_INVALID_USAGE;

			}
			backend = BACKEND_UDEV;
			seat_or_devices[0] = optarg;
			ndevices = 1;
			break;
		case OPT_GRAB:
			grab = true;
			break;
		case OPT_VERBOSE:
			verbose = true;
			break;
		default:
			if (tools_parse_option(c, optarg, &options) != 0) {
				usage();
				return EXIT_INVALID_USAGE;
			}
			break;
		}

	}

	if (optind < argc) {
		if (backend == BACKEND_UDEV) {
			usage();
			return EXIT_INVALID_USAGE;
		}
		backend = BACKEND_DEVICE;
		do {
			if (ndevices >= ARRAY_LENGTH(seat_or_devices)) {
				usage();
				return EXIT_INVALID_USAGE;
			}
			seat_or_devices[ndevices++] = argv[optind];
		} while(++optind < argc);
	} else if (backend == BACKEND_NONE) {
		backend = BACKEND_UDEV;
		seat_or_devices[0] = "seat0";
	}

	memset(&act, 0, sizeof(act));
	act.sa_sigaction = sighandler;
	act.sa_flags = SA_SIGINFO;

	if (sigaction(SIGINT, &act, NULL) == -1) {
		fprintf(stderr, "Failed to set up signal handling (%s)\n",
				strerror(errno));
		return EXIT_FAILURE;
	}

	if (verbose)
		printf("libinput version: %s\n", LIBINPUT_VERSION);

	li = tools_open_backend(backend, seat_or_devices, verbose, &grab);
	if (!li)
		return EXIT_FAILURE;

	mainloop(li);

	libinput_unref(li);

	return EXIT_SUCCESS;
}
