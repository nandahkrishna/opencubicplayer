/* OpenCP Module Player
 * copyright (c) 1994-'10 Niklas Beisert <nbeisert@physik.tu-muenchen.de>
 * copyright (c) 2005-'22 Stian Skjelstad <stian.skjelstad@gmail.com>
 *
 * Timidity glue logic
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include "config.h"
#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "timidity.h"
#include "controls.h"
#include "instrum.h"
#include "output.h"
#include "playmidi.h"
#include "readmidi.h"
#include "wrd.h"
#include "aq.h"
#include "recache.h"
#include "resample.h"
#include "arc.h"
#include "boot/psetting.h"
#include "dev/deviplay.h"
#include "dev/mcp.h"
#include "dev/player.h"
#include "dev/plrasm.h"
#include "filesel/mdb.h"
#include "stuff/err.h"
#include "stuff/imsrtns.h"
#include "stuff/poll.h"

#include "timidityplay.h"

#include "timidity-git/timidity/playmidi.c"

#define RC_ASYNC_HACK 0x31337

#ifdef TIMIDITY_DEBUG
# define PRINT(fmt, args...) fprintf(stderr, "%s %s: " fmt, __FILE__, __func__, ##args)
#else
# define PRINT(a, ...) do {} while(0)
#endif

static int inpause;

static int bal;
static int vol;
static unsigned long voll,volr;
static int pan;
static int srnd;
static uint32_t speed;
static uint32_t dspeed;
static int loading;

/* devp pre-buffer zone */
static uint16_t *buf16 = 0; /* stupid dump that should go away */
/* devp buffer zone */
static uint32_t bufpos; /* devp write head location */
static uint32_t buflen; /* devp buffer-size in samples */
static int output_counter;
static void *plrbuf; /* the devp buffer */
static int stereo; /* boolean */
static int bit16; /* boolean */
static int signedout; /* boolean */
static int reversestereo; /* boolean */
static int donotloop=1;

/* timidityIdler dumping locations */
static uint8_t *gmibuf = 0;     /* the buffer */
static uint32_t gmibuflen;  /* total buffer size */
/*static uint32_t aylen;*/     /* expected wave length */
static uint32_t gmibufhead; /* actually this is the write head */
static uint32_t gmibuftail;  /* read pos */
static uint32_t gmibuffpos; /* read fine-pos.. when rate has a fraction */
static uint32_t gmibuffill = 0;
static int32_t gmibuffree;
static uint32_t gmibufrate = 0x10000; /* re-sampling rate.. fixed point 0x10000 => 1.0 */

static int (*_GET)(int ch, int opt);
static void (*_SET)(int ch, int opt, int val);

#define PANPROC \
do { \
	float _rs = rs, _ls = ls; \
	if(pan==-64) \
	{ \
		float t=_ls; \
		_ls = _rs; \
		_rs = t; \
	} else if(pan==64) \
	{ \
	} else if(pan==0) \
		_rs=_ls=(_rs+_ls) / 2.0; \
	else if(pan<0) \
	{ \
		_ls = _ls / (-pan/-64.0+2.0) + _rs*(64.0+pan)/128.0; \
		_rs = _rs / (-pan/-64.0+2.0) + _ls*(64.0+pan)/128.0; \
	} else if(pan<64) \
	{ \
		_ls = _ls / (pan/-64.0+2.0) + _rs*(64.0-pan)/128.0; \
		_rs = _rs / (pan/-64.0+2.0) + _ls*(64.0-pan)/128.0; \
	} \
	rs = _rs * volr / 256.0; \
	ls = _ls * voll / 256.0; \
	if (srnd) \
	{ \
		ls ^= 0xffff; \
	} \
} while(0)

/* clipper threadlock since we use a timer-signal */
static volatile int clipbusy=0;
static char *current_path=0;

static int gmi_eof;
static int gmi_looped;

static int ctl_next_result = RC_NONE;
static int ctl_next_value = 0;

/* timidity.c */
extern char *opt_aq_max_buff;
extern char *opt_aq_fill_buff;
extern int   opt_aq_fill_buff_free_needed;

/* main interfaces (To be used another main) */
#if defined(main) || defined(ANOTHER_MAIN) || defined ( IA_W32GUI ) || defined ( IA_W32G_SYN )
#define MAIN_INTERFACE
#else
#define MAIN_INTERFACE static
#endif /* main */
MAIN_INTERFACE void timidity_start_initialize(void);
MAIN_INTERFACE int read_config_file(char *name, int self, int allow_missing_file);
MAIN_INTERFACE int timidity_post_load_configuration(void);
MAIN_INTERFACE void timidity_init_player(void);
MAIN_INTERFACE int timidity_play_main(int nfiles, char **files);
extern int got_a_configuration;
MAIN_INTERFACE void tmdy_free_config(void);
extern void init_effect(void);
void timidity_init_aq_buff(void);

struct _CtlEventDelayed;
typedef struct _CtlEventDelayed
{
	struct _CtlEventDelayed *next;
	int delay_samples;
	CtlEvent event;
} CtlEventDelayed;

static CtlEventDelayed *EventDelayed_PlrBuf_head = 0;
static CtlEventDelayed *EventDelayed_PlrBuf_tail = 0;
int                     eventDelayed_PlrBuf_lastpos;

static CtlEventDelayed *EventDelayed_gmibuf_head = 0;
static CtlEventDelayed *EventDelayed_gmibuf_tail = 0;

static void free_EventDelayed (CtlEventDelayed *self)
{
	if (self->event.type == CTLE_PROGRAM)
	{
		free ((char *)self->event.v3);
		self->event.v3 = 0;
	}
	free (self);
}

struct mchaninfo channelstat[16] = {0};

void __attribute__ ((visibility ("internal"))) timidityGetChanInfo(uint8_t ch, struct mchaninfo *ci)
{
	assert (ch < 16);
	*ci = channelstat[ch];
}

static void timidity_apply_EventDelayed (CtlEvent *event)
{
	switch (event->type)
	{
		case CTLE_NOTE:
			if ((event->v2 < 0) || (event->v2 >= 16))
			{
				return; /* Outside the normal MIDI range */
			}
			if (event->v1 == 2) /* NOTE-ON */
			{
				int i;
				for (i=0; i < channelstat[event->v2].notenum; i++)
				{
					if (channelstat[event->v2].note[i] == event->v3)
					{
						PRINT ("RESTART ch=%ld note=%ld\n", event->v2, event->v3);
						channelstat[event->v2].vol[i] = event->v4;
						channelstat[event->v2].opt[i] = 1; /* no idea */
						return; /* already playing? */
					}
				}

				if (channelstat[event->v2].notenum >= 32)
				{
					return; /* FULL */
				}

				for (i=0; i < (channelstat[event->v2].notenum); i++)
				{
					if (channelstat[event->v2].note[i] > event->v3)
					{ /* INSERT */
						PRINT ("INSERT ch=%ld note=%ld\n", event->v2, event->v3);
						memmove (channelstat[event->v2].note + i + 1, channelstat[event->v2].note + i, channelstat[event->v2].notenum - i);
						memmove (channelstat[event->v2].vol + i + 1, channelstat[event->v2].vol + i, channelstat[event->v2].notenum - i);
						memmove (channelstat[event->v2].opt + i + 1, channelstat[event->v2].opt + i, channelstat[event->v2].notenum - i);

						channelstat[event->v2].note[i] = event->v3;
						channelstat[event->v2].vol[i] = event->v4;
						channelstat[event->v2].opt[i] = 1; /* no idea */
						channelstat[event->v2].notenum++;
						return;
					}
				}

				/* APPEND */
				PRINT ("APPEND ch=%ld note=%ld\n", event->v2, event->v3);
				channelstat[event->v2].note[channelstat[event->v2].notenum] = event->v3;
				channelstat[event->v2].vol[channelstat[event->v2].notenum] = event->v4;
				channelstat[event->v2].opt[channelstat[event->v2].notenum] = 1; /* no idea */
				channelstat[event->v2].notenum++;
				return;

			} else if (event->v1 == 4) /* SUSTAIN */
			{
				int i;
				for (i=0; i < channelstat[event->v2].notenum; i++)
				{
					if (channelstat[event->v2].note[i] == event->v3)
					{
						channelstat[event->v2].opt[i] &= ~1; /* no idea */
						return; /* already playing? */
					}
				}
			} else if ((event->v1 == 1) || (event->v1 == 8) || (event->v1 == 16)) /* NOTE-OFF */
			{
				int i;
				for (i=0; i < channelstat[event->v2].notenum; i++)
				{
					if (channelstat[event->v2].note[i] == event->v3)
					{ /* REMOVE */
						PRINT ("REMOVE ch=%ld note=%ld\n", event->v2, event->v3);
						memmove (channelstat[event->v2].note + i, channelstat[event->v2].note + i + 1, channelstat[event->v2].notenum - i - 1);
						memmove (channelstat[event->v2].vol + i, channelstat[event->v2].vol + i + 1, channelstat[event->v2].notenum - i - 1);
						memmove (channelstat[event->v2].opt + i, channelstat[event->v2].opt + i + 1, channelstat[event->v2].notenum - i - 1);
						channelstat[event->v2].notenum--;
						return;
					}
				}
				PRINT ("NOT FOUND REMOVE ch=%ld note=%ld\n", event->v2, event->v3);
			}
			return;

		case CTLE_PROGRAM:
			if ((event->v1 < 0) || (event->v1 >= 16))
			{
				return; /* Outside the normal MIDI range */
			}
			snprintf (channelstat[event->v1].instrument, sizeof (channelstat[event->v1].instrument), "%s", (char *)event->v3);
			channelstat[event->v1].program = event->v2;
			channelstat[event->v1].bank_msb = event->v4>>8;
			channelstat[event->v1].bank_lsb = event->v4&0xff;
			return;

		case CTLE_VOLUME:
			if ((event->v1 < 0) || (event->v1 >= 16))
			{
				return; /* Outside the normal MIDI range */
			}
			channelstat[event->v1].gvol = event->v2;
			return;

		case CTLE_PANNING:
			if ((event->v1 < 0) || (event->v1 >= 16))
			{
				return; /* Outside the normal MIDI range */
			}
			channelstat[event->v1].pan = event->v2 & 0x7f;
			return;

		case CTLE_PITCH_BEND:
			if ((event->v1 < 0) || (event->v1 >= 16))
			{
				return; /* Outside the normal MIDI range */
			}
			channelstat[event->v1].pitch = event->v2;
			return;

		case CTLE_CHORUS_EFFECT:
			if ((event->v1 < 0) || (event->v1 >= 16))
			{
				return; /* Outside the normal MIDI range */
			}
			channelstat[event->v1].chorus = event->v2;
			return;

		case CTLE_REVERB_EFFECT:
			if ((event->v1 < 0) || (event->v1 >= 16))
			{
				return; /* Outside the normal MIDI range */
			}
			channelstat[event->v1].reverb = event->v2;
			return;

		case CTLE_SUSTAIN:
			if ((event->v1 < 0) || (event->v1 >= 16))
			{
				return; /* Outside the normal MIDI range */
			}
			channelstat[event->v1].pedal = event->v2;
			return;
	}
}

static void timidity_append_EventDelayed_PlrBuf (CtlEventDelayed *self)
{
	self->delay_samples = bufpos; /* absolute position in the buffer */

	if (EventDelayed_PlrBuf_head)
	{
		EventDelayed_PlrBuf_tail->next = self;
		EventDelayed_PlrBuf_tail = self;
	} else {
		EventDelayed_PlrBuf_head = self;
		EventDelayed_PlrBuf_tail = self;
	}
}

static void timidity_SetBufferPos_EventDelayed_PlrBuf (int old_pos, int new_pos)
{
	CtlEventDelayed *iter;

	for (iter = EventDelayed_PlrBuf_head; iter;)
	{
		CtlEventDelayed *next = iter->next;

		if ( ((old_pos < new_pos) && ((iter->delay_samples >= old_pos) && (iter->delay_samples <= new_pos))) || /* normal progression */
		     ((old_pos > new_pos) && ((iter->delay_samples >= old_pos) || (iter->delay_samples <= new_pos))) ) /* position wrapped */
		{
			assert (EventDelayed_PlrBuf_head == iter);

			EventDelayed_PlrBuf_head = next;

			if (!iter->next)
			{
				EventDelayed_PlrBuf_tail = 0; /* we are always deleting the head entry if any.. so list is empty if no next */
			} else {
				iter->next = 0;
			}
			iter->delay_samples = 0;
			timidity_apply_EventDelayed (&iter->event);
			free_EventDelayed (iter);

			iter = next;
		} else {
			break;
		}
	}
}

static void timidity_append_EventDelayed_gmibuf (CtlEvent *event)
{
	CtlEventDelayed *self = calloc (sizeof (*self), 1);
	if (!self)
	{
		perror ("timidity_append_EventDelayed_gmibuf malloc");
		_exit(1);
	}

	self->event = *event;
	self->delay_samples = gmibuffill;

	if (self->event.type == CTLE_PROGRAM)
	{
		self->event.v3 = (long)strdup(self->event.v3?(char *)self->event.v3:"");
	}

	if (EventDelayed_gmibuf_head)
	{
		assert (EventDelayed_gmibuf_tail->delay_samples <= self->delay_samples);
		EventDelayed_gmibuf_tail->next = self;
		EventDelayed_gmibuf_tail = self;
	} else {
		EventDelayed_gmibuf_head = self;
		EventDelayed_gmibuf_tail = self;
	}
}

static void timidity_play_EventDelayed_gmibuf (uint32_t samples)
{
	CtlEventDelayed *iter, *next;

	for (iter = EventDelayed_gmibuf_head; iter; iter = next)
	{
		next = iter->next;

		if (iter->delay_samples <= samples)
		{
			assert (iter == EventDelayed_gmibuf_head);

			EventDelayed_gmibuf_head = next;

			if (!iter->next)
			{
				EventDelayed_gmibuf_tail = 0; /* we are always deleting the head entry if any.. so list is empty if no next */
			} else {
				iter->next = 0;
			}
			iter->delay_samples = 0;
			timidity_append_EventDelayed_PlrBuf (iter);
		} else {
			iter->delay_samples -= samples;
		}
	}
}

static int read_user_config_file(void)
{
    char *home;
    char path[BUFSIZ];
    int status;

    home = getenv("HOME");
#ifdef __W32__
/* HOME or home */
    if(home == NULL)
	home = getenv("HOMEPATH");
    if(home == NULL)
	home = getenv("home");
#endif
    if(home == NULL)
    {
	ctl->cmsg(CMSG_INFO, VERB_NOISY,
		  "Warning: HOME environment is not defined.");
	return 0;
    }

#ifdef __W32__
/* timidity.cfg or _timidity.cfg or .timidity.cfg*/
    snprintf(path, sizeof (path), "%s" PATH_STRING "timidity.cfg", home);
    status = read_config_file(path, 0, 1);
    if (status != READ_CONFIG_FILE_NOT_FOUND)
        return status;

    snprintf(path, sizeof (path), "%s" PATH_STRING "_timidity.cfg", home);
    status = read_config_file(path, 0, 1);
    if (status != READ_CONFIG_FILE_NOT_FOUND)
        return status;
#endif

    snprintf(path, sizeof (path), "%s" PATH_STRING ".timidity.cfg", home);
    status = read_config_file(path, 0, 1);
    if (status != 3 /*READ_CONFIG_FILE_NOT_FOUND*/)
        return status;

    return 0;
}

/* Killing Module 2 MIDI support with dummy calls */

int __attribute__ ((visibility ("internal"))) get_module_type (char *fn)
{
	return IS_OTHER_FILE; /* Open Cubic Player will provide module support */
}

int __attribute__ ((visibility ("internal"))) convert_mod_to_midi_file(MidiEvent * ev)
{
	ctl->cmsg(CMSG_INFO, VERB_NORMAL,
	          "Aborting!  timidity attempted to convert module to midi file\n");
	return 1;
}

char __attribute__ ((visibility ("internal"))) *get_module_title (struct timidity_file *tf, int mod_type)
{
	return NULL;
}

int __attribute__ ((visibility ("internal"))) load_module_file (struct timidity_file *tf, int mod_type)
{
	return 1; /* Fail to load */
}

void __attribute__ ((visibility ("internal"))) ML_RegisterAllLoaders ()
{
}

/* Our virtual Control Mode */


static int ocp_ctl_open (int using_stdin, int using_stdout)
{
	return 0;
}

static void ocp_ctl_close (void)
{
}

static int ocp_ctl_pass_playing_list (int number_of_files, char *list_of_files[])
{
	return 0;
}

void __attribute__ ((visibility ("internal"))) timidityRestart (void)
{
	ctl_next_value = 0;
	ctl_next_result = RC_RESTART;
}

void __attribute__ ((visibility ("internal"))) timiditySetRelPos(int pos)
{
	if (pos > 0)
	{ /* async, so set the value, before result */
		ctl_next_value = plrRate * pos;
		ctl_next_result = RC_FORWARD;
	} else {
		ctl_next_value = plrRate * -pos;
		ctl_next_result = RC_BACK;
	}
}

static int ocp_ctl_read (int32 *valp)
{
	int retval = ctl_next_result;
	*valp = ctl_next_value;

	if (retval != RC_NONE)
	{
		ctl_next_result = RC_NONE;
		ctl_next_value = 0;

		PRINT ("[timidity] ctl->read=%d (val=%d)\n", retval, *valp);
	}

	return retval;
}

static int ocp_ctl_write (char *buf, int32 size)
{
	/* stdout redirects ? */
	fprintf (stderr, "[timidity] %.*s", (int)size, buf);
	return size;
}


static int ocp_ctl_cmsg(int type, int verbosity_level, char *fmt, ...)
{
	va_list ap;

#ifndef TIMIDITY_DEBUG
	if (verbosity_level == VERB_DEBUG_SILLY)
	{
		if (!loading)
		{
			return 0;
		}
	} else if (!((type == CMSG_WARNING) || (type == CMSG_ERROR) || (type == CMSG_FATAL)))
	{
		if (!loading)
		{
			return 0;
		}
	}
#endif

	fputs("[timidity] ", stderr);

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	fputs ("\n", stderr);

	return 0;
}

static void ocp_ctl_event(CtlEvent *event)
{
	switch (event->type)
	{
		case CTLE_NOW_LOADING:
			PRINT ("[timidity] ctl->event (event=CTLE_NOW_LOADING, v1=\"%s\")\n", (char *)event->v1);
			break;
		case CTLE_LOADING_DONE:
			PRINT ("[timidity] ctl->event (event=CTLE_LOADING_DONE, v1=%ld %s)\n", event->v1, (event->v1<0)?"error":(event->v1>0)?"terminated":"success");
			break;
		case CTLE_PLAY_START:
			PRINT ("[timidity] ctl->event (event=CTLE_PLAY_START, v1=%ld samples)\n", event->v1);
			break;
		case CTLE_PLAY_END:
			PRINT ("[timidity] ctl->event (event=CTLE_PLAY_END)\n");
			break;
		case CTLE_CURRENT_TIME:
			PRINT ("[timidity] ctl->event (event=CTLE_CURRENT_TIME, v1=%ld seconds, v2=%ld voices)\n", event->v1, event->v2);
			break;
		case CTLE_NOTE:
			PRINT ("[timidity] ctl->event (event=CTLE_NOTE, v1=%ld status, v2=%ld ch, v3=%ld note, v4=%ld velocity)\n", event->v1, event->v2, event->v3, event->v4);
			break;
		case CTLE_MASTER_VOLUME:
			PRINT ("[timidity] ctl->event (event=CTLE_MASTER_VOLUME, v1=%ld amp %%)\n", event->v1);
			break;
		case CTLE_METRONOME:
			PRINT ("[timidity] ctl->event (event=CTLE_METRONOME, v1=%ld measure, v2=%ld beat)\n", event->v1, event->v2);
			break;
		case CTLE_KEYSIG:
			PRINT ("[timidity] ctl->event (event=CTLE_KEYSIG, v1=%ld key sig)\n", event->v1);
			break;
		case CTLE_KEY_OFFSET:
			PRINT ("[timidity] ctl->event (event=CTLE_KEY_OFFSET, v1=%ld key offset)\n", event->v1);
			break;
		case CTLE_TEMPO:
			PRINT ("[timidity] ctl->event (event=CTLE_TEMPO, v1=%ld tempo)\n", event->v1);
			break;
		case CTLE_TIME_RATIO:
			PRINT ("[timidity] ctl->event (event=CTLE_TIME_RATIO, v1=%ld time ratio %%)\n", event->v1);
			break;
		case CTLE_TEMPER_KEYSIG:
			PRINT ("[timidity] ctl->event (event=CTLE_TEMPER_KEYSIG, v1=%ld tuning key sig)\n", event->v1);
			break;
		case CTLE_TEMPER_TYPE:
			PRINT ("[timidity] ctl->event (event=CTLE_TEMPER_TYPE, v1=%ld ch, v2=%ld tuning type)\n", event->v1, event->v2);
			break;
		case CTLE_MUTE:
			PRINT ("[timidity] ctl->event (event=CTLE_MUTE, v1=%ld ch, v2=%ld is_mute)\n", event->v1, event->v2);
			break;
		case CTLE_PROGRAM:
			PRINT ("[timidity] ctl->event (event=CTLE_PROGRAM, v1=%ld ch, v2=%ld prog, v3=name \"%s\", v4=%ld bank %dmsb.%dlsb)\n", event->v1, event->v2, (char *)event->v3, event->v4, (int)(event->v4>>8), (int)(event->v4&0xff));
			break;
		case CTLE_VOLUME:
			PRINT ("[timidity] ctl->event (event=CTLE_VOLUME, v1=%ld ch, v2=%ld value)\n", event->v1, event->v2);
			break;
		case CTLE_EXPRESSION:
			PRINT ("[timidity] ctl->event (event=CTLE_EXPRESSION, v1=%ld ch, v2=%ld value)\n", event->v1, event->v2);
			break;
		case CTLE_PANNING:
			PRINT ("[timidity] ctl->event (event=CTLE_PANNING, v1=%ld ch, v2=%ld value)\n", event->v1, event->v2);
			break;
		case CTLE_SUSTAIN:
			PRINT ("[timidity] ctl->event (event=CTLE_SUSTAIN, v1=%ld ch, v2=%ld value)\n", event->v1, event->v2);
			break;
		case CTLE_PITCH_BEND:
			PRINT ("[timidity] ctl->event (event=CTLE_PITCH_BEND, v1=%ld ch, v2=%ld value)\n", event->v1, event->v2);
			break;
		case CTLE_MOD_WHEEL:
			PRINT ("[timidity] ctl->event (event=CTLE_MOD_WHEEL, v1=%ld ch, v2=%ld value)\n", event->v1, event->v2);
			break;
		case CTLE_CHORUS_EFFECT:
			PRINT ("[timidity] ctl->event (event=CTLE_CHORUS_EFFECT, v1=%ld ch, v2=%ld value)\n", event->v1, event->v2);
			break;
		case CTLE_REVERB_EFFECT:
			PRINT ("[timidity] ctl->event (event=CTLE_REVERB_EFFECT, v1=%ld ch, v2=%ld value)\n", event->v1, event->v2);
			break;
		case CTLE_LYRIC:
			PRINT ("[timidity] ctl->event (event=CTLE_LYRIC, v1=%ld lyric-ID)\n", event->v1);
			break;
		case CTLE_REFRESH:
			PRINT ("[timidity] ctl->event (event=CTLE_REFRESH)\n");
			break;
		case CTLE_RESET:
			PRINT ("[timidity] ctl->event (event=CTLE_RESET)\n");
			break;
		case CTLE_SPEANA:
			{
#ifdef TIMIDITY_DEBUG
				double *v1 = (double *)event->v1;
				int i;
				PRINT ("[timidity] ctl->event (event=CTLE_SPEANA, v1=[)\n");
				for (i=0; i < event->v2; i++)
				{
					PRINT ("%s%lf", i?",":"", v1[i]);
				}
				PRINT ("], v2=%ld len)\n", event->v2);
#endif
			}
			break;
		case CTLE_PAUSE:
			PRINT ("[timidity] ctl->event (event=CTLE_PAUSE, v1=%ld on/off, v2=%ld time of pause)\n", event->v1, event->v2);
			break;
		case CTLE_GSLCD:
			PRINT ("[timidity] ctl->event (event=CTLE_GSLCD, v1=%ld ?)\n", event->v1);
			break;
		case CTLE_MAXVOICES:
			PRINT ("[timidity] ctl->event (event=CTLE_MAXVOICES, v1=%ld voices)\n", event->v1);
			break;
		case CTLE_DRUMPART:
			PRINT ("[timidity] ctl->event (event=CTLE_DRUMPART, v1=%ld ch, v2=%ld is_drum)\n", event->v1, event->v2);
			break;
		default:
			PRINT ("[timidity] ctl->event (event=%d unknown)\n", event->type);
			break;
	}

	switch (event->type)
	{
		case CTLE_NOTE:
			if ( (event->v1 ==  1) || /* NOTE FREE */
			     (event->v1 ==  2) || /* NOTE ON */
			     (event->v1 ==  4) || /* NOTE SUSTAIN */
			     (event->v1 ==  8) || /* NOTE OFF */
			     (event->v1 == 16))   /* NOTE DIE */
			{
				timidity_append_EventDelayed_gmibuf (event);
			}
			break;
		//case CTLE_MUTE:
		case CTLE_PROGRAM:
		case CTLE_PANNING:
		case CTLE_PITCH_BEND:
		//case CTLE_DRUMPART:
		case CTLE_VOLUME:
		case CTLE_CHORUS_EFFECT:
		case CTLE_REVERB_EFFECT:
		case CTLE_SUSTAIN:
			timidity_append_EventDelayed_gmibuf (event);
			break;

	}
}

static ControlMode ocp_ctl =
{
	.id_name           = "Open Cubic Player Control API",
	.id_character      = '_',
	.id_short_name     = "OCP",
	.verbosity         = 0,
	.trace_playing     = 0, /* ? */
	.opened            = 1,
	.flags             = 0,
	.open              = ocp_ctl_open,
	.close             = ocp_ctl_close,
	.pass_playing_list = ocp_ctl_pass_playing_list,
	.read              = ocp_ctl_read,
	.write             = ocp_ctl_write,
	.cmsg              = ocp_ctl_cmsg,
	.event             = ocp_ctl_event
};

/* Our virtual PlayMode */

static int ocp_playmode_open_output(void)
{
	/* 0=success, 1=warning, -1=fatal error */
	PRINT ("[timidity] playmode->open_output()\n");
	output_counter = 0;
	return 0;
}

static void ocp_playmode_close_output(void)
{
	PRINT ("[timidity] playmode->close_output\n");
}


static int ocp_playmode_output_data(char *buf, int32 bytes)
{
	PRINT ("[timidity] playmode->output_data (bytes=%d gmibufhead=%d gmibuftail=%d gmibuflen=%d)\n", bytes, gmibufhead, gmibuftail, gmibuflen);

	gmibuffill += (bytes>>2);
	gmibuffree -= (bytes>>2);

	if (gmibufhead+(bytes>>2) >= gmibuflen)
	{
		assert (gmibufhead >= gmibuftail); /* we are not going to overrun the buffer */

		memcpy (gmibuf+(gmibufhead<<2), buf, (gmibuflen-gmibufhead)<<2);
		buf   += (gmibuflen-gmibufhead)<<2;
		bytes -= (gmibuflen-gmibufhead)<<2;
		gmibufhead = 0;
	}

	if (bytes)
	{
		assert ((gmibuftail <= gmibufhead) || ((gmibufhead + (bytes>>2)) <= gmibuftail)); /* we are not going to overrun the buffer */

		memcpy (gmibuf+(gmibufhead<<2), buf, bytes);
		gmibufhead += (bytes>>2);
	}

	output_counter += (bytes>>2);

	return 0;
}

static int ocp_playmode_acntl(int request, void *arg)
{
	switch(request)
	{
		/* case PM_REQ_MIDI - we have not enabled this flag */
		/* case PM_REQ_INST_NAME - ??? */

		case PM_REQ_DISCARD: /* what-ever... we are the actual master of this */
			PRINT ("[timidity] playmode->acntl(request=DISCARD)\n");
#warning TODO implement
			output_counter = 0;
			return 0;

		case PM_REQ_FLUSH: /* what-ever... we are the actual master of this */
			PRINT ("[timidity] playmode->acntl(request=FLUSH)\n");
			output_counter = 0;
			return 0;

		case PM_REQ_GETQSIZ:
			PRINT ("[timidity] playmode->acntl(request=GETQSIZ) => %d\n", buflen>>1);
			*((int *)arg) = buflen>>1 /* >>1 is due to STEREO */;
			return 0;

		/* case PM_REQ_SETQSIZ */
		/* case PM_REQ_GETFRAGSIZ */

		case PM_REQ_RATE:
			/* sample rate in and out */
			PRINT ("[timidity] playmode->acntl(request=RATE, rate=%d)\n", *(int *)arg);
			/* but we ignore it... */
			return 0;

		case PM_REQ_GETSAMPLES:
			PRINT ("[timidity] playmode->acntl(request=GETSAMPLES) => 0 (fixed)\n");
			*((int *)arg) = output_counter; /* samples */
			return 0;

		case PM_REQ_PLAY_START: /* Called just before playing */
			PRINT ("[timidity] playmode->acntl(request=PLAY_START)\n");
			break;

		case PM_REQ_PLAY_END: /* Called just after playing */
			PRINT ("[timidity] playmode->acntl(request=PLAY_END)\n");
			return 0;

		case PM_REQ_GETFILLABLE:
			{
				ssize_t clean = gmibuffree;
				/* Timidity always tries to overfill the buffer, since it normally waits for completion... */
				if (clean < 0)
				{
					clean = 0;
				}
				PRINT ("[timidity] playmode->acntl(request=GETFILLABLE) => %d\n", (int)clean);
				*((int *)arg) = clean; /* samples */;
				return 0;
			}

		case PM_REQ_GETFILLED:
			*((int *)arg) = gmibuffill;
			PRINT ("[timidity] playmode->acntl(request=GETFILLED) => %d\n", *((int *)arg));
			return 0;

		/* case PM_REQ_OUTPUT_FINISH: called just after the last output_data(); */

		case PM_REQ_DIVISIONS:
			/* sample rate in and out */
			PRINT ("[timidity] playmode->acntl(request=DIVISIONS, bpm=%d\n", *(int *)arg);
			/* but we ignore it... */
			return 0;
	}
	return -1;
}


static int ocp_playmode_detect(void)
{
	/* 0=not available, 1=available */
	return 1;
}

static PlayMode ocp_playmode =
{
	.encoding = /*PE_MONO*/ PE_SIGNED | PE_16BIT, /* TODO PE_24BIT */
	.flag = PF_PCM_STREAM | PF_CAN_TRACE,
	.rate = 44100,
	.fd = -1,
	.extra_param = {0},
	.id_name = "Open Cubic Player Output API",
	.id_character = '_',
	.name = "NULL",
	.open_output = ocp_playmode_open_output,
	.close_output = ocp_playmode_close_output,
	.output_data = ocp_playmode_output_data,
	.acntl = ocp_playmode_acntl,
	.detect= ocp_playmode_detect
};

PlayMode *play_mode_list[2] = {&ocp_playmode, 0}, *target_play_mode = &ocp_playmode, *play_mode = &ocp_playmode;

ControlMode *ctl_list[2] = {&ocp_ctl, 0}, *ctl = &ocp_ctl;

static void emulate_main_start()
{
	const char *configfile;
	free_instruments_afterwards = 1;

#ifdef SUPPORT_SOCKET
	init_mail_addr();
	if(url_user_agent == NULL)
	{
		url_user_agent = (char *)safe_malloc(10 + strlen(timidity_version));
		strcpy(url_user_agent, "TiMidity-");
		strcat(url_user_agent, timidity_version);
	}
#endif /* SUPPORT_SOCKET */

	timidity_start_initialize();

	configfile = cfGetProfileString ("timidity", "configfile", "");

	if (configfile[0])
	{
		int len = strlen (configfile);
		if (len > 5 && (!strcasecmp (&configfile[len - 4], ".sf2")))
		{
			/* add_soundfont lacks constification of filename */
			add_soundfont ((char *)configfile, -1, -1, -1, -1);
			goto ready;
		} else {
			/* read_config_file lacks constification of filename */
			if (read_config_file((char *)configfile, 0, 0))
			{
				fprintf (stderr, "Failed to load \"%s\", defaulting to global loading\n", configfile);
			} else {
				got_a_configuration = 1;
				goto ready;
			}
		}
	}
	/* (timidity_pre_load_configuration ())
	{
		fprintf (stderr, "[timidity] pre-load config failed\n");
	}*/
	if (!got_a_configuration)
	{
		/* test CONFIG_FILE first if it is defined to be something else than one of our standard paths */
		if (strcmp(CONFIG_FILE, "/etc/timidity/timidity.cfg") &&
		    strcmp(CONFIG_FILE, "/etc/timidity.cfg") &&
		    strcmp(CONFIG_FILE, "/usr/local/share/timidity/timidity.cfg") &&
		    strcmp(CONFIG_FILE, "/usr/share/timidity/timidity.cfg") && !read_config_file(CONFIG_FILE, 0, 0))
		{
			got_a_configuration = 1;
		} else if (!read_config_file("/etc/timidity/timidity.cfg", 0, 0))
		{
			got_a_configuration = 1;
		} else if (!read_config_file("/etc/timidity.cfg", 0, 0))
		{
			got_a_configuration = 1;
		} else if (!read_config_file("/usr/local/share/timidity/timidity.cfg", 0, 0))
		{
			got_a_configuration = 1;
		} else if (!read_config_file("/usr/share/timidity/timidity.cfg", 0, 0))
		{
		got_a_configuration = 1;
		} else {
			ctl->cmsg(CMSG_WARNING, VERB_NORMAL, "Warning: Unable to find global timidity.cfg file");
		}

		if (read_user_config_file())
		{
			ctl->cmsg(CMSG_ERROR, VERB_NORMAL, "Error: Syntax error in ~/.timidity.cfg");
		}
	}

ready:
	/* we need any emulated configuration, perform them now... */
	if (timidity_post_load_configuration ())
	{
		fprintf (stderr, "[timidity] post-load config failed\n");
	}

	timidity_init_player ();
}

static void emulate_main_end()
{
	int i;

#ifdef SUPPORT_SOCKET
	if (url_user_agent)
		free(url_user_agent);
	if (url_http_proxy_host)
		free(url_http_proxy_host);
	if (url_ftp_proxy_host)
		free(url_ftp_proxy_host);
	if (user_mailaddr)
		free(user_mailaddr);
#endif

	if (opt_aq_max_buff)
		free(opt_aq_max_buff);
	opt_aq_max_buff = NULL;

	if (opt_aq_fill_buff && opt_aq_fill_buff_free_needed)
		free(opt_aq_fill_buff);
	opt_aq_fill_buff_free_needed = 1;
	opt_aq_fill_buff = NULL;

	if (output_text_code)
		free(output_text_code);
	output_text_code = NULL;

	free_soft_queue();
	free_instruments(0);
	playmidi_stream_free();
	free_soundfonts();
	free_cache_data();
	free_wrd();
	free_readmidi();
	free_global_mblock();
	tmdy_free_config();
	free_reverb_buffer();
	free_effect_buffers();
	free(voice); voice=0;
	free_gauss_table();
	for (i = 0; i < MAX_CHANNELS; i++)
		free_drum_effect(i);
}

int emulate_timidity_play_main_start ()
{
	if(wrdt->open(NULL))
	{
		fprintf(stderr, "Couldn't open WRD Tracer: %s (`%c')\n", wrdt->name, wrdt->id);
		return 1;
	}

	/* Open output device */
#if 0
	ctl->cmsg(CMSG_INFO, VERB_DEBUG_SILLY,
	          "Open output: %c, %s",
	           play_mode->id_character,
	           play_mode->id_name);
#endif

	if (play_mode->flag & PF_PCM_STREAM)
	{
		play_mode->extra_param[1] = aq_calc_fragsize();
		ctl->cmsg(CMSG_INFO, VERB_DEBUG_SILLY,
		          "requesting fragment size: %d",
		          play_mode->extra_param[1]);
	}
	if(play_mode->open_output() < 0)
	{
		ctl->cmsg(CMSG_FATAL, VERB_NORMAL,
		          "Couldn't open %s (`%c')",
		          play_mode->id_name, play_mode->id_character);
		ctl->close();
		return 2;
	}

	if(!control_ratio)
	{
	    control_ratio = play_mode->rate / CONTROLS_PER_SECOND;
	    if(control_ratio < 1)
		control_ratio = 1;
	    else if (control_ratio > MAX_CONTROL_RATIO)
		control_ratio = MAX_CONTROL_RATIO;
	}

	init_load_soundfont();
	aq_setup();
	timidity_init_aq_buff();

	if(allocate_cache_size > 0)
		resamp_cache_reset();

	return 0;
}

static void emulate_timidity_play_main_end (void)
{
	// if (intr) aq_flush(1);

	play_mode->close_output();
	ctl->close();
	wrdt->close();

#ifdef SUPPORT_SOUNDSPEC
	if(view_soundspec_flag)
		close_soundspec();
#endif /* SUPPORT_SOUNDSPEC */

	free_archive_files();
#ifdef SUPPORT_SOCKET
	url_news_connection_cache(URL_NEWS_CLOSE_CACHE);
#endif /* SUPPORT_SOCKET */
}

struct emulate_play_midi_file_session
{
	MidiEvent *event;
	int32 nsamples;
	int first;
};

static struct emulate_play_midi_file_session timidity_main_session;

static int emulate_play_midi_file_start(const char *fn, uint8_t *data, size_t datalen, struct emulate_play_midi_file_session *s)
{
	int i, j, rc;


	s->first = 1;
	s->event = NULL;

	/* Set current file information */
	current_file_info = get_midi_file_info((char *)fn, 1);
	current_file_info->midi_data = (char *)data;
	current_file_info->midi_data_size = datalen;

	rc = check_apply_control();
	if(RC_IS_SKIP_FILE(rc) && rc != RC_RELOAD)
		return rc;

	/* Reset key & speed each files */
	current_keysig = (opt_init_keysig == 8) ? 0 : opt_init_keysig;
	note_key_offset = key_adjust;
	midi_time_ratio = tempo_adjust;
	for (i = 0; i < MAX_CHANNELS; i++)
	{
		for (j = 0; j < 12; j++)
			channel[i].scale_tuning[j] = 0;
		channel[i].prev_scale_tuning = 0;
		channel[i].temper_type = 0;
	}
	CLEAR_CHANNELMASK(channel_mute);
	if (temper_type_mute & 1)
		FILL_CHANNELMASK(channel_mute);

	/* Reset restart offset */
	midi_restart_time = 0;

#ifdef REDUCE_VOICE_TIME_TUNING
	/* Reset voice reduction stuff */
	min_bad_nv = 256;
	max_good_nv = 1;
	ok_nv_total = 32;
	ok_nv_counts = 1;
	ok_nv = 32;
	ok_nv_sample = 0;
	old_rate = -1;
	reduce_quality_flag = no_4point_interpolation;
	restore_voices(0);
#endif

	ctl_mode_event(CTLE_METRONOME, 0, 0, 0);
	ctl_mode_event(CTLE_KEYSIG, 0, current_keysig, 0);
	ctl_mode_event(CTLE_TEMPER_KEYSIG, 0, 0, 0);
	ctl_mode_event(CTLE_KEY_OFFSET, 0, note_key_offset, 0);
	i = current_keysig + ((current_keysig < 8) ? 7 : -9), j = 0;
	while (i != 7)
		i += (i < 7) ? 5 : -7, j++;
	j += note_key_offset, j -= floor(j / 12.0) * 12;
	current_freq_table = j;
	ctl_mode_event(CTLE_TEMPO, 0, current_play_tempo, 0);
	ctl_mode_event(CTLE_TIME_RATIO, 0, 100 / midi_time_ratio + 0.5, 0);
	for (i = 0; i < MAX_CHANNELS; i++)
	{
		ctl_mode_event(CTLE_TEMPER_TYPE, 0, i, channel[i].temper_type);
		ctl_mode_event(CTLE_MUTE, 0, i, temper_type_mute & 1);
	}

	return rc;
}

static int emulate_play_midi_start(MidiEvent *eventlist, int32 samples)
{
	int rc = RC_NONE;

	sample_count = samples;
	event_list = eventlist;
	lost_notes = cut_notes = 0;
	check_eot_flag = 1;

	wrd_midi_event(-1, -1); /* For initialize */

	reset_midi(0);
	if(!opt_realtime_playing && allocate_cache_size > 0 && !IS_CURRENT_MOD_FILE && (play_mode->flag&PF_PCM_STREAM))
	{
		play_midi_prescan(eventlist);
		reset_midi(0);
	}

#if 0
	rc = aq_flush(0);
	if(RC_IS_SKIP_FILE(rc))
		return rc;
#else
	init_effect ();
#endif

	skip_to(midi_restart_time);

	if(midi_restart_time > 0)
	{
		/* Need to update interface display */
		int i;
		for(i = 0; i < MAX_CHANNELS; i++)
		{
			redraw_controllers(i);
		}
	}

	return rc;
}

static int dump_rc(int rc)
{
	switch (rc)
	{
		case RC_ASYNC_HACK:        PRINT ("RC_ASYNC_HACK\n"); break;

		case RC_ERROR:             PRINT ("RC_ERROR\n"); break;
		case RC_NONE:              PRINT ("RC_NONE\n"); break;
		case RC_QUIT:              PRINT ("RC_QUIT\n"); break;
		case RC_NEXT:              PRINT ("RX_NEXT\n"); break;
		case RC_PREVIOUS:          PRINT ("RC_PREVIOUS\n"); break;
		case RC_FORWARD:           PRINT ("RC_FORWARD\n"); break;
		case RC_BACK:              PRINT ("RC_BACK\n"); break;
		case RC_JUMP:              PRINT ("RC_JUMP\n"); break;
		case RC_TOGGLE_PAUSE:      PRINT ("RC_TOGGLE_PAUSE\n"); break;
		case RC_RESTART:           PRINT ("RC_RESTART\n"); break;
		case RC_PAUSE:             PRINT ("RC_PAUSE\n"); break;
		case RC_CONTINUE:          PRINT ("RC_CONTINUE\n"); break;
		case RC_REALLY_PREVIOUS:   PRINT ("RC_REALLY_PREVIOUS\n"); break;
		case RC_CHANGE_VOLUME:     PRINT ("RC_CHANGE_VOLUME\n"); break;
		case RC_LOAD_FILE:         PRINT ("RC_LOAD_FILE\n"); break;
		case RC_TUNE_END:          PRINT ("RC_TUNE_END\n"); break;
		case RC_KEYUP:             PRINT ("RC_KEYUP\n"); break;
		case RC_KEYDOWN:           PRINT ("RC_KEYDOWN\n"); break;
		case RC_SPEEDUP:           PRINT ("RC_SPEEDUP\n"); break;
		case RC_SPEEDDOWN:         PRINT ("RC_SPEEDDOWN\n"); break;
		case RC_VOICEINCR:         PRINT ("RC_VOICEINCR\n"); break;
		case RC_VOICEDECR:         PRINT ("RC_VOICEDECR\n"); break;
		case RC_TOGGLE_DRUMCHAN:   PRINT ("RC_TOGGLE_DRUMCHAN\n"); break;
		case RC_RELOAD:            PRINT ("RC_RELOAD\n"); break;
		case RC_TOGGLE_SNDSPEC:    PRINT ("RC_TOGGLE_SNDSPEC\n"); break;
		case RC_CHANGE_REV_EFFB:   PRINT ("RC_CHANGE_REV_EFFB\n"); break;
		case RC_CHANGE_REV_TIME:   PRINT ("RC_CHANGE_REV_TIME\n"); break;
		case RC_SYNC_RESTART:      PRINT ("RC_SYNC_RESTART\n"); break;
		case RC_TOGGLE_CTL_SPEANA: PRINT ("RC_TOGGLE_CTL_SPEANA\n"); break;
		case RC_CHANGE_RATE:       PRINT ("RC_CHANGE_RATE\n"); break;
		case RC_OUTPUT_CHANGED:    PRINT ("RC_OUTPUT_CHANGED\n"); break;
		case RC_STOP:              PRINT ("RC_STOP\n"); break;
		case RC_TOGGLE_MUTE:       PRINT ("RC_TOGGLE_MUTE\n"); break;
		case RC_SOLO_PLAY:         PRINT ("RC_SOLO_PLAY\n"); break;
		case RC_MUTE_CLEAR:        PRINT ("RC_MUTE_CLEAR\n"); break;
		default:                   PRINT ("RC_UKN_%d\n", rc); break;

	}

	return rc;
}

static int emulate_play_event (MidiEvent *ev)
{
	int32 cet;

	/* Open Cubic Player always stream, so we skip some checks */

	cet = MIDI_EVENT_TIME(ev);

	if(cet > current_sample)
	{
		int32 needed = cet - current_sample;
		int32 canfit = aq_fillable();

		PRINT ("emulate_play_event, needed=%d canfit=%d clean=%d (%d) -- ", needed, canfit, (int)gmibuffree, gmibuflen);

//		if (canfit <= (gmibuflen >>2)) /* we only use the first quarter of the buffer... the last is reserved for flush at that happens at the end of the track */
		if ((canfit <= 0) || (gmibuffree <= (audio_buffer_size*2)))
		{
			PRINT ("short exit.. we want more buffer to be free\n");
			return RC_ASYNC_HACK;
		}

//		if (canfit > (gmibuflen >> 2))
//			canfit = gmibuflen >> 2;

		if (needed > canfit)
		{
			int rc;

			PRINT ("needed > canfit\n");

			rc = compute_data(canfit);

			ctl_mode_event(CTLE_REFRESH, 0, 0, 0);
			if (rc == RC_NONE)
				rc = RC_ASYNC_HACK;
			return rc;
		}
	}

	PRINT ("Allow\n");

	PRINT ("emulate_play_event calling play_event\n");

	return dump_rc(play_event (ev));
}

static int emulate_play_midi_iterate(void)
{
	int rc;

	for(;;)
	{
		if (gmibuffree > (audio_buffer_size*2))
		{
			int32_t cet = MIDI_EVENT_TIME (current_event);

			midi_restart_time = 1;

			/* detect if the next event is more that buffer-size away */
			if ((cet > current_sample) && (cet - current_sample) > audio_buffer_size)
			{ /* inject a dummy event to just push audio samples... */
				MidiEvent *pushed_event = current_event;
				MidiEvent dummy;
				dummy.time = (float)(current_sample + (audio_buffer_size/2))/midi_time_ratio;
				dummy.type = ME_NONE;
				dummy.channel = 0;
				dummy.a = 0;
				dummy.b = 0;
				PRINT ("emulate_play_midi_iterate calling FAKED emulate_play_event (estimate samples  %d)\n", MIDI_EVENT_TIME (&dummy) - current_sample);
				rc = emulate_play_event (&dummy);
				dump_rc (rc);
				current_event = pushed_event;
				if (rc == RC_NONE)
				{
					rc = RC_ASYNC_HACK;
				}
				break;
			} else {
				PRINT ("emulate_play_midi_iterate calling emulate_play_event\n");
				rc = emulate_play_event(current_event);
				dump_rc (rc);
				if(rc != RC_NONE)
					break;
				if (midi_restart_time)  /* don't skip the first event if == 0 */
				{
					if (speed != 0x10000)
					{
						int32_t diff = (current_event[1].time - current_sample);
						int32_t newdiff = diff * 0x10000 / speed;
						current_sample += (diff - newdiff);
						PRINT ("current_sample override %d - %d => %d\n", diff, newdiff, diff - newdiff);
					}
					current_event++;
				}
			}
		} else {
			rc = RC_ASYNC_HACK;
			break;
		}

	}

	return rc;
}

static int emulate_play_midi_file_iterate(const char *fn, struct emulate_play_midi_file_session *s)
{
	int i, rc;

	if (s->first)
	{
play_reload: /* Come here to reload MIDI file */
		PRINT ("RELOAD RELOAD RELOAD RC_TUNE_END perhaps?\n");
		s->first = 0;
		rc = play_midi_load_file((char *)fn, &s->event, &s->nsamples);

		dump_rc (rc);
		if (RC_IS_SKIP_FILE(rc))
			goto play_end; /* skip playing */

		init_mblock(&playmidi_pool);

		ctl_mode_event(CTLE_PLAY_START, 0, s->nsamples, 0);
		play_mode->acntl(PM_REQ_PLAY_START, NULL);

		rc = emulate_play_midi_start(s->event, s->nsamples);

		if (rc != RC_NONE)
			return rc;
	}

	rc = emulate_play_midi_iterate();

	if (rc == RC_ASYNC_HACK)
		return rc;

	/* shut down */

	play_mode->acntl(PM_REQ_PLAY_END, NULL);
	ctl_mode_event(CTLE_PLAY_END, 0, 0, 0);
	reuse_mblock(&playmidi_pool);

	for(i = 0; i < MAX_CHANNELS; i++)
	{
		memset(channel[i].drums, 0, sizeof(channel[i].drums));
	}

play_end:
	free_all_midi_file_info ();

	if(wrdt->opened)
		wrdt->end();

	if(free_instruments_afterwards)
	{
		int cnt;
		free_instruments(0);
		cnt = free_global_mblock(); /* free unused memory */
		if(cnt > 0)
		{
			ctl->cmsg(CMSG_INFO, VERB_VERBOSE, "%d memory blocks are free", cnt);
		}
	}

	free_special_patch(-1);

	if(s->event != NULL)
	{
		free(s->event);
		s->event = NULL;
	}
	if ((rc == RC_JUMP) || (rc == RC_RELOAD) || ((rc == RC_TUNE_END) && (!donotloop)) )
	{
		goto play_reload;
	} else {
		gmi_eof = 1;
	}

	return rc;
}

static void timidityIdler(void)
{
	int rc;

	while (1)
	{
		if (gmi_eof)
			break;

		if (gmibuffree > (audio_buffer_size*2))
		{
			rc = emulate_play_midi_file_iterate(current_path, &timidity_main_session);
			if (rc == RC_ASYNC_HACK)
				break;
		} else {
			break;
		}
	}
}

int __attribute__ ((visibility ("internal"))) timidityIsLooped(void)
{
	return gmi_looped;
}

void __attribute__ ((visibility ("internal"))) timiditySetLoop(unsigned char s)
{
	donotloop=!s;
}

void __attribute__ ((visibility ("internal"))) timidityPause(unsigned char p)
{
	inpause=p;
}

static void timiditySetVolume(void)
{
	volr=voll=vol*4;
	if (bal<0)
		voll=(voll*(64+bal))>>6;
	else
		volr=(volr*(64-bal))>>6;
}

static void SET(int ch, int opt, int val)
{
	switch (opt)
	{
		case mcpMasterSpeed:
			dspeed = val * 0x100;
			speed = (float)dspeed * ((float)0x10000 / gmibufrate);
			PRINT ("#1  dspeed=0x%08x gmibufrate=0x%08x speed=0x%08x\n", dspeed, gmibufrate, speed);
			break;
		case mcpMasterPitch:
			gmibufrate = val * 0x100;
			speed = (float)dspeed * ((float)0x10000 / gmibufrate);
			PRINT ("#2  dspeed=0x%08x gmibufrate=0x%08x speed=0x%08x\n", dspeed, gmibufrate, speed);
			break;
		case mcpMasterSurround:
			srnd=val;
			break;
		case mcpMasterPanning:
			pan=val;
			if (reversestereo)
			{
				pan = -pan;
			}
			timiditySetVolume();
			break;
		case mcpMasterVolume:
			vol=val;
			timiditySetVolume();
			break;
		case mcpMasterBalance:
			bal=val;
			timiditySetVolume();
			break;
	}
}
static int GET(int ch, int opt)
{
	return 0;
}

void __attribute__ ((visibility ("internal"))) timidityGetGlobInfo(struct mglobinfo *gi)
{
	int pos = plrGetPlayPos () >> (stereo+bit16);
	int32_t curtick = current_sample
			- aq_soft_filled()
			- ( gmibuffill ) /* gmibuf length */
			- ( (bufpos-pos+buflen)%buflen); /* PlrBuf length */
	if (curtick < 0)
	{
		curtick = 0;
	}
	gi->curtick = curtick;
	gi->ticknum = timidity_main_session.nsamples;
}

void __attribute__ ((visibility ("internal"))) timidityIdle(void)
{
	uint32_t bufdelta;
	uint32_t pass2;
/*
	uint32_t toloop;*/

	if (clipbusy++)
	{
		clipbusy--;
		return;
	}

again:
	{
		uint32_t bufplayed;

		bufplayed=plrGetBufPos()>>(stereo+bit16);
		bufdelta=(buflen+bufplayed-bufpos)%buflen;
	}
	/* bufdelta is now in samples */

	/* No delta on the devp? */
	if (!bufdelta)
	{
		clipbusy--;
		if (plrIdle)
			plrIdle();
		return;
	}

	timidityIdler();

	{
		uint32_t towrap=imuldiv(gmibuffill, 65536, gmibufrate);

		if (towrap == 0)
		{
			if (gmi_eof)
			{
				gmi_looped = 1;
			}

			clipbusy--;

			return;
		}

		if (bufdelta>towrap)
		{
			/*quietlen=bufdelta-towrap;*/
			bufdelta=towrap;
		}
	}

	if (inpause)
	{ /* If we are in pause, we fill buffer with the correct type of zeroes */
		if ((bufpos+bufdelta)>buflen)
			pass2=bufpos+bufdelta-buflen;
		else
			pass2=0;
		if (bit16)
		{
			plrClearBuf((uint16_t *)plrbuf+(bufpos<<stereo), (bufdelta-pass2)<<stereo, signedout);
			if (pass2)
				plrClearBuf((uint16_t *)plrbuf, pass2<<stereo, signedout);
		} else {
			plrClearBuf(buf16, bufdelta<<stereo, signedout);
			plr16to8((uint8_t *)plrbuf+(bufpos<<stereo), (uint16_t *)buf16, (bufdelta-pass2)<<stereo);
			if (pass2)
				plr16to8((uint8_t *)plrbuf, (uint16_t *)buf16+((bufdelta-pass2)<<stereo), pass2<<stereo);
		}
		bufpos+=bufdelta;
		if (bufpos>=buflen)
			bufpos-=buflen;
	} else {
		int buf16_filled = 0;
		uint32_t i;

		if (gmibufrate==0x10000)
		{
			while (buf16_filled<bufdelta) /* due to buffer-wrap */
			{
				int w=(bufdelta-buf16_filled);
				if ((gmibuflen-gmibuftail) < w) /* crop at buffer-wrap if needed */
				{
					w=gmibuflen-gmibuftail;
				}
				for (i=0; i < w; i++)
				{
					int16_t rs = ((int16_t *)gmibuf)[(gmibuftail<<1) + 0];
					int16_t ls = ((int16_t *)gmibuf)[(gmibuftail<<1) + 1];
					PANPROC;
					buf16[(buf16_filled<<1)+0] = rs;
					buf16[(buf16_filled<<1)+1] = ls;
					gmibuftail++;
					buf16_filled++;
				}
				if (gmibuftail >= gmibuflen)
				{
					gmibuftail = 0;
				}
				timidity_play_EventDelayed_gmibuf (w);
				gmibuffill-=w;
				gmibuffree+=w;
			}
		} else {
			int32_t aym1;
			uint32_t ay1, ay2;

			int32_t rc0, rc1, rc2, rc3, rvm1, rv1, rv2;
			int32_t lc0, lc1, lc2, lc3, lvm1, lv1, lv2;

			int16_t ls, rs;
			for (buf16_filled=0; buf16_filled<bufdelta; buf16_filled++)
			{
				aym1=gmibuftail-4; if (aym1<0) aym1+=gmibuflen;
				ay1=gmibuftail+4; if (ay1>=gmibuflen) ay1-=gmibuflen;
				ay2=gmibuftail+8; if (ay2>=gmibuflen) ay2-=gmibuflen;


				rc0 = *(uint16_t *)(gmibuf+(gmibuftail<<2))^0x8000;
				rvm1= *(uint16_t *)(gmibuf+(aym1<<2))^0x8000;
				rv1 = *(uint16_t *)(gmibuf+(ay1<<2))^0x8000;
				rv2 = *(uint16_t *)(gmibuf+(ay2<<2))^0x8000;
				rc1 = rv1-rvm1;
				rc2 = 2*rvm1-2*rc0+rv1-rv2;
				rc3 = rc0-rvm1-rv1+rv2;
				rc3 =  imulshr16(rc3,gmibuffpos);
				rc3 += rc2;
				rc3 =  imulshr16(rc3,gmibuffpos);
				rc3 += rc1;
				rc3 =  imulshr16(rc3,gmibuffpos);
				rc3 += rc0;
				if (rc3<0)
					rc3=0;
				if (rc3>65535)
					rc3=65535;

				lc0 = *(uint16_t *)(gmibuf+(gmibuftail<<2)+2)^0x8000;
				lvm1= *(uint16_t *)(gmibuf+(aym1<<2)+2)^0x8000;
				lv1 = *(uint16_t *)(gmibuf+(ay1<<2)+2)^0x8000;
				lv2 = *(uint16_t *)(gmibuf+(ay2<<2)+2)^0x8000;
				lc1 = lv1-lvm1;
				lc2 = 2*lvm1-2*lc0+lv1-lv2;
				lc3 = lc0-lvm1-lv1+lv2;
				lc3 =  imulshr16(lc3,gmibuffpos);
				lc3 += lc2;
				lc3 =  imulshr16(lc3,gmibuffpos);
				lc3 += lc1;
				lc3 =  imulshr16(lc3,gmibuffpos);
				lc3 += lc0;
				if (lc3<0)
					lc3=0;
				if (lc3>65535)
					lc3=65535;

				rs = rc3 ^ 0x8000;
				ls = lc3 ^ 0x8000;

				PANPROC;

				buf16[2*buf16_filled+0] = rs;
				buf16[2*buf16_filled+1] = ls;

				gmibuffpos+=gmibufrate;
				gmibuftail+=(gmibuffpos>>16);
				if (gmibuftail>=gmibuflen)
					gmibuftail-=gmibuflen;
				timidity_play_EventDelayed_gmibuf (gmibuffpos>>16);
				gmibuffill-=(gmibuffpos>>16);
				gmibuffree+=(gmibuffpos>>16);
				gmibuffpos&=0xFFFF;
			}
		}

		if ((bufpos+bufdelta)>buflen)
			pass2=bufpos+bufdelta-buflen;
		else
			pass2=0;
		bufdelta-=pass2;
		if (bit16)
		{
			if (stereo)
			{
				int16_t *p=(int16_t *)plrbuf+2*bufpos;
				int16_t *b=(int16_t *)buf16;
				if (signedout)
				{
					for (i=0; i<bufdelta; i++)
					{
						p[0]=b[0];
						p[1]=b[1];
						p+=2;
						b+=2;
					}
					p=(int16_t *)plrbuf;
					for (i=0; i<pass2; i++)
					{
						p[0]=b[0];
						p[1]=b[1];
						p+=2;
						b+=2;
					}
				} else {
					for (i=0; i<bufdelta; i++)
					{
						p[0]=b[0]^0x8000;
						p[1]=b[1]^0x8000;
						p+=2;
						b+=2;
					}
					p=(int16_t *)plrbuf;
					for (i=0; i<pass2; i++)
					{
						p[0]=b[0]^0x8000;
						p[1]=b[1]^0x8000;
						p+=2;
						b+=2;
					}
				}
			} else {
				int16_t *p=(int16_t *)plrbuf+bufpos;
				int16_t *b=(int16_t *)buf16;
				if (signedout)
				{
					for (i=0; i<bufdelta; i++)
					{
						p[0]=b[0];
						p++;
						b++;
					}
					p=(int16_t *)plrbuf;
					for (i=0; i<pass2; i++)
					{
						p[0]=b[0];
						p++;
						b++;
					}
				} else {
					for (i=0; i<bufdelta; i++)
					{
						p[0]=b[0]^0x8000;
						p++;
						b++;
					}
					p=(int16_t *)plrbuf;
					for (i=0; i<pass2; i++)
					{
						p[0]=b[0]^0x8000;
						p++;
						b++;
					}
				}
			}
		} else {
			if (stereo)
			{
				uint8_t *p=(uint8_t *)plrbuf+2*bufpos;
				uint8_t *b=(uint8_t *)buf16;
				if (signedout)
				{
					for (i=0; i<bufdelta; i++)
					{
						p[0]=b[1];
						p[1]=b[3];
						p+=2;
						b+=4;
					}
					p=(uint8_t *)plrbuf;
					for (i=0; i<pass2; i++)
					{
						p[0]=b[1];
						p[1]=b[3];
						p+=2;
						b+=4;
					}
				} else {
					for (i=0; i<bufdelta; i++)
					{
						p[0]=b[1]^0x80;
						p[1]=b[3]^0x80;
						p+=2;
						b+=4;
					}
					p=(uint8_t *)plrbuf;
					for (i=0; i<pass2; i++)
					{
						p[0]=b[1]^0x80;
						p[1]=b[3]^0x80;
						p+=2;
						b+=4;
					}
				}
			} else {
				uint8_t *p=(uint8_t *)plrbuf+bufpos;
				uint8_t *b=(uint8_t *)buf16;
				if (signedout)
				{
					for (i=0; i<bufdelta; i++)
					{
						p[0]=b[1];
						p++;
						b+=2;
					}
					p=(uint8_t *)plrbuf;
					for (i=0; i<pass2; i++)
					{
						p[0]=b[1];
						p++;
						b+=2;
					}
				} else {
					for (i=0; i<bufdelta; i++)
					{
						p[0]=b[1]^0x80;
						p++;
						b+=2;
					}
					p=(uint8_t *)plrbuf;
					for (i=0; i<pass2; i++)
					{
						p[0]=b[1]^0x80;
						p++;
						b+=2;
					}
				}
			}
		}
		bufpos+=bufdelta+pass2;
		if (bufpos>=buflen)
			bufpos-=buflen;
	}

	plrAdvanceTo(bufpos<<(stereo+bit16));

	{
		int pos = plrGetPlayPos () >> (stereo+bit16);
		timidity_SetBufferPos_EventDelayed_PlrBuf (eventDelayed_PlrBuf_lastpos, pos);
		eventDelayed_PlrBuf_lastpos = pos;
	}

	goto again;


	if (plrIdle)
		plrIdle();

	clipbusy--;
}

static void doTimidityClosePlayer(int CloseDriver)
{
	pollClose();

	if (CloseDriver)
	{
		plrClosePlayer();
	}

	free(buf16);
	buf16 = 0;

	free(gmibuf);
	gmibuf = 0;

	emulate_timidity_play_main_end ();

	emulate_main_end ();

	free (timidity_main_session.event);
	timidity_main_session.event = NULL;

	free (current_path);
	current_path = 0;

	while (EventDelayed_PlrBuf_head)
	{
		CtlEventDelayed *next = EventDelayed_PlrBuf_head->next;
		free_EventDelayed (EventDelayed_PlrBuf_head);
		EventDelayed_PlrBuf_head = next;
	}
	EventDelayed_PlrBuf_tail = 0;

	while (EventDelayed_gmibuf_head)
	{
		CtlEventDelayed *next = EventDelayed_gmibuf_head->next;
		free_EventDelayed (EventDelayed_gmibuf_head);
		EventDelayed_gmibuf_head = next;
	}
	EventDelayed_gmibuf_tail = 0;

	free_all_midi_file_info ();

	if (_SET)
	{
		mcpSet = _SET;
		_SET = 0;
	}

	if (_GET)
	{
		mcpGet = _GET;
		_GET = 0;
	}
}

int __attribute__ ((visibility ("internal"))) timidityOpenPlayer(const char *path, uint8_t *buffer, size_t bufferlen, struct ocpfilehandle_t *file)
{
	if (!plrPlay)
		return errGen;

	loading = 1;

	plrSetOptions(44100, (PLR_SIGNEDOUT|PLR_16BIT)|PLR_STEREO);

	stereo=!!(plrOpt&PLR_STEREO);
	bit16=!!(plrOpt&PLR_16BIT);
	signedout=!!(plrOpt&PLR_SIGNEDOUT);
	reversestereo=!!(plrOpt&PLR_REVERSESTEREO);

	ocp_playmode.rate = plrRate;

	emulate_main_start();
	inpause=0;
	voll=256;
	volr=256;
	pan=64;
	srnd=0;
	speed=0x100;
	loading = 0;

	if (!plrOpenPlayer(&plrbuf, &buflen, plrBufSize * plrRate / 1000, file))
	{
		return errGen;
	}
	PRINT ("(plr)buflen=%d\n", buflen);

	buf16=malloc(sizeof(uint16_t)*buflen*2);
	if (!buf16)
	{
		return errAllocMem;
	}
	bufpos=0;

	gmibuffree=1;
	while ((gmibuffree < (plrRate/8+1)) && (gmibuffree < (audio_buffer_size*3)))
	{
		gmibuffree<<=1;
	}
	gmibuflen=gmibuffree + ((plrRate*4/audio_buffer_size)+1)*audio_buffer_size; /* timidity force-flushes multiple seconds at the EOF */
	gmibuftail=0;
	gmibufhead=0;
	gmibuffill=0;
	gmibuf=malloc(gmibuflen<<2/*stereo+16bit*/);

	if (!gmibuf)
	{
		return errAllocMem;
	}
	gmibuffpos=0;

	gmi_looped=0;
	gmi_eof=0;

	if (emulate_timidity_play_main_start ())
	{
		return errGen;
	}

	eventDelayed_PlrBuf_lastpos = plrGetPlayPos() >> (stereo+bit16);

	current_path = strdup (path);
	emulate_play_midi_file_start(current_path, buffer, bufferlen, &timidity_main_session); /* gmibuflen etc must be set, since we will start to get events already here... */

	if (!pollInit(timidityIdle))
	{
		doTimidityClosePlayer (1);
		current_file_info->midi_data = NULL;
		current_file_info->midi_data_size = 0;
		return errGen;
	}

	_SET=mcpSet;
	_GET=mcpGet;
	mcpSet=SET;
	mcpGet=GET;

	mcpNormalize (mcpNormalizeNoFilter | mcpNormalizeCanSpeedPitchUnlock | mcpNormalizeCannotEcho | mcpNormalizeCannotAmplify);

	return errOk;
}

void __attribute__ ((visibility ("internal"))) timidityClosePlayer(void)
{
#warning we need to break idle loop with EVENT set to quit, in order to make a clean exit..
	doTimidityClosePlayer (1);
}
