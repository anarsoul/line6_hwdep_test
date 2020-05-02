/*
 *  Based on sbiload.c
 *
 *  Copyright (c) 2020 Vasily Khoruzhick <anarsoul@gmail.com>
 *  Copyright (c) 2007 Takashi Iwai <tiwai@suse.de>
 *  Copyright (c) 2000 Uros Bizjak <uros@kss-loka.si>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <alsa/asoundlib.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Options for the command */
#define HAS_ARG 1
static struct option long_opts[] = {
	{ "device", HAS_ARG, NULL, 'D' },
	{ "verbose", HAS_ARG, NULL, 'v' },
	{ "quiet", 0, NULL, 'q' },
	{ "version", 0, NULL, 'V' },
	{ 0, 0, 0, 0 },
};

/* Number of elements in an array */
#define NELEM(a) ( sizeof(a)/sizeof((a)[0]) )

/* Default verbose level */
static int quiet;
static int verbose = 0;

/* Global declarations */
static snd_hwdep_t *handle;
static int iface;

/* Function prototypes */
static void show_usage(void);
static int init_hwdep(const char *name);

bool interrupted = false;
unsigned char msg[8192];
ssize_t msg_size;

void sighandler(int signo)
{
	if (signo == SIGINT)
		interrupted  = true;
}

/*
 * Show usage message
 */
static void show_usage()
{
	char **cpp;
	static char *msg[] = {
		"Usage: hd500 [options]",
		"",
		"  -D, --device=name     - hwdep device string",
		"  -v, --verbose=level   - Verbose level (default = 0)",
		"  -q, --quiet           - Be quiet, no error/warning messages",
		"  -V, --version         - Show version",
	};

	for (cpp = msg; cpp < msg + NELEM(msg); cpp++) {
		printf("%s\n", *cpp);
	}
}

#define VERSION "1"

/*
 * Show version
 */
static void show_version()
{
	printf("Version: " VERSION "\n");
}

static void print_buf(unsigned char *buf, ssize_t start, ssize_t size)
{
	for (int i = 0; i < size; i += 16) {
		printf("%.4lx:", i + start);
		for (int j = i; (j < (i + 16)) && (j < size); j++)
			printf(" %.2x", buf[j]);
		printf("\n");
	}
}

static void reset_message(void)
{
	msg_size = 0;
}

static bool message_complete(void)
{
	int expected_size = (msg[0] + (msg[1] << 8)) * 4 + 4;
	if (expected_size == msg_size)
		return true;

	return false;
}

static void print_message(void)
{
	uint32_t *msg32 = (uint32_t *)msg;
	float *f;
	bool dump = false;
	char name[17];

	if (msg_size < 8) {
		dump = true;
		goto out;
	}
	switch (msg32[1]) {
	case 0x23004000:
		printf("preset changed\n");
		break;
	case 0x2c004000:
		printf("setlist: %d\n", msg32[2]);
		break;
	case 0x27004000:
		printf("preset: %d\n", msg32[2]);
		break;
	case 0x35004000:
		f = (float *)&msg32[4];
		printf("pedal: %f\n", (double)*f);
		break;
	case 0x16004000:
		f = (float *)&msg32[5];
		printf("tempo: %f\n", (double)*f);
		break;
	case 0x01004000:
		memcpy(name, &msg[8], 16);
		name[16] = '\0';
		printf("preset: %s\n", name);
		break;
	case 0x13004000:
		printf("Footswitch %d %s\n", msg32[3],
		       msg32[4] ? "enabled" : "disabled");
		break;
	default:
		dump = true;
		break;
	}

out:
	if (dump)
		print_buf(msg, 0, msg_size);
}

static void read_message(void)
{
	ssize_t ret;
	unsigned char buf[8192];

	do {
		ret = snd_hwdep_read(handle, buf, sizeof(buf));
		if (ret > 0) {
			int data_size = buf[0] + (buf[1] << 8);
			if (data_size != ret - 4) {
				printf("Got bogus packet, dump:\n");
				print_buf(buf, 0, ret);
				reset_message();
				continue;
			}

			memcpy(msg + msg_size, buf + 4, data_size);
			msg_size += data_size;

			if (message_complete()) {
				print_message();
				reset_message();
			}
		}
	} while (ret >= 0);
}

#if 0
static int get_current_preset_idx(void)
{
	unsigned char cmd[] =
	    { 0x0c, 0x00, 0x01, 0x00, 0x02, 0x00, 0x0a, 0x40,
	      0x01, 0x05, 0x00, 0x21, 0x08, 0x00, 0x00, 0x00
	};
	ssize_t ret;
	ret = snd_hwdep_write(handle, cmd, sizeof(cmd));
	if (ret != sizeof(cmd)) {
		if (!quiet)
			fprintf(stderr, "Unable to write cmd: %s\n",
				snd_strerror(ret));
		return -1;
	}

	return 0;
}
#endif

/*
 * Open a hwdep device
 */
static int open_hwdep(const char *name)
{
	int err;
	snd_hwdep_info_t *info;

	if ((err =
	     snd_hwdep_open(&handle, name,
			    SND_HWDEP_OPEN_DUPLEX | SND_HWDEP_OPEN_NONBLOCK)) <
	    0)
		return err;

	snd_hwdep_info_alloca(&info);
	if (!snd_hwdep_info(handle, info)) {
		iface = snd_hwdep_info_get_iface(info);
		if (iface == SND_HWDEP_IFACE_LINE6)
			return 0;
	}
	snd_hwdep_close(handle);
	handle = NULL;
	return -EINVAL;
}

static int init_hwdep(const char *name)
{
	char tmpname[16];

	if (!name || !*name) {
		/* auto probe */
		int card = -1;
		snd_ctl_t *ctl;

		while (!snd_card_next(&card) && card >= 0) {
			int dev;
			sprintf(tmpname, "hw:%d", card);
			if (snd_ctl_open(&ctl, tmpname, 0) < 0)
				continue;
			dev = -1;
			while (!snd_ctl_hwdep_next_device(ctl, &dev)
			       && dev >= 0) {
				sprintf(tmpname, "hw:%d,%d", card, dev);
				if (!open_hwdep(tmpname)) {
					snd_ctl_close(ctl);
					return 0;
				}
			}
			snd_ctl_close(ctl);
		}
		if (!quiet)
			fprintf(stderr, "Can't find any PODHD hwdep device\n");
		return -1;
	}

	return 0;
}

/*
 * Unsubscribe client from destination port
 * and close sequencer
 */
static void finish_hwdep()
{
	snd_hwdep_close(handle);
	handle = NULL;
}

int main(int argc, char **argv)
{
	char opts[NELEM(long_opts) * 2 + 1];
	char *name;
	char *cp;
	int c;
	struct option *op;

	/* Build up the short option string */
	cp = opts;
	for (op = long_opts; op < &long_opts[NELEM(long_opts)]; op++) {
		*cp++ = op->val;
		if (op->has_arg)
			*cp++ = ':';
	}

	name = NULL;

	/* Deal with the options */
	for (;;) {
		c = getopt_long(argc, argv, opts, long_opts, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'D':
			name = optarg;
			break;
		case 'q':
			quiet = 1;
			verbose = 0;
			break;
		case 'v':
			quiet = 0;
			verbose = atoi(optarg);
			break;
		case 'V':
			show_version();
			exit(1);
		case '?':
			show_usage();
			exit(1);
		}
	}

	signal(SIGINT, sighandler);

	if (init_hwdep(name) < 0) {
		return 1;
	}

	//get_current_preset_idx();

	struct pollfd pfd;
	snd_hwdep_poll_descriptors(handle, &pfd, 1);

	while (!interrupted) {
		int pollrc = poll(&pfd, 1, 1000);
		if (pollrc < 0) {
			perror("poll");
			break;
		}
		if (pfd.revents & (POLLIN | POLLRDNORM))
			read_message();
		if (pfd.revents & (POLLERR | POLLHUP))
			break;
	}

	finish_hwdep();
	return 0;
}
