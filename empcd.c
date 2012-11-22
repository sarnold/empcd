/***********************************************************
 EMPCd - Event Music Player Client daemon
 by Jeroen Massar <jeroen@unfix.org>
************************************************************
 $Author: jeroen $
 $Id: $
 $Date: $
***********************************************************/

#include "empcd.h"

#define EMPCD_VERSION "2005.12.12"
#define EMPCD_VSTRING "empcd %s by Jeroen Massar <jeroen@unfix.org>\n"

/* MPD functions */
#include "support/mpc-0.11.2/src/libmpdclient.h"
#define MPD_HOST_DEFAULT "localhost"
#define MPD_PORT_DEFAULT "6600"

struct empcd_events	events[100];
unsigned int		maxevent = 0;
mpd_Connection		*mpd = NULL;
unsigned int		verbosity = 0, drop_uid = 0, drop_gid = 0;
bool			daemonize = true;
bool			running = true;
bool			exclusive = true;
char			*mpd_host = NULL, *mpd_port = NULL;

/* When we receive a signal, we abort */
void handle_signal(int i)
{
	running = false;
	signal(i, &handle_signal);
}

void dologA(int level, const char *fmt, va_list ap)
{
	char buf[8192];

	if (level == LOG_DEBUG && verbosity < 1) return;

	vsnprintf(buf, sizeof(buf), fmt, ap);

	if (daemonize) syslog(LOG_LOCAL7|level, buf);
	else
	{
		FILE *out = (level == LOG_DEBUG || level == LOG_ERR ? stderr : stdout);
		fprintf(out, "[%6s] ",
			level == LOG_DEBUG ?    "debug" :
			(level == LOG_ERR ?     "error" :
			(level == LOG_WARNING ? "warn" :
			(level == LOG_NOTICE ?  "notice" :
			(level == LOG_INFO ?    "info" : "(!?)")))));
		fprintf(out, buf);
	}
}

void dolog(int level, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	dologA(level, fmt, ap);
	va_end(ap);
}

mpd_Connection *empcd_setup()
{
	int		iport;
	char		*test;
	int		port_env = 0;
	int		host_env = 0;
	int		password_len = 0;
	int		parsed_len = 0;
	mpd_Connection	*mpd = NULL;
	
	iport = strtol(mpd_port, &test, 10);
	if (iport <= 0 || test[0] != '\0')
	{
		dolog(LOG_ERR, "MPD_PORT \"%s\" is not a positive integer\n", mpd_port);
		return NULL;
	}

	/* parse password and host */
	test = strstr(mpd_host,"@");
        password_len = test-mpd_host;
	if (test) parsed_len++;

	if (!test) password_len = 0;
        if (test && password_len != 0) parsed_len += password_len;

	mpd = mpd_newConnection(mpd_host+parsed_len, iport, 10);
	if (!mpd) return NULL;

	if (mpd->error)
	{
		dolog(LOG_ERR, "MPD Connection Error: %s\n", mpd->errorStr);
		return NULL;
	}

	if (password_len)
	{
		char *pass = strdup(mpd_host);
		pass[password_len] = '\0';
		mpd_sendPasswordCommand(mpd, pass);
		mpd_finishCommand(mpd);
		free(pass);

		if (mpd->error)
		{
			dolog(LOG_ERR, "MPD Authentication Error: %s\n", mpd->errorStr);
			return NULL;
		}
	}

	return mpd;
}

bool mpd_check()
{
	if (!mpd->error) return false;

	/* Ignore timeouts */
	if (mpd->error != MPD_ERROR_CONNCLOSED)
	{
		dolog(LOG_WARNING, "MPD error: %s\n", mpd->errorStr);
	}

	/* Don't reconnect for non-fatal errors */
	if (mpd->error < 10 && mpd->error > 19)
	{
		return false;
	}

	/* Close the old connection */
	mpd_closeConnection(mpd);

	/* Setup a new connection */
	mpd = empcd_setup();
	if (!mpd)
	{
		dolog(LOG_ERR, "MPD Connection Lost, exiting\n");
		exit(0);
	}
	return true;
}

mpd_Status *empcd_status()
{
	int retry = 5;
	mpd_Status *s = NULL;

	while (retry > 0)
	{
		retry--;

		mpd_sendStatusCommand(mpd);
		if (mpd_check()) continue;

		s = mpd_getStatus(mpd);
		if (mpd_check()) continue;

		mpd_finishCommand(mpd);
		if (mpd_check()) continue;

		break;
	}

	return s;
}

/********************************************************************/

void f_exec(char *arg)
{
	system(arg);
}

#define F_CMDM(fn, f) \
void fn(char *arg)\
{\
	int retry = 5;\
\
	while (retry > 0)\
	{\
		retry--;\
		f;\
		if (mpd_check()) continue;\
		mpd_finishCommand(mpd);\
		if (mpd_check()) continue;\
		break;\
	}\
}

#define F_CMD(fn, f) F_CMDM(fn,f(mpd))

F_CMD(f_next, mpd_sendNextCommand)
F_CMD(f_prev, mpd_sendPrevCommand)
F_CMD(f_stop, mpd_sendStopCommand)
F_CMDM(f_play, mpd_sendPlayCommand(mpd,-1))

void f_seek(char *arg)
{
	int	dir = 0, seekto = 0, i = 0, retry = 5;
	bool	perc = false;

	mpd_Status *status = empcd_status(mpd);
	if (!status) return;

	if (arg[0] == '-')	{ i++; dir = -1; }
	else if (arg[0] == '+') { i++; dir = +1; }
	seekto = strlen(&arg[i]);
	if (arg[i+seekto] == '%') perc = true;

	seekto = atoi(&arg[i]);

	if (perc)
	{
		if (dir != 0) seekto = status->elapsedTime + ((status->totalTime * seekto / 100) * dir);
		else seekto = (status->totalTime * perc / 100);
	}
	else
	{
		if (dir != 0) seekto = status->elapsedTime + (seekto*dir);
		/* dir == 0 case is set correctly above */
	}

	/*
	 * Take care of limits
	 * (end-10 so that one can search till the end easily)
	 */
	if (seekto < 0 || seekto > (status->totalTime-10)) return;

	while (retry > 0)
	{
		retry--;
		mpd_sendSeekIdCommand(mpd, status->songid, seekto);
		if (mpd_check()) continue;
		mpd_finishCommand(mpd);
		if (mpd_check()) continue;
		break;
	}

	mpd_freeStatus(status);
}

void f_pause(char *arg)
{
	int retry = 5, mode = 0;
	if (!arg)
	{
		/* Toggle the random mode */
		mpd_Status *status = empcd_status(mpd);
		if (!status) return;

		mode = (status->state == MPD_STATUS_STATE_PAUSE ? MPD_STATUS_STATE_PLAY : MPD_STATUS_STATE_PAUSE);
		mpd_freeStatus(status);
	}
	else if (strcasecmp(arg, "on" ) == 0) mode = MPD_STATUS_STATE_PAUSE;
	else if (strcasecmp(arg, "off") == 0) mode = MPD_STATUS_STATE_PLAY;

	while (retry > 0)
	{
		retry--;
		mpd_sendPauseCommand(mpd, mode);
		if (mpd_check()) continue;
		mpd_finishCommand(mpd);
		if (mpd_check()) continue;
		break;
	}
}

void f_random(char *arg)
{
	int retry = 5, mode = 0;

	if (!arg)
	{
		/* Toggle the random mode */
		mpd_Status *status = empcd_status(mpd);
		if (!status) return;

		mode = !status->random;
		mpd_freeStatus(status);
	}
	else if (strcasecmp(arg, "on" ) == 0) mode = 1;
	else if (strcasecmp(arg, "off") == 0) mode = 0;

	while (retry > 0)
	{
		retry--;
		mpd_sendRandomCommand(mpd, mode);
		if (mpd_check()) continue;
		mpd_finishCommand(mpd);
		if (mpd_check()) continue;
		break;
	}
}

struct empcd_funcs
{
	char *name;
	void (*function)(char *arg);
	char *format;
	char *label;
} func_map[] =
{
	{"EXEC",	f_exec,		"exec <shellcmd>",		"Execute a command"},
	{"MPD_NEXT",	f_next,		"mpd_next",			"MPD Next Track"},
	{"MPD_PREV",	f_prev,		"mpd_prev",			"MPD Previous Track"},
	{"MPD_STOP",	f_stop,		"mpd_stop",			"MPD Stop Playing"},
	{"MPD_PLAY",	f_play,		"mpd_play",			"MPD Start Playing"},
	{"MPD_PAUSE",	f_pause,	"mpd_pause [on|off]",		"MPD Pause Toggle or Set"},
	{"MPD_SEEK",	f_seek,		"mpd_seek [+|-]<val>[%]",	"MPD Seek direct or relative (+|-) percentage when ends in %"},
	{"MPD_RANDOM",	f_random,	"mpd_random [on|off]",		"MPD Random Toggle or Set"},
	{NULL,		NULL,		NULL,				"undefined"}
};

/********************************************************************/

bool set_event(uint16_t type, uint16_t code, int32_t value, void (*action)(char *arg), char *args)
{
	if (maxevent >= (sizeof(events)/sizeof(events[0])))
	{
		dolog(LOG_ERR, "Maximum number of events reached\n");
		return false;
	}

	events[maxevent].type = type;
	events[maxevent].code = code;
	events[maxevent].value = value;
	events[maxevent].action = action;
	events[maxevent].args = args ? strdup(args) : args;

	maxevent++;
	return true;
}

/*
	KEY_KPSLASH RELEASE f_seek -1
	<key> <value> <action> <arg>
*/

bool set_event_from_map(char *buf, struct empcd_mapping *event_map, struct empcd_mapping *value_map)
{
	unsigned int i, o = 0, len = strlen(buf), l, event = 0, value = 0, func = 0;
	void (*what)(char *arg);
	char *arg = NULL;

	for (i=0; event_map[i].code != EMPCD_MAPPING_END; i++)
	{
		l = strlen(event_map[i].name);
		if (len < o+l || buf[o+l] != ' ') continue;
		if (strncasecmp(&buf[o], event_map[i].name, l) == 0) break;
	}

	if (event_map[i].code == EMPCD_MAPPING_END)
	{
		dolog(LOG_DEBUG, "Undefined Code at %u in '%s'\n", o, buf);
		return false;
	}
	event = i;

	o += l+1;
	for (i=0; value_map[i].code != EMPCD_MAPPING_END; i++)
	{
		l = strlen(value_map[i].name);
		if (len < o+l || buf[o+l] != ' ') continue;
		if (strncasecmp(&buf[o], value_map[i].name, l) == 0) break;
	}

	if (value_map[i].code == EMPCD_MAPPING_END)
	{
		dolog(LOG_DEBUG, "Undefined Key Value at %u in '%s'\n", o, buf);
		return false;
	}
	value = i;

	o += l+1;
	for (i=0; func_map[i].name != NULL; i++)
	{
		l = strlen(func_map[i].name);
		if (len != o+l && (len < o+l || buf[o+l] != ' ')) continue;
		if (strncasecmp(&buf[o], func_map[i].name, l) == 0) break;
	}

	if (func_map[i].name == NULL)
	{
		dolog(LOG_DEBUG, "Undefined Function at %u in '%s'\n", o, buf);
		return false;
	}
	func = i;

	o += l+1;
	if (len > o) arg = &buf[o];

	dolog(LOG_DEBUG, "Mapping Event %s (%s) %s (%s) to do %s (%s) with arg %s\n",
		event_map[event].name, event_map[event].label,
		value_map[value].name, value_map[value].label,
		func_map[func].name, func_map[func].label,
		arg ? arg : "<none>");

	return set_event(EV_KEY, event_map[event].code, value_map[value].code, func_map[func].function, arg);
}

/********************************************************************/

/*
	 0 = failed to open file
	 1 = all okay
	-1 = error parsing file
*/
int readconfig(char *cfgfile, char **device)
{
	unsigned int line = 0;

	FILE *f = fopen(cfgfile, "r");

	dolog(LOG_DEBUG, "ReadConfig(%s) = %s\n", cfgfile, f ? "ok" : "error");

	if (!f) return 0;

	while (!feof(f))
	{
		char buf[1024], buf2[1024];
		unsigned int n, i = 0, j = 0;

		line++;

		if (fgets(buf2, sizeof(buf2), f) == 0) break;
		n = strlen(buf2)-1;

		/*
		 * Trim whitespace
		 * - Translate \t to space
		 * - strip multiple whitespaces
		 * Saves handling them below
		 */
		for (i=0,j=0;i<n;i++)
		{
			if (buf2[i] == '\t') buf2[i] = ' ';
			if ((i == 0 || (i > 0 && buf2[i-1] == ' ')) &&
				(buf2[i] == ' ' || buf2[i] == '\t'))
			{
				continue;
			}
			buf[j++] = buf2[i];
		}
		/* Trim trailing space if it is there */
		if (j>0 && buf[j-1] == ' ') j--;
		/* Terminate our new constructed string */
		buf[j] = '\0';
		n = j;

		/* Empty or comment line? */
		if (	n == 0 ||
			buf[0] == '#' ||
			(buf[0] == '/' && buf[1] == '/'))
		{
			continue;
		}

		dolog(LOG_DEBUG, "%s@%04u: %s", cfgfile, line, buf);

		if (strncasecmp("mpd_host ", buf, 9) == 0)
		{
			dolog(LOG_DEBUG, "Setting MPD_HOST to %s\n", &buf[9]);
			if (mpd_host) free(mpd_host);
			mpd_host = strdup(&buf[9]);
		}
		else if (strncasecmp("mpd_port ", buf, 9) == 0)
		{
			dolog(LOG_DEBUG, "Setting MPD_PORT to %s\n", &buf[9]);
			if (mpd_port) free(mpd_port);
			mpd_port = strdup(&buf[9]);
		}
		else if (strncasecmp("eventdevice ", buf, 12) == 0)
		{
			if (*device) free(*device);
			*device = strdup(&buf[12]);
		}
		else if (strncasecmp("exclusive ", buf, 10) == 0)
		{
			if (strncasecmp("on", &buf[10], 2) == 0) exclusive = true;
			else if (strncasecmp("off", &buf[10], 3) == 0) exclusive = false;
			else
			{
				dolog(LOG_ERR, "Exclusive is either 'on' or 'off'\n");
				return -1;
			}
		}
		else if (strncasecmp("exclusive", buf, 9) == 0)
		{
			exclusive = true;
		}
		else if (strncasecmp("nonexclusive", buf, 12) == 0)
		{
			exclusive = false;
		}
		else if (strncasecmp("key ", buf, 4) == 0)
		{
			if (!set_event_from_map(&buf[4], key_event_map, key_value_map)) return -1;
		}
		else if (strncasecmp("user ", buf, 5) == 0)
		{
			struct passwd *passwd;

			/* setuid()+setgid() to another user+group */
			passwd = getpwnam(&buf[5]);
			if (passwd)
			{
				drop_uid = passwd->pw_uid;
				drop_gid = passwd->pw_gid;
			}
			else
			{
				dolog(LOG_ERR, "Couldn't find user %s\n", optarg);
				return -1;
			}
		}
		else
		{
			dolog(LOG_ERR, "Unrecognized configuration line %u: %s\n", line, buf);
			return -1;
		}
	}

	fclose(f);

	return 1; 
}

/* Long options */
static struct option const long_options[] = {
	{"config",		required_argument,	NULL, 'c'},
	{"daemonize",		no_argument,		NULL, 'd'},
	{"nodaemonize",		no_argument,		NULL, 'f'},
	{"eventdevice",		required_argument,	NULL, 'e'},
	{"help",		no_argument,		NULL, 'h'},
	{"list-keys",		no_argument,		NULL, 'K'},
	{"list-functions",	no_argument,		NULL, 'L'},
	{"quiet",		no_argument,		NULL, 'q'},
	{"user",		required_argument,	NULL, 'u'},
	{"verbose",		no_argument,		NULL, 'v'},
	{"version",		no_argument,		NULL, 'V'},
	{"verbosity",		required_argument,	NULL, 'y'},
	{"exclusive",		no_argument,		NULL, 'x'},
	{"nonexclusive",	no_argument,		NULL, 'X'},
	{NULL,			no_argument,		NULL, 0},
};

static char short_options[] = "c:de:fhKLqu:vVy:";

static struct
{
	char *args;
	char *desc;
} desc_options[] =
{
	{"<file>",		"Configuration File Location"},
	{NULL,			"Detach the program into the background"},
	{NULL,			"Don't detach, stay in the foreground"},
	{"<eventdevice>",	"The event device to use, default: /dev/input/event0"},
	{NULL,			"This help"},
	{NULL,			"List the keys that are known to this program"},
	{NULL,			"List the functions known to this program"},
	{NULL,			"Lower the verbosity level to 0 (quiet)"},
	{"<username>",		"Drop priveleges to <user>"},
	{NULL,			"Increase the verbosity level by 1"},
	{NULL,			"Show the version of this program"},
	{"<level>",		"Set the verbosity level to <level>"},
	{NULL,			"Exclusive device access (default)"},
	{NULL,			"Non-Exclusive device access"},
	{NULL,			NULL}
};

int main (int argc, char **argv)
{
	int			fd = -1, option_index, j;
	char			*device = NULL, *cfgfile = NULL, *conffile = NULL, *t;
	struct input_event	ev, prev;
	struct empcd_events	*evt;
	unsigned int		i, repeat = 0;

	while ((j = getopt_long(argc, argv, short_options, long_options, &option_index)) != EOF)
	{
		switch (j)
		{
		case 'c':
			if (conffile) free(conffile);
			conffile = strdup(optarg);
			break;
		case 'd':
			daemonize = true;
			break;
		case 'f':
			daemonize = false;
			break;
		case 'e':
			if (device) free(device);
			device = strdup(optarg);
			break;
		case 'h':
			fprintf(stderr, "usage: %s\n", argv[0]);
			for (i=0; long_options[i].name; i++)
			{
				char buf[3] = "  ";
				if (long_options[i].val != 0)
				{
					buf[0] = '-';
					buf[1] = long_options[i].val;
				}
				fprintf(stderr, "%2s -%-15s %-15s %s\n",
					buf,
					long_options[i].name,
					desc_options[i].args ? desc_options[i].args : "",
					desc_options[i].desc ? desc_options[i].desc : "");
			}
			return 1;
		case 'L':
			for (i=0; func_map[i].name; i++)
			{
				fprintf(stderr, "%-25s %s\n", func_map[i].format, func_map[i].label);
			}
			return 0;
		case 'K':
			for (i=0; key_event_map[i].code != EMPCD_MAPPING_END; i++)
			{
				fprintf(stderr, "%-25s %s\n", key_event_map[i].name, key_event_map[i].label);
			}
			return 0;
		case 'q':	
			verbosity++;
			break;
		case 'u':
			{
				struct passwd *passwd;
				/* setuid()+setgid() to another user+group */
				passwd = getpwnam(optarg);
				if (passwd)
				{
					drop_uid = passwd->pw_uid;
					drop_gid = passwd->pw_gid;
				}
				else
				{
					dolog(LOG_ERR, "Couldn't find user %s\n", optarg);
					return 1;
				}
			}
			break;
		case 'v':
			verbosity++;
			break;
		case 'y':
			verbosity = atoi(optarg);
			break;
		case 'V':
			fprintf(stderr, EMPCD_VSTRING, EMPCD_VERSION);
			return 1;
		case 'x':
			exclusive = true;
			break;
		case 'X':
			exclusive = false;
			break;
		default:
			if (j != 0) fprintf(stderr, "Unknown short option '%c'\n", j);
			else fprintf(stderr, "Unknown long option\n");
			fprintf(stderr, "See '%s -h' for help\n", argv[0]);
			return 1;
		}
	}

	dolog(LOG_INFO, EMPCD_VSTRING, EMPCD_VERSION);

	if (!device) device = strdup("/dev/input/event0");

	if ((t = getenv("MPD_HOST"))) mpd_host = strdup(t);
	else mpd_host = strdup(MPD_HOST_DEFAULT);
	if ((t = getenv("MPD_PORT"))) mpd_port = strdup(t);
	else mpd_port = strdup(MPD_PORT_DEFAULT);

	if (!conffile)
	{
		/* Try user's config */
		cfgfile = getenv("HOME");
		if (cfgfile)
		{
			char buf[256];
			snprintf(buf, sizeof(buf), "%s/%s", cfgfile, ".empcd.conf");
			cfgfile = conffile = strdup(buf);
			j = readconfig(cfgfile, &device);
		}
		else j = 0;
		if (j == 0)
		{
			cfgfile = "/etc/empcd.conf";
			j = readconfig(cfgfile, &device);
		}
	}
	else
	{
		/* Try specified config */
		cfgfile = conffile;
		j = readconfig(cfgfile, &device);
	}

	if (j != 1)
	{
		if (j == 0) dolog(LOG_ERR, "Configuration file '%s' not found\n", cfgfile);
		else if (j == -1) dolog(LOG_ERR, "Parse error in configuration file '%s'\n", cfgfile);
		if (conffile) free(conffile);
		if (device) free(device);
		return 1;
	}
	if (conffile) free(conffile);

	if (daemonize)
	{
		j = fork();
		if (j < 0)
		{
			dolog(LOG_ERR, "Couldn't fork for daemonization\n");
			return 1;
                }

		/* Exit the mother fork */
		if (j != 0) return 0;

		/* Child fork */
		setsid();

		/* Cleanup stdin/out/err */
		freopen("/dev/null","r",stdin);
		freopen("/dev/null","w",stdout);
		freopen("/dev/null","w",stderr);
	}

	/* Handle these signals for a clean exit */
	signal(SIGHUP,  &handle_signal);
	signal(SIGTERM, &handle_signal);
	signal(SIGINT,  &handle_signal);
	signal(SIGKILL, &handle_signal);

	/* Ignore some odd signals */
	signal(SIGILL,  SIG_IGN);
	signal(SIGABRT, SIG_IGN);
	signal(SIGUSR1, SIG_IGN);
	signal(SIGUSR2, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGSTOP, SIG_IGN);
	signal(SIGTSTP, SIG_IGN);
	signal(SIGTTIN, SIG_IGN);
	signal(SIGTTOU, SIG_IGN);
        signal(SIGUSR1, SIG_IGN);
        signal(SIGUSR2, SIG_IGN);

	/* Try to open the device */
	fd = open(device, O_RDONLY);
	if (fd < 0)
	{
		perror("Couldn't open event device");
		return 1;
	}
	free(device);
	device = NULL;

	/* Obtain Exclusive device access */
	if (exclusive) ioctl(fd, EVIOCGRAB, 1);

	/* Setup MPD connectivity */
	mpd = empcd_setup();
	if (!mpd)
	{
		dolog(LOG_ERR, "Couldn't contact MPD server\n");
		return 1;
	}

	/*
	 * Drop our root priveleges.
	 * We don't need them anymore anyways
	 */
	if (drop_uid != 0)
	{
		dolog(LOG_INFO, "Dropping userid to %u...\n", drop_uid);
		setuid(drop_uid);
	}
	if (drop_gid != 0)
	{
		dolog(LOG_INFO, "Dropping groupid to %u...\n", drop_gid);
		setgid(drop_gid);
	}

	while (running)
	{
		struct timeval	tv;
		fd_set		fdread;

		FD_ZERO(&fdread);
		FD_SET(fd, &fdread);
		tv.tv_sec = 5;
		tv.tv_usec = 0;
		j = select(fd+1, &fdread, NULL, NULL, &tv);
		if (j == 0) continue;
		if (j < 0 || read(fd, &ev, sizeof(ev)) == -1) break;

		/* Slow down repeated KEY repeat's */
		if (	ev.type == EV_KEY)
		{
			if (	ev.value == EV_KEY_REPEAT &&
				ev.type == prev.type &&
				ev.code == prev.code &&
				ev.value == prev.value)
			{
				if (repeat >= 5) repeat = 0;
				else
				{
					repeat++;
					continue;
				}
			}
			else
			{
				repeat = 0;
				prev.type = ev.type;
				prev.code = ev.code;
				prev.value = ev.value;
			}
		}

		/* Lookup the code in our table */
		i = 0;
		while (	i < maxevent && (
			events[i].type != ev.type ||
			events[i].code != ev.code ||
			events[i].value != ev.value))
		{
			i++;
		}

		if (i < maxevent) evt = &events[i];
		else evt = NULL;

		if (	(evt != NULL && verbosity > 2) ||
			(evt == NULL && verbosity > 5)  )
		{
			char			buf[1024];
			unsigned int		n = 0;
			struct empcd_mapping	*map = NULL, *val = NULL;
			struct empcd_funcs	*func = func_map;

			if (ev.type == EV_KEY)
			{
				map = key_event_map;
				val = key_value_map;
			}

			if (map)
			{
				for (i=0; map[i].code != EMPCD_MAPPING_END && map[i].code != ev.code; i++);
				map = &map[i];
			}

			if (val)
			{
				for (i=0; val[i].code != EMPCD_MAPPING_END && val[i].code != ev.value; i++);
				val = &val[i];
			}

			if (evt)
			{
				for (i=0; func[i].name != NULL && func[i].function != evt->action; i++);
				func = &func[i];
			}

			n += snprintf(&buf[n], sizeof(buf)-n, "Event: T%lu.%06lu, type %u, code %u, value %d",
				ev.time.tv_sec, ev.time.tv_usec, ev.type,
				ev.code, ev.value);

			if (map)
			{
				n += snprintf(&buf[n], sizeof(buf)-n, ": %s, name: %s, label: %s",
					val ? val->name : "<unknown value>",
					map->name ? map->name : "<unknown name>",
					map->label ? map->label : "");
			}

			if (evt)
			{
				n += snprintf(&buf[n], sizeof(buf)-n, ", action: %s(%s)",
					func->name ? func->name : "?",
					evt->args ? evt->args : "");
			}

			dolog(LOG_DEBUG, "%s\n", buf);
		}

		if (evt == NULL) continue;
		evt->action(evt->args);
	}

	dolog(LOG_INFO, "empcd shutting down\n");

	mpd_closeConnection(mpd);

	close(fd);
	return 0;
}
