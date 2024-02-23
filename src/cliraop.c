/*
 * RAOP : Client to control an AirPlay device, cli simple example
 *
 * Copyright (C) 2004 Shiro Ninomiya <shiron@snino.com>
 * Philippe <philippe_44@outlook.com>
 *
 * See LICENSE
 *
 */

#include <stdio.h>
#include <signal.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"

#include <sys/stat.h>
#include <sys/types.h>

#if WIN
#include <conio.h>
#include <time.h>
#else
#include <unistd.h>
#include <termios.h>
#include <sys/param.h>
#include <sys/time.h>
#if OSX || FREEBDS
#include <sys/resource.h>
#endif
#endif

#include "cross_thread.h"
#include "raop_client.h"
#include "cross_net.h"
#include "cross_ssl.h"
#include "cross_util.h"
#include "cross_log.h"
#include "http_fetcher.h"

#define RAOP_SEC(ntp) ((uint32_t)((ntp) >> 32))
#define RAOP_FRAC(ntp) ((uint32_t)(ntp))
#define RAOP_SECNTP(ntp) RAOP_SEC(ntp), RAOP_FRAC(ntp)

bool startsWith(const char *pre, const char *str)
{
	size_t lenpre = strlen(pre),
		   lenstr = strlen(str);
	return lenstr < lenpre ? false : memcmp(pre, str, lenpre) == 0;
}

// locals
uint32_t glNetmask;
char glInterface[16] = "?";
static struct in_addr glHost;
static bool glMainRunning = true;
static pthread_t glCmdPipeReaderThread;
char cmdPipeName[32];
int fd1;
char cmdPipeBuf[512];
int latency = MS2TS(1000, 44100);
struct
{
	char *title;
	char *artist;
	char *album;
	int duration;
	int progress;
} metadata = {0};
struct raopcl_s *raopcl;
enum
{
	STOPPED,
	PAUSED,
	PLAYING
} status;

// debug level from tools & other elements
log_level util_loglevel;
log_level raop_loglevel;
log_level main_log;

// our debug level
log_level *loglevel = &main_log;

// different combination of debug levels per channel
struct debug_s
{
	int main, raop, util;
} debug[] = {
	{lSILENCE, lSILENCE, lSILENCE},
	{lERROR, lERROR, lERROR},
	{lINFO, lERROR, lERROR},
	{lINFO, lINFO, lERROR},
	{lDEBUG, lERROR, lERROR},
	{lDEBUG, lINFO, lERROR},
	{lDEBUG, lDEBUG, lERROR},
	{lSDEBUG, lINFO, lERROR},
	{lSDEBUG, lDEBUG, lERROR},
	{lSDEBUG, lSDEBUG, lERROR},
};

/*----------------------------------------------------------------------------*/
static int print_usage(char *argv[])
{
	char *name = strrchr(argv[0], '\\');

	name = (name) ? name + 1 : argv[0];

	printf("usage: %s <options> <server_ip> <filename ('-' for stdin)>\n"
		   "\t[-ntp print current NTP and exit\n"
		   "\t[-p <port number>]\n"
		   "\t[-v <volume> (0-100)]\n"
		   "\t[-l <latency> (frames]\n"
		   "\t[-w <wait>]  (start after <wait> milliseconds)\n"
		   "\t[-n <start>] (start at NTP <start> + <wait>)\n"
		   "\t[-e] audio payload encryption\n"
		   "\t[-auth] <UDN> for authentication (only if crypto present in TXT record)\n"
		   "\t[-a] send ALAC compressed audio\n"
		   "\t[-s <secret>] (valid secret for AppleTV)\n"
		   "\t[-P <password>] (device password)\n"
		   "\t[-et <et>] (et field in mDNS - 4 for airport-express and used to detect MFi)\n"
		   "\t[-md <[0][,1][,2]>] (md in mDNS: metadata capabilties 0=text, 1=artwork, 2=progress)\n"
		   "\t[-d <debug level>] (0 = silent)\n",
		   name);
	return -1;
}

/*----------------------------------------------------------------------------*/
static void init_platform()
{
	netsock_init();
	cross_ssl_load();
}

/*----------------------------------------------------------------------------*/
static void close_platform()
{
	netsock_close();
	cross_ssl_free();
}

/*----------------------------------------------------------------------------*/
static void *CmdPipeReaderThread(void *args)
{
	fd1 = open(cmdPipeName, O_RDONLY);
	// Read and print line from named pipe
	while (glMainRunning)
	{
		if (!glMainRunning)
			break;

		if (read(fd1, cmdPipeBuf, 512) > 0)
		{
			// read lines
			char *save_ptr1, *save_ptr2;
			char *line = strtok_r(cmdPipeBuf, "\n", &save_ptr1);
			// loop through the string to extract all other tokens
			while (line != NULL)
			{
				if (!glMainRunning)
					break;

				LOG_DEBUG("Received line on fifo %s", line);
				// extract key-value pair within line
				char *key = strtok_r(line, "=", &save_ptr2);
				if (strlen(key) == 0)
					continue;
				char *value = strtok_r(NULL, "", &save_ptr2);
				if (value == NULL)
					value = "";

				if (strcmp(key, "TITLE") == 0)
				{
					metadata.title = value ? value : "";
				}
				else if (strcmp(key, "ARTIST") == 0)
				{
					metadata.artist = value ? value : "";
				}
				else if (strcmp(key, "ALBUM") == 0)
				{
					metadata.album = value ? value : "";
				}
				else if (strcmp(key, "DURATION") == 0)
				{
					metadata.duration = atoi(value);
				}
				else if (strcmp(key, "PROGRESS") == 0)
				{
					metadata.progress = atoi(value);
					raopcl_set_progress_ms(raopcl, metadata.progress * 1000, metadata.duration * 1000);
				}
				else if (strcmp(key, "ARTWORK") == 0)
				{
					if (startsWith("http://", value))
					{
						LOG_DEBUG("Downloading artwork from URL: %s", value);
						char *contentType;
						char *content;
						int size = http_fetch(value, &contentType, &content);
						if (size > 0 && glMainRunning)
						{
							LOG_INFO("Sending artwork to player...");
							raopcl_set_artwork(raopcl, contentType, size, content);
							free(content);
						}
						else
						{
							LOG_WARN("Unable to download artwork", value);
						}
					}
					else if (access(value, F_OK) == 0)
					{
						// local file
						LOG_DEBUG("Setting artwork from file: %s", value);
						FILE *infile;
						char *buffer;
						long numbytes;
						infile = fopen(value, "r");
						fseek(infile, 0L, SEEK_END);
						numbytes = ftell(infile);
						fseek(infile, 0L, SEEK_SET);
						buffer = (char *)calloc(numbytes, sizeof(char));
						fread(buffer, sizeof(char), numbytes, infile);
						fclose(infile);
						raopcl_set_artwork(raopcl, "image/jpg", numbytes, buffer);
						free(buffer);
					}
					else
					{
						LOG_WARN("Unable to process artwork path: %s", value);
					}
				}
				else if (strcmp(key, "VOLUME") == 0)
				{
					LOG_INFO("Setting volume to: %s", value);
					raopcl_set_volume(raopcl, raopcl_float_volume(atoi(value)));
				}
				else if (strcmp(key, "ACTION") == 0 && strcmp(value, "PAUSE") == 0)
				{
					if (status == PLAYING)
					{
						raopcl_pause(raopcl);
						raopcl_flush(raopcl);
						status = PAUSED;
						LOG_INFO("Pause at : %u.%u", RAOP_SECNTP(raopcl_get_ntp(NULL)));
					}
					else
					{
						LOG_WARN("Pause requested but player is already paused");
					}
				}
				else if (strcmp(key, "ACTION") == 0 && strcmp(value, "PLAY") == 0)
				{
					uint64_t now = raopcl_get_ntp(NULL);
					uint64_t start_at = now + MS2NTP(200) - TS2NTP(latency, raopcl_sample_rate(raopcl));
					status = PLAYING;
					raopcl_start_at(raopcl, start_at);
					LOG_INFO("Re-started at : %u.%u", RAOP_SECNTP(start_at));
				}
				else if (strcmp(key, "ACTION") == 0 && strcmp(value, "STOP") == 0)
				{
					status = STOPPED;
					LOG_INFO("Stopped at : %u.%u", RAOP_SECNTP(raopcl_get_ntp(NULL)));
					raopcl_stop(raopcl);
					break;
				}
				else if (strcmp(key, "ACTION") == 0 && strcmp(value, "SENDMETA") == 0)
				{
					LOG_INFO("Sending metadata: %p", metadata);
					raopcl_set_daap(raopcl, 4, "minm", 's', metadata.title,
									"asar", 's', metadata.artist,
									"asal", 's', metadata.album,
									"astn", 'i', 1);
				}

				// read next line in cmdPipeBuf
				line = strtok_r(NULL, "\n", &save_ptr1);
			}

			// clear cmdPipeBuf
			memset(cmdPipeBuf, 0, sizeof cmdPipeBuf);
		}
		else
		{
			usleep(250);
		}
	}

	return NULL;
}
/*																		  */
/*----------------------------------------------------------------------------*/
int main(int argc, char *argv[])
{
	char *glDACPid = "1A2B3D4EA1B2C3D4";
	char *activeRemote = "ap5918800d";
	char *fname = NULL;
	int port = 5000;
	char *passwd = NULL;
	int volume = 0, wait = 0;
	struct
	{
		struct hostent *hostent;
		char *name;
		struct in_addr addr;
	} player = {0};

	metadata.duration = 0;
	metadata.progress = 0;
	metadata.title = "";
	metadata.artist = "";
	metadata.album = "";
	int infile;
	uint8_t *buf;
	int i, n = -1, level = 3;
	raop_crypto_t crypto = RAOP_CLEAR;
	uint64_t start = 0, start_at = 0, last = 0, frames = 0;
	bool alac = false, encryption = false;
	char *secret = "", *md = "0,1,2", *et = "0,4";
	bool auth = false;

	char *iface = NULL;
	glHost = get_interface(!strchr(glInterface, '?') ? glInterface : NULL, &iface, &glNetmask);
	LOG_INFO("Binding to %s [%s] with mask 0x%08x", inet_ntoa(glHost), iface, ntohl(glNetmask));
	NFREE(iface);

	// parse arguments
	for (i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "-ntp"))
		{
			uint64_t t = raopcl_get_ntp(NULL);
			printf("%" PRIu64 "\n", t);
			exit(0);
		}
		if (!strcmp(argv[i], "-check"))
		{
			printf("cliraop check\n");
			exit(0);
		}
		if (!strcmp(argv[i], "-p"))
		{
			port = atoi(argv[++i]);
		}
		else if (!strcmp(argv[i], "-v"))
		{
			volume = atoi(argv[++i]);
		}
		else if (!strcmp(argv[i], "-w"))
		{
			wait = atoi(argv[++i]);
		}
		else if (!strcmp(argv[i], "-l"))
		{
			latency = MS2TS(atoi(argv[++i]), 44100);
		}
		else if (!strcmp(argv[i], "-s"))
		{
			secret = argv[++i];
		}
		else if (!strcmp(argv[i], "-md"))
		{
			md = argv[++i];
		}
		else if (!strcmp(argv[i], "-et"))
		{
			et = argv[++i];
		}
		else if (!strcmp(argv[i], "-dacp"))
		{
			glDACPid = argv[++i];
		}
		else if (!strcmp(argv[i], "-ar"))
		{
			activeRemote = argv[++i];
		}
		else if (!strcmp(argv[i], "-auth"))
		{
			auth = true;
		}
		else if (!strcmp(argv[i], "-a"))
		{
			alac = true;
		}
		else if (!strcmp(argv[i], "-n"))
		{
			sscanf(argv[++i], "%" PRIu64, &start);
		}
		else if (!strcmp(argv[i], "-d"))
		{
			level = atoi(argv[++i]);
			if (level >= sizeof(debug) / sizeof(struct debug_s))
			{
				level = sizeof(debug) / sizeof(struct debug_s) - 1;
			}
		}
		else if (!strcmp(argv[i], "-e"))
		{
			encryption = true;
			continue;
		}
		else if (!strcmp(argv[i], "-P"))
		{
			passwd = argv[++i];
			continue;
		}
		else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
		{
			return print_usage(argv);
		}
		else if (!player.name)
		{
			player.name = argv[i];
		}
		else if (!fname)
		{
			fname = argv[i];
		}
	}

	util_loglevel = debug[level].util;
	raop_loglevel = debug[level].raop;
	main_log = debug[level].main;

	if (!player.name)
		return print_usage(argv);
	if (!fname)
		return print_usage(argv);

	if (!strcmp(fname, "-"))
	{
		infile = fileno(stdin);
	}
	else if ((infile = open(fname, O_RDONLY)) == -1)
	{
		LOG_ERROR("cannot open file %s", fname);
		close_platform();
		exit(1);
	}

	// setup named pipe for metadata
	snprintf(cmdPipeName, sizeof(cmdPipeName), "/tmp/fifo-%s", activeRemote);
	LOG_INFO("Listening for commands on named pipe %s", cmdPipeName);
	mkfifo(cmdPipeName, 0666);
	init_platform();

	if ((encryption || auth) && strchr(et, '1'))
		crypto = RAOP_RSA;
	else
		crypto = RAOP_CLEAR;

	// create the raop context
	if ((raopcl = raopcl_create(glHost, 0, 0, glDACPid, activeRemote, alac ? RAOP_ALAC : RAOP_PCM, DEFAULT_FRAMES_PER_CHUNK,
								latency, crypto, auth, secret, passwd, et, md,
								44100, 16, 2,
								volume > 0 ? raopcl_float_volume(volume) : -144.0)) == NULL)
	{
		LOG_ERROR("Cannot init RAOP %p", raopcl);
		close_platform();
		exit(1);
	}

	// get player's address
	player.hostent = gethostbyname(player.name);
	if (!player.hostent)
	{
		LOG_ERROR("Cannot resolve name %s", player.name);
		goto exit;
	}

	memcpy(&player.addr.s_addr, player.hostent->h_addr_list[0], player.hostent->h_length);
	// connect to player
	if (!raopcl_connect(raopcl, player.addr, port, volume > 0))
	{
		LOG_ERROR("Cannot connect to AirPlay device %s:%hu, check firewall & port", inet_ntoa(player.addr), port);
		goto exit;
	}

	latency = raopcl_latency(raopcl);

	LOG_INFO("connected to %s on port %d, player latency is %d ms", inet_ntoa(player.addr),
			 port, (int)TS2MS(latency, raopcl_sample_rate(raopcl)));

	if (start || wait)
	{
		uint64_t now = raopcl_get_ntp(NULL);

		start_at = (start ? start : now) + MS2NTP(wait) -
				   TS2NTP(latency, raopcl_sample_rate(raopcl));

		LOG_INFO("now %u.%u, audio starts at NTP %u.%u (in %u ms)", RAOP_SECNTP(now), RAOP_SECNTP(start_at),
				 (start_at + TS2NTP(latency, raopcl_sample_rate(raopcl)) > now) ? (uint32_t)NTP2MS(start_at - now + TS2NTP(latency, raopcl_sample_rate(raopcl))) : 0);

		raopcl_start_at(raopcl, start_at);
	}

	pthread_create(&glCmdPipeReaderThread, NULL, CmdPipeReaderThread, NULL);

	start = raopcl_get_ntp(NULL);
	status = PLAYING;

	buf = malloc(DEFAULT_FRAMES_PER_CHUNK * 4);
	uint32_t KeepAlive = 0;

	do
	{
		uint64_t playtime, now;

		if (status == STOPPED)
			break;

		now = raopcl_get_ntp(NULL);

		// execute every second
		if (now - last > MS2NTP(1000))
		{
			last = now;
			uint32_t elapsed = TS2MS(frames - raopcl_latency(raopcl), raopcl_sample_rate(raopcl));
			if (frames && frames > raopcl_latency(raopcl))
			{
				LOG_INFO("elapsed milliseconds: %" PRIu64, elapsed);
			}

			// send keepalive when needed (to prevent stop playback on homepods)
			if (!(KeepAlive++ & 0x0f))
				raopcl_keepalive(raopcl);

			// send progress based on elapsed only if no progress exists in metadata
			if (metadata.progress == 0)
			{
				raopcl_set_progress_ms(raopcl, elapsed, metadata.duration * 1000);
			}
		}

		// send chunk if needed
		if (status == PLAYING && raopcl_accept_frames(raopcl))
		{
			n = read(infile, buf, DEFAULT_FRAMES_PER_CHUNK * 4);
			if (!n)
				continue;
			raopcl_send_chunk(raopcl, buf, n / 4, &playtime);
			frames += n / 4;
		}
		else
		{
			usleep(500);
		}

	} while (n || raopcl_is_playing(raopcl));

	glMainRunning = false;
	free(buf);
	raopcl_disconnect(raopcl);
	pthread_join(glCmdPipeReaderThread, NULL);

exit:
	close(fd1);
	unlink(cmdPipeName);
	raopcl_destroy(raopcl);
	close_platform();
	return 0;
}