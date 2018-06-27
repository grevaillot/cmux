/**
*	Cmux
*	Enables GSM 0710 multiplex using n_gsm
*
*	Copyright (C) 2013 - Rtone - Nicolas Le Manchet <nicolaslm@rtone.fr> and others
*
*	This program is free software: you can redistribute it and/or modify
*	it under the terms of the GNU General Public License as published by
*	the Free Software Foundation, either version 3 of the License, or
*	(at your option) any later version.
*
*	This program is distributed in the hope that it will be useful,
*	but WITHOUT ANY WARRANTY; without even the implied warranty of
*	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*	GNU General Public License for more details.
*
*	You should have received a copy of the GNU General Public License
*	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*/
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>

#include <fcntl.h>
#include <termios.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/types.h>
#include <unistd.h>
#include <err.h>
#include <signal.h>
#include <ctype.h>

/**
*	gsmmux.h provides n_gsm line dicipline structures and functions.
*	It should be kept in sync with your kernel release.
*/
#include "gsmmux.h"

/* n_gsm ioctl */
#ifndef N_GSM0710
# define N_GSM0710	21
#endif

/* attach a line discipline ioctl */
#ifndef TIOCSETD
# define TIOCSETD	0x5423
#endif

 /* size of the reception buffer which gets data from the serial line */
#define SIZE_BUF	256

char *g_type = "default";

/* number of virtual TTYs to create (most modems can handle up to 4) */
int g_nodes = 1;

/* remove dangling virtual TTYs at start */
bool g_remove_nodes_at_start = 0;

/* name of the virtual TTYs to create */
char *g_base = "/dev/ttyGSM";

/* name of the driver, used to get the major number */
char *g_driver = "gsmtty";

/**
* whether or not to detach the program from the terminal
*	0 : do not daemonize
*	1 : daemonize
*/
int g_daemon = 1;

/**
* whether or not to print debug messages to stderr
*	0 : debug off
*	1 : debug on
*/
int g_debug = 1;

/**
* show version and exit
*/
bool g_version = false;
/* serial port of the modem */
char *g_device = "/dev/ttyUSB0";

/* line speed */
int g_speed = 115200;

/* maximum transfert unit (MTU), value in bytes */
int g_mtu = 512;

/* multiplexed baudrate, 5 = 115200, 6=230400, 7=1Mbit*/
int g_mux_baudrate = 6;

/**
*	Prints debug messages to stderr if debug is wanted
*/
static void dbg(char *fmt, ...) {
	va_list args;

	if (g_debug) {
		fflush(NULL);
		va_start(args, fmt);
		vfprintf(stderr, fmt, args);
		va_end(args);
		fprintf(stderr, "\n");
		fflush(NULL);
	}
	return;
}

static char *trim_rn(char *str)
{
	char *s = str;

	char *p = s;
	while (*p) {
		if ((*p != '\r') && (*p != '\n')) {
			*s = *p;
			s++;
		}
		p++;
	}
	*s = '\0';

	return str;
}

static ssize_t read_timeout(int fd, void *buf, size_t count, struct timeval *timeout)
{
	fd_set set;

	FD_ZERO(&set);
	FD_SET(fd, &set);

	int ret = select(fd + 1, &set, NULL, NULL, timeout);
	if (ret == -1) {
		return -1;
	} else if(ret == 0) {
		return 0;
	}

	return read(fd, buf, count);
}


/**
*	Sends an AT command to the specified line and gets its result
*	Returns  0 on success
*			-1 on failure
*/

static int at_sync_cmdget(int fd, char *at_cmd, char *result, size_t result_len)
{
	int r = -1;

	if ((fd < 0) || (!at_cmd) || (!result))
		return -1;

	char b;
	int flushed = 0;
	while (read(fd, &b, 1) > 0) {
		flushed++;
	}

	int at_sync_cmdlen = strlen(at_cmd);
	if (at_cmd[at_sync_cmdlen - 1] != '\r') {
		dprintf(fd, "%s\r", at_cmd);
	} else {
		dprintf(fd, "%s", at_cmd);
	}

	fsync(fd);

	dbg("%s: wrote %s", __FUNCTION__, at_cmd);

	char buf[result_len];
	size_t buf_content = 0;
	bzero(buf, result_len);

	bool do_read = true;
	while (do_read) {
		struct timeval tout = { .tv_sec = 0, .tv_usec = 500 * 1000 };
		int ret = read_timeout(fd, buf + buf_content, sizeof(buf) - (buf_content + 1), &tout);

		if (ret < 0) {
			warn("%s: could not read %s\n", __FUNCTION__, at_cmd);
			do_read = false;
		} else if (ret == 0) {
			dbg("%s: got 0 or timeout", __FUNCTION__);
			do_read = false;
		} else {
			buf_content += ret;

			char *ptr;
			if ((ptr = strstr(buf, "OK"))) {
				dbg("%s: got OK", __FUNCTION__);
				do_read = false;
				r = 0;
				*ptr = 0;
			} else if ((ptr =  strstr(buf, "ERROR"))) {
				do_read = false;
				r = 1;
				dbg("%s: got ERROR on %s\n"
						"----\n"
						"%s\n"
						"----"
						, __FUNCTION__, at_cmd, buf);
				*ptr = 0;
			}
		}
	}

	if (r < 0)
		return r;

	trim_rn(buf);
	bzero(result, result_len);
	strncpy(result, buf, result_len - 1);
	return r;
}

static int at_sync_cmdok(int fd, char *at_cmd)
{
	char buf[SIZE_BUF];
	return at_sync_cmdget(fd, at_cmd, buf, sizeof(buf));
}

static int send_at_command(int serial_fd, char *command)
{
	char buf[SIZE_BUF] = {0,};
	int ret = at_sync_cmdget(serial_fd, command, buf, sizeof(buf));
	dbg("%s: %s->%s, ret %d", __FUNCTION__, command, buf, ret);
	return ret;
}

/**
*	Function raised by signal catching
*/
void signal_callback_handler(int signum) {
	return;
}

/**
*	Gets the major number of the driver device
*	Returns  the major number on success
*			-1 on failure
*/
int get_major(char *driver) {
	FILE *fp;
	char *line = NULL;
	size_t len = 0;
	ssize_t read;
	char device[20];
	int major = -1;

	/* open /proc/devices file */
	if ((fp = fopen("/proc/devices", "r")) == NULL)
		err(EXIT_FAILURE, "Cannot open /proc/devices");

	/* read the file line by line */
	while ((major == -1) && (read = getline(&line, &len, fp)) != -1) {

		/* if the driver name string is found in the line, try to get the major */
		if (strstr(line, driver) != NULL) {
			if (sscanf(line,"%d %s\n", &major, device) != 2)
				major = -1;
		}

		/* free the line before getting a new one */
		if (line) {
			free(line);
			line = NULL;
		}
	}

	/* close /proc/devices file */
	fclose(fp);

	return major;
}

/**
*	Creates nodes for the virtual TTYs
*	Returns the number of nodes created
*/
int make_nodes(int major, char *basename, int nodes_count) {
	int minor, created = 0;
	dev_t device;
	char node_name[15];
	mode_t oldmask;

	/* set a new mask to get 666 mode and stores the old one */
	oldmask = umask(0);

	for (minor = 1; minor <= nodes_count; minor++) {

		/* append the minor number to the base name */
		sprintf(node_name, "%s%d", basename, minor);

		/* store a device info with major and minor */
		device = makedev(major, minor);

		/* create the actual character node */
		if (mknod(node_name, S_IFCHR | 0666, device) != 0) {
			warn("Cannot create %s", node_name);
		} else {
			created++;
			dbg("Created %s", node_name);
		}
	}

	/* revert the mask to the old one */
	umask(oldmask);

	return created;
}

/**
*	Removes previously created TTY nodes
*	Returns nothing, it doesn't really matter if it fails
*/
void remove_nodes(char *basename, int nodes_count) {
	char node_name[15];
	int node;

	for (node = 1; node <= nodes_count; node++) {

		/* append the minor number to the base name */
		sprintf(node_name, "%s%d", basename, node);

		/* unlink the actual character node */
		dbg("Removing %s", node_name);
		if (unlink(node_name) == -1)
			warn("Cannot remove %s", node_name);
	}

	return;
}

int match(const char *arg, const char *opt) {
	return (arg == opt) || (arg && (strcmp(arg, opt) == 0));
}

int parse_num(char *str, const char *opt) {
	char* end = NULL;
	int n = strtol(str, &end, 10);
	if (*end || n < 0)
		errx(EXIT_FAILURE, "Invalid number for option %s: %s", opt, str);

	return n;
}

char* parse_string(char *str, const char *opt) {
	if (str == NULL)
		errx(EXIT_FAILURE, "Argument missing for option %s", opt);

	return str;
}

int handle_string_arg(char **args, char**val, const char *opt) {
	if (match(args[0], opt)) {
		*val = parse_string(args[1], opt);
		return 1;
	}
	return 0;
}

int handle_number_arg(char **args, int *val, const char *opt) {
	if (match(args[0], opt)) {
		char *str = parse_string(args[1], opt);
		*val = parse_num(str, opt);
		return 1;
	}
	return 0;
}

int handle_set_flag_arg(char **args, bool *val, const char *opt) {
	if (match(args[0], opt)) {
		*val = true;
		return 1;
	}
	return 0;
}

void print_help() {
	printf(
		"Usage: cmux --device /dev/ttyUSB0 --speed 115200\n\n"
		"--version	print version\n"
		"--type <type>	SIM900, TELIT or default. (Default: %s)\n"
		"--device <name>	Serial device name. (Default: %s)\n"
		"--speed <rate>	Serial device line speed. (Default: %d)\n"
		"--mtu <number>	MTU size. (Default: %d)\n"
		"--debug [1|0]	Enable debugging. (Default: %d)\n"
		"--daemon [1|0]	Fork into background. (Default: %d)\n"
		"--driver <name>	Driver to use. (Default: %s)\n"
		"--base <name>	Base name for the nodes. (Default: %s)\n"
		"--nodes [0-4]	Number of nodes to create. (Default: %d)\n"
		"--remove_nodes Remove danglings nodes at start.\n"
		"\n",
		g_type, g_device, g_speed, g_mtu, g_debug,
		g_daemon, g_driver, g_base, g_nodes
	);
}

int to_line_speed(int speed) {
	switch(speed) {
		case 2400: return B2400;
		case 4800: return B4800;
		case 9600: return B9600;
		case 19200: return B19200;
		case 38400: return B38400;
		case 57600: return B57600;
		case 115200: return B115200;
		case 230400: return B230400;
		case 460800: return B460800;
		case 921600: return B921600;
		default:
			errx(EXIT_FAILURE, "Invalid value for speed: %d", speed);
	}
}

// string lower case
char *to_lower(const char *str) {
	int i;

	if (str == NULL)
		return NULL;

	char *s = strdup(str);
	for (i = 0; i < strlen(s); ++i) {
		s[i] = tolower(s[i]);
	}

	return s;
}

int main(int argc, char **argv) {
	int serial_fd, major, speed, i;
	struct termios tio;
	int ldisc = N_GSM0710;
	struct gsm_config gsm;
	char atcommand[40];

	for (i = 1; i < argc; ++i) {
		char **args = &argv[i];

		if (match(args[0], "-h")) {
			print_help();
			return 0;
		}

		if
		(
			handle_string_arg(args, &g_type, "--type")
			|| handle_string_arg(args, &g_device, "--device")
			|| handle_number_arg(args, &g_speed, "--speed")
			|| handle_number_arg(args, &g_mtu, "--mtu")
			|| handle_number_arg(args, &g_debug, "--debug")
			|| handle_number_arg(args, &g_daemon, "--daemon")
			|| handle_number_arg(args, &g_nodes, "--nodes")
			|| handle_set_flag_arg(args, &g_version, "--version")
			|| handle_set_flag_arg(args, &g_remove_nodes_at_start, "--remove_nodes")
			|| handle_string_arg(args, &g_driver, "--driver")
			|| handle_string_arg(args, &g_base, "--base")
		)
			i++;
		else
			errx(EXIT_FAILURE, "Unknown argument: %s", args[0]);
	}

	speed = to_line_speed(g_speed);
	g_type = to_lower(g_type);

	if (g_version) {
		dbg( "version: %s:%s\n" __DATE__, __TIME__);
		exit(EXIT_SUCCESS);
	}
	if (strcmp(g_type, "default") && strcmp(g_type, "sim900") && strcmp(g_type, "telit"))
		errx(EXIT_FAILURE, "Invalid value for --type: %s", g_type);

	if (g_daemon != 0 && g_daemon != 1)
		errx(EXIT_FAILURE, "Invalid value for --daemon: %d", g_daemon);

	if (g_debug != 0 && g_debug != 1)
		errx(EXIT_FAILURE, "Invalid value for --debug: %d", g_debug);

	if (g_nodes > 4)
		errx(EXIT_FAILURE, "Invalid value for --nodes: %d , must be < 5.", g_nodes);

	if (match(g_type, "sim900"))
		g_mtu = 255;

	/* print global parameters */
	dbg(
		"version: %s:%s\n"
		"type: %s\n"
		"device: %s\n"
		"speed: %d\n"
		"mtu: %d\n"
		"debug: %d\n"
		"daemon: %d\n"
		"driver: %s\n"
		"base: %s\n"
		"nodes: %d\n"
		"remove nodes: %s\n",
		__DATE__, __TIME__,
		g_type, g_device, g_speed, g_mtu, g_debug,
		g_daemon, g_driver, g_nodes ? g_base : "disabled", g_nodes,
		g_remove_nodes_at_start ? "true" : "false"
	);

	/* open the serial port */
	serial_fd = open(g_device,  O_RDWR | O_NOCTTY | O_SYNC | O_NONBLOCK);
	if (serial_fd == -1)
		err(EXIT_FAILURE, "Cannot open %s", g_device);

	/* get the current attributes of the serial port */
	if (tcgetattr(serial_fd, &tio) == -1)
		err(EXIT_FAILURE, "Cannot get line attributes");

	/* set the new attributes */
	tio.c_iflag = 0;
	tio.c_oflag = 0;
	tio.c_cflag = CS8 | CREAD | CLOCAL;
	tio.c_cflag |= CRTSCTS;
	tio.c_lflag = 0;
	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 0;

	/* write the speed of the serial line */
	if (cfsetospeed(&tio, speed) < 0 || cfsetispeed(&tio, speed) < 0)
		err(EXIT_FAILURE, "Cannot set line speed");

	/* write the attributes */
	if (tcsetattr(serial_fd, TCSANOW, &tio) == -1)
		err(EXIT_FAILURE, "Cannot set line attributes");


	int retry = 5;
	while (true) {
		dbg("%s: attempting to ping module...\n", __FUNCTION__);

		// disable echo and stuff
		dprintf(serial_fd, "ATE0\r\n");
		fsync(serial_fd);

		if (at_sync_cmdok(serial_fd, "AT") == 0) {
			dbg("%s: got OK", __FUNCTION__);
			break;
		}

		retry--;

		if (retry == 0)
			errx(EXIT_FAILURE, "no response, abort");

		dbg("%s: wait and retry...", __FUNCTION__);
		sleep(1);
	}

	/**
	*	Send AT commands to put the modem in CMUX mode.
	*	This is vendor specific and should be changed
	*	to fit your modem needs.
	*	The following matches Quectel M95.
	*/

	if (match(g_type, "sim900")) {
		if (send_at_command(serial_fd, "AAAT\r"))
			errx(EXIT_FAILURE, "AAAAT: bad response");
	}

	if (match(g_type, "telit")) {
		if (send_at_command(serial_fd, "AT#SELINT=2\r"))
			errx(EXIT_FAILURE, "AT#SELINT=2: bad response");

		if (send_at_command(serial_fd, "ATE0V1&K3&D2\r"))
			errx(EXIT_FAILURE, "ATE0V1&K3&D2: bad response");

		sprintf(atcommand, "AT+IPR=%d\r", g_speed);
		if (send_at_command(serial_fd, atcommand))
			errx(EXIT_FAILURE, "AT+IPR=%d: bad response", g_speed);

		if (send_at_command(serial_fd, "AT#CMUXMODE=0\r"))
			errx(EXIT_FAILURE, "AT#CMUXMODE=0: bad response");

		send_at_command(serial_fd, "AT+CMUX=0\r");
	} else {
		if (!match(g_type, "default"))
			if (send_at_command(serial_fd, "AT+IFC=2,2\r"))
				errx(EXIT_FAILURE, "AT+IFC=2,2: bad response");

		if (send_at_command(serial_fd, "AT+GMM\r"))
			warnx("AT+GMM: bad response");

		if (send_at_command(serial_fd, "AT\r"))
			warnx("AT: bad response");

		if (!match(g_type, "sim900") && !match(g_type, "default")) {
			sprintf(atcommand, "AT+IPR=%d&w\r", g_speed);
			if (send_at_command(serial_fd, atcommand) == -1)
				errx(EXIT_FAILURE, "AT+IPR=%d&w: bad response", g_speed);
		}

		sprintf(atcommand, "AT+CMUX=0,0,%d,%d,10,3,30,10,2\r", g_mux_baudrate, g_mtu);
		if (send_at_command(serial_fd, atcommand))
			errx(EXIT_FAILURE, "Cannot enable modem CMUX");
	}

	/* use n_gsm line discipline */
	if (match(g_type, "sim900")) {
		sleep(0);
	}

	if (ioctl(serial_fd, TIOCSETD, &ldisc) < 0)
		err(EXIT_FAILURE, "Cannot set N_GSM0710 line discipline. Is 'n_gsm' kernel module registered?");

	/* get n_gsm configuration */
	if (ioctl(serial_fd, GSMIOC_GETCONF, &gsm) < 0)
		err(EXIT_FAILURE, "Cannot get GSM multiplex parameters");

	/* set and write new attributes */
	gsm.initiator = 1;
	gsm.encapsulation = 0;
	gsm.mru = g_mtu;
	gsm.mtu = g_mtu;
	gsm.t1 = 10;
	gsm.n2 = 3;
	gsm.t2 = 30;
	gsm.t3 = 10;

	if (ioctl(serial_fd, GSMIOC_SETCONF, &gsm) < 0)
		err(EXIT_FAILURE, "Cannot set GSM multiplex parameters");
	dbg("Line discipline set.");

	if (g_remove_nodes_at_start) {
		if (g_nodes > 0)
			remove_nodes(g_base, g_nodes);
	}

	/* create the virtual TTYs */
	if (g_nodes > 0) {
		int created;
		if ((major = get_major(g_driver)) < 0)
			errx(EXIT_FAILURE, "Cannot get major number");
		if ((created = make_nodes(major, g_base, g_nodes)) < g_nodes)
			warnx("Cannot create all nodes, only %d of %d have been created.", created, g_nodes);
		if (created == 0) {
			warnx("No nodes have been created.");
		}
	}

	/* detach from the terminal if needed */
	if (g_daemon) {
		dbg("Going to background");
		if (daemon(0,0) != 0)
			err(EXIT_FAILURE, "Cannot daemonize");
	}

	/* wait to keep the line discipline enabled, wake it up with a signal */
	signal(SIGINT, signal_callback_handler);
	signal(SIGTERM, signal_callback_handler);

	pause();

	/* remove the created virtual TTYs */
	if (g_nodes > 0)
		remove_nodes(g_base, g_nodes);

	/* close the serial line */
	close(serial_fd);

	return EXIT_SUCCESS;
}
