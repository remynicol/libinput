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
#include "libinput-debug_events.h"

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
static volatile sig_atomic_t stop = 0;
static char touch_buffer[5];
static struct Command* commands = NULL;

static char
coord_to_zone(double x, double y)
{
	if (y < 100.0 - x) {
	  	if (y < x)
	    	return 'h';
	  	else
	    	return 'g';
	} else {
		if (y < x)
	    	return 'd';
	  	else
	    	return 'b';
	}  
}

static void
event_to_command(int nb) {
	char* event = malloc(nb+2);
	strncpy(event, touch_buffer, nb+1);
	event[nb+1] = '\0';
	for (struct Command *cursor = commands; cursor; cursor = (struct Command*) cursor->next)
		if (strcmp(event, cursor->event) == 0) {
			system(cursor->action);
			printf("%s -> %s\n", cursor->event, cursor->action);
		}
	free(event);
}

static void
touch_event(struct libinput_event *ev)
{
	struct libinput_event_touch *t = libinput_event_get_touch_event(ev);
	int32_t nb = libinput_event_touch_get_slot(t);
	double x = libinput_event_touch_get_x_transformed(t, screen_width);
	double y = libinput_event_touch_get_y_transformed(t, screen_height);

	if (nb < 0 || nb >= 5)
		return;

	touch_buffer[nb] = coord_to_zone(x, y);
	if (nb > 0)
		event_to_command(nb);
}

static int
handle_and_manage_events(struct libinput *li)
{
	int rc = -1;
	struct libinput_event *ev;

	libinput_dispatch(li);
	while ((ev = libinput_get_event(li))) {
		if (libinput_event_get_type(ev) == LIBINPUT_EVENT_TOUCH_DOWN)
			touch_event(ev);
		
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
	if (handle_and_manage_events(li))
		fprintf(stderr, "Expected device added events on startup but got none. "
				"Maybe you don't have the right permissions?\n");

	/* time offset starts with our first received event */
	if (poll(&fds, 1, -1) > -1) {
		struct timespec tp;

		clock_gettime(CLOCK_MONOTONIC, &tp);
		start_time = tp.tv_sec * 1000 + tp.tv_nsec / 1000000;
		do {
			handle_and_manage_events(li);
			usleep(100*1000);
		} while (!stop && poll(&fds, 1, -1) > -1);
	}

	printf("\n");
}

static void
usage(void) {
	printf("Usage: tap_to_command [options] [--cmd 'xx-touch /tmp/test' [with x in 'gdbh' between 2 and 5 times]] [--udev <seat>|--device /dev/input/event0 ...]\n");
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
			OPT_CMD,
		};
		static struct option opts[] = {
			CONFIGURATION_OPTIONS,
			{ "help",                      no_argument,       0, 'h' },
			{ "device",                    required_argument, 0, OPT_DEVICE },
			{ "udev",                      required_argument, 0, OPT_UDEV },
			{ "grab",                      no_argument,       0, OPT_GRAB },
			{ "verbose",                   no_argument,       0, OPT_VERBOSE },
			{ "cmd",                   	   required_argument, 0, OPT_CMD },
			{ 0, 0, 0, 0}
		};

		c = getopt_long(argc, argv, "h", opts, &option_index);
		if (c == -1)
			break;

		struct Command *candidate = malloc(sizeof(commands));
		bool splitter_found, break_for;
		int i;
		switch(c) {
		case '?':
			exit(EXIT_INVALID_USAGE);
			break;
		case 'h':
			usage();
			exit(EXIT_SUCCESS);
			break;
		case OPT_CMD:
			splitter_found = false;
			break_for = false;
			for (i = 0; optarg[i] != '\0' && !break_for; i++) {
				if (optarg[i] == 'g' || optarg[i] == 'h' || optarg[i] == 'b' || optarg[i] == 'd')
					continue;
				if (optarg[i] == '-' && i >= 2 && i <= 5) {
					splitter_found = true;
					candidate->event = malloc(i+1);
					strncpy(candidate->event, optarg, i);
					candidate->event[i] = '\0';
				}
				break_for = true;
			}
			if (!splitter_found) {
				usage();
				return EXIT_INVALID_USAGE;
			}
			char* action = optarg + i;
			candidate->action = malloc(strlen(action) + 1);
			strcpy(candidate->action, action);
			candidate->next = (void*) commands;
			commands = candidate;
			break;
		case OPT_DEVICE:
			if (backend == BACKEND_UDEV || ndevices >= ARRAY_LENGTH(seat_or_devices)) {
				usage();
				return EXIT_INVALID_USAGE;
			}
			backend = BACKEND_DEVICE;
			seat_or_devices[ndevices++] = optarg;
			break;
		case OPT_UDEV:
			if (backend == BACKEND_DEVICE || ndevices >= ARRAY_LENGTH(seat_or_devices)) {
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

	for (struct Command *cursor = commands; cursor; cursor = (struct Command*) cursor->next)
		printf("config: %s -> %s\n", cursor->event, cursor->action);

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
