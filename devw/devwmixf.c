/* OpenCP Module Player
 * copyright (c) 1994-'10 Niklas Beisert <nbeisert@physik.tu-muenchen.de>
 * copyright (c) 2004-'22 Stian Skjelstad <stian.skjelstad@gmail.com>
 *
 * Wavetable Device: FPU HighQuality Software Mixer for Pentium/above CPUs
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
 *
 * revision history: (please note changes here)
 *  -kb990208   Tammo Hinrichs <kb@vmo.nettrade.de>
 *    -first release
 *  -ryg990426  Fabian Giesen  <fabian@jdcs.su.nw.schule.de>
 *    -slave strikes back and applies some of kebbys changes of which i
 *     obviously don't exactly know what they are (i'm not interested in it
 *     either). blame kb if you wanna have sum description :)
 *  -ryg990504  Fabian Giesen  <fabian@jdcs.su.nw.schule.de>
 *    -added float postprocs. prepare for some really ruling effect filters
 *     in opencp soon...
 *
 * TODO:
 * - some hack to re-enable the display mixer, which is of course
 *   NOT capable of handling float values... and I don't want to
 *   store both versions of the samples, as this would suck QUITE
 *   too much memory
 *
 *
 * annotations:
 * This mixer is and will always be only half of what I want it to be.
 * The OpenCP 2.x.0 framework just does not contain the functionality
 * i would need to unleash this mixing routine's full potential, such
 * as almost infinite vol/pan resolution and perfect volume ramping.
 * so wait for the GSL core which will hopefully be contained in OpenCP 3.x
 * and will feature all this mixing routine can do. You've seen nothing yet.
 */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <sys/mman.h>
#include "types.h"
#include "boot/plinkman.h"
#include "dev/imsdev.h"
#include "dev/mcp.h"
#include "dev/mix.h"
#include "dev/player.h"
#include "stuff/imsrtns.h"
#include "devwmixf.h"
#include "stuff/poll.h"
#include "dwmixfa.h"

/* NB NB   more includes further down in the file */

extern struct sounddevice mcpFMixer;

static int dopause;
static uint32_t playsamps;
static uint32_t pausesamps;

static struct sampleinfo *samples;
static int samplenum;

static volatile int clipbusy=0;
static float amplify;
static float transform[2][2];
static int   volopt;
static uint32_t relpitch;
static int interpolation;
static void *plrbuf;

static int volramp;
static int declick;

static uint8_t stereo;
static uint8_t bit16;
static uint8_t signedout;
static uint8_t reversestereo;

static int     channelnum;

struct channel
{
	void *samp;

	uint32_t length;
	uint32_t loopstart;
	uint32_t loopend;

	int   newsamp;

	float dstvols[2];
	int   dontramp;

	float vol[2];
	float orgvol[2];

	float orgvolx;
	float orgpan;
	float orgfrez;

	float *sbpos;
	float sbuf[8];

	uint32_t orgrate;
	uint32_t orgfrq;
	uint32_t orgdiv;
	int      volopt;
	uint32_t samptype;

	uint32_t orgloopstart;
	uint32_t orgloopend;
	uint32_t orgsloopstart;
	uint32_t orgsloopend;

	long  handle;
};
static struct channel *channels;

static unsigned int bufpos;
static uint32_t buflen;

static void (*playerproc)(void);

static uint32_t tickwidth;
static uint32_t tickplayed;
static uint32_t orgspeed;
static uint32_t relspeed;
static uint32_t newtickwidth;
static uint32_t cmdtimerpos;

static float mastervol;
static float masterbal;
static float masterpan;
static int   mastersrnd;
static int   masterrvb;
static int   masterchr;

static inline float frchk(float a)
{
	switch (fpclassify(a))
	{
		default:
		case FP_NAN:
		case FP_INFINITE:
		case FP_SUBNORMAL:

		case FP_ZERO:
			return 0.0f;

		case FP_NORMAL:
			if (fabs(a)<0.00000001)
				return 0.0;
			else
				return a;
	}
}

static void calcinterpoltab(void)
{
	int i;
	for (i=0; i<256; i++)
	{
		float x1=i/256.0;
		float x2=x1*x1;
		float x3=x1*x1*x1;
		dwmixfa_state.ct0[i]=-0.5*x3+x2-0.5*x1;
		dwmixfa_state.ct1[i]=1.5*x3-2.5*x2+1;
		dwmixfa_state.ct2[i]=-1.5*x3+2*x2+0.5*x1;
		dwmixfa_state.ct3[i]=0.5*x3-0.5*x2;
	};
}


static void calcstep(struct channel *c)
{
	int n=c->handle;
	uint32_t rstep;
	if (!(dwmixfa_state.voiceflags[n]&MIXF_PLAYING))
		return;
	if (!c->orgdiv)
		return;

	rstep=imuldiv(imuldiv(c->orgfrq, c->orgrate, c->orgdiv)<<8, relpitch, dwmixfa_state.samprate);
	dwmixfa_state.freqw[n]=rstep>>16;
	dwmixfa_state.freqf[n]=rstep<<16;

	dwmixfa_state.voiceflags[n]&=~(MIXF_INTERPOLATE|MIXF_INTERPOLATEQ);
	dwmixfa_state.voiceflags[n]|=interpolation?((interpolation>1)?MIXF_INTERPOLATEQ:MIXF_INTERPOLATE):0;
}

static void calcsteps(void)
{
	int i;
	for (i=0; i<channelnum; i++)
		calcstep(&channels[i]);
}

static void calcspeed(void)
{
	if (channelnum)
		newtickwidth=imuldiv(256*256*256, dwmixfa_state.samprate, orgspeed*relspeed);
}

static void transformvol(struct channel *ch)
{
	int n=ch->handle;

	ch->vol[0]=transform[0][0]*ch->orgvol[0]+transform[0][1]*ch->orgvol[1];
	ch->vol[1]=transform[1][0]*ch->orgvol[0]+transform[1][1]*ch->orgvol[1];
	if (volopt^ch->volopt)
		ch->vol[1]=-ch->vol[1];

	if (dwmixfa_state.voiceflags[n]&MIXF_MUTE)
	{
		ch->dstvols[0]=ch->dstvols[1]=0;
	} else if (!stereo)
	{
		ch->dstvols[0]=0.5*(fabs(ch->vol[0])+fabs(ch->vol[1]));
		ch->dstvols[1]=0;
	} else if (reversestereo)
	{
		ch->dstvols[0]=ch->vol[1];
		ch->dstvols[1]=ch->vol[0];
	} else {
		ch->dstvols[0]=ch->vol[0];
		ch->dstvols[1]=ch->vol[1];
	}
}

static void calcvol(struct channel *chn)
{
	chn->orgvol[0]=chn->orgvolx*(0.5-chn->orgpan);
	chn->orgvol[1]=chn->orgvolx*(0.5+chn->orgpan);
	transformvol(chn);
}

static void calcvols(void)
{
	float vols[2][2];

	float realamp=(1.0/65536.0)*amplify;
	int i;

	vols[0][0]=vols[1][1]=mastervol*(0.5+masterpan);
	vols[0][1]=vols[1][0]=mastervol*(0.5-masterpan);

	if (masterbal>0)
	{
		vols[0][0]=vols[0][0]*(0.5-masterbal);
		vols[0][1]=vols[0][1]*(0.5-masterbal);
	} else if (masterbal<0)
	{
		vols[1][0]=vols[1][0]*(0.5+masterbal);
		vols[1][1]=vols[1][1]*(0.5+masterbal);
	}

	volopt=mastersrnd;
	transform[0][0]=realamp*vols[0][0];
	transform[0][1]=realamp*vols[0][1];
	transform[1][0]=realamp*vols[1][0];
	transform[1][1]=realamp*vols[1][1];

	for (i=0; i<channelnum; i++)
		transformvol(&channels[i]);
}


static void stopchan(struct channel *c)
{
	int n=c->handle;
	if (!(dwmixfa_state.voiceflags[n]&MIXF_PLAYING))
		return;

	/* cool end-of-sample-declicking */
	if (!(dwmixfa_state.voiceflags[n] & MIXF_QUIET))
	{
		int offs = ( dwmixfa_state.voiceflags[n] & MIXF_INTERPOLATEQ ) ? 1 : 0;
		float ff2 = dwmixfa_state.ffreq[n] * dwmixfa_state.ffreq[n];
		dwmixfa_state.fadeleft += ff2*dwmixfa_state.volleft[n] * dwmixfa_state.smpposw[n][offs];
		dwmixfa_state.faderight += ff2*dwmixfa_state.volright[n] * dwmixfa_state.smpposw[n][offs];
	}

	dwmixfa_state.voiceflags[n]&=~MIXF_PLAYING;
}


static void rstlbuf(struct channel *c)
{
	if (c->sbpos)
	{
		int i;
		for (i=0; i<8; i++)
			c->sbpos[i]=c->sbuf[i];
		c->sbpos=0;
	}
}


static void setlbuf(struct channel *c)
{
	int n=c->handle;

	if (c->sbpos)
		rstlbuf(c);

	if (dwmixfa_state.voiceflags[n]&MIXF_LOOPED)
	{
		float *dst=dwmixfa_state.loopend[n];
		float *src=dst-dwmixfa_state.looplen[n];
		int i;
		for (i=0; i<8; i++)
		{
			c->sbuf[i]=dst[i];
			dst[i]=src[i];
		}
		c->sbpos=dst;
	}
}



static void mixmain(/*int min*/)
{
	int i;
	int /*bufmin,*/
	    bufdeltatot;

	if (!channelnum)
		return;
	if (clipbusy++)
	{
		clipbusy--;
		return;
	}


/*
	bufmin=imulshr16(min, samprate);
	if (bufmin<0)
		bufmin=0;*/

	bufdeltatot=((buflen+(plrGetBufPos()>>(stereo+bit16))-bufpos)%buflen);
/*
	if (bufdeltatot<bufmin)
		bufdeltatot=0;*/

	if (dopause)
	{
		int pass2=((bufpos+bufdeltatot)>buflen)?(bufpos+bufdeltatot-buflen):0;

		if (bit16)
		{
			memsetw((int16_t *)plrbuf+(bufpos<<stereo), signedout?0:0x8000, (bufdeltatot-pass2)<<stereo);
			if (pass2)
				memsetw((int16_t *)plrbuf, signedout?0:0x8000, pass2<<stereo);
		} else {
			memsetb((uint8_t *)plrbuf+(bufpos<<stereo), signedout?0:0x80, (bufdeltatot-pass2)<<stereo);
			if (pass2)
				memsetb((uint8_t *)plrbuf, signedout?0:0x80, pass2<<stereo);
		}

		bufpos+=bufdeltatot;
		if (bufpos>=buflen)
			bufpos-=buflen;

		plrAdvanceTo(bufpos<<(stereo+bit16));
		pausesamps+=bufdeltatot;
	} else while (bufdeltatot>0)
	{
		unsigned int bufdelta=(bufdeltatot>MIXF_MIXBUFLEN)?MIXF_MIXBUFLEN:bufdeltatot;
		uint32_t ticks2go;
		/* float invt2g; */
		if (bufdelta>(buflen-bufpos))
			bufdelta=buflen-bufpos;

		ticks2go=(tickwidth-tickplayed)>>8;

		if (bufdelta>ticks2go)
			bufdelta=ticks2go;

		/* invt2g=1.0/(float)ticks2go; */

		/* set-up mixing core variables */
		for (i=0; i<channelnum; i++) if (dwmixfa_state.voiceflags[i]&MIXF_PLAYING)
		{
			struct channel *ch=&channels[i];

			dwmixfa_state.volleft[i]=frchk(dwmixfa_state.volleft[i]);
			dwmixfa_state.volright[i]=frchk(dwmixfa_state.volright[i]);

			if (!dwmixfa_state.volleft[i] && !dwmixfa_state.volright[i] && !dwmixfa_state.rampleft[i] && !dwmixfa_state.rampright[i])
				dwmixfa_state.voiceflags[i]|=MIXF_QUIET;
			else
				dwmixfa_state.voiceflags[i]&=~MIXF_QUIET;


			if (dwmixfa_state.ffreq[i]!=1 || dwmixfa_state.freso[i]!=0)
				dwmixfa_state.voiceflags[i]|=MIXF_FILTER;
			else
				dwmixfa_state.voiceflags[i]&=~MIXF_FILTER;

			/* declick start of sample */
			if (ch->newsamp)
			{
				if (!(dwmixfa_state.voiceflags[i] & MIXF_QUIET))
				{
					int offs = ( dwmixfa_state.voiceflags[i] & MIXF_INTERPOLATEQ ) ? 1 : 0;
					float ff2 = dwmixfa_state.ffreq[i] * dwmixfa_state.ffreq[i];
					dwmixfa_state.fadeleft -= ff2 * dwmixfa_state.volleft[i] * dwmixfa_state.smpposw[i][offs];
					dwmixfa_state.faderight -= ff2 * dwmixfa_state.volright[i] * dwmixfa_state.smpposw[i][offs];
				}
				ch->newsamp=0;
			}

			/*voiceflags[i]|=isstereo; <- this was hard to track down*/
		}
		dwmixfa_state.nsamples=bufdelta;
		dwmixfa_state.outbuf=((uint8_t *)(plrbuf))+(bufpos<<(stereo+bit16));

		/* optionally turn off the declicking */
		if (!declick)
			dwmixfa_state.fadeleft=dwmixfa_state.faderight=0;
#ifdef MIXER_DEBUG
		{
			int i;
			fprintf(stderr, "nsamples=%d\n", nsamples);
			for (i=0;i<nvoices;i++)
				if (voiceflags[i])
				{
					fprintf(stderr, "voice #%d\n", i);
					fprintf(stderr, "  samp: %p (%p)\n", channels[i].samp, channels[i].sbpos);
					fprintf(stderr, "  looplen: %08x\n", looplen[i]);
					fprintf(stderr, "  voiceflags: 0x%08x\n", voiceflags[i]);
					fprintf(stderr, "  freq: 0x%08x:0x%08x\n", freqw[i], freqf[i]);
					fprintf(stderr, "  vol %lf %lf\n", volleft[i], volright[i]);
				}
			fprintf(stderr, "\n");
		}
#endif
		mixer();

		tickplayed+=bufdelta<<8;
		if (!((tickwidth-tickplayed)>>8))
		{
			float invt2g;
			tickplayed-=tickwidth;

			playerproc();

			cmdtimerpos+=tickwidth;
			tickwidth=newtickwidth;
			invt2g=256.0/tickwidth;

			/* set up volume ramping */
			for (i=0; i<channelnum; i++) if (dwmixfa_state.voiceflags[i]&MIXF_PLAYING)
			{

				struct channel *ch=&channels[i];
				if (ch->dontramp)
				{
					dwmixfa_state.volleft[i]=frchk(ch->dstvols[0]);
					dwmixfa_state.volright[i]=frchk(ch->dstvols[1]);
					dwmixfa_state.rampleft[i]=dwmixfa_state.rampright[i]=0;
					if (volramp)
						ch->dontramp=0;
				} else {
					dwmixfa_state.rampleft[i]=frchk(invt2g*(ch->dstvols[0]-dwmixfa_state.volleft[i]));
					if (dwmixfa_state.rampleft[i]==0)
						dwmixfa_state.volleft[i]=ch->dstvols[0];
					dwmixfa_state.rampright[i]=frchk(invt2g*(ch->dstvols[1]-dwmixfa_state.volright[i]));
					if (dwmixfa_state.rampright[i]==0)
						dwmixfa_state.volright[i]=ch->dstvols[1];
				}
				/* filter resonance */
				dwmixfa_state.freso[i]=pow(ch->orgfrez,dwmixfa_state.ffreq[i]);
			}
		}
		bufpos+=bufdelta;
		if (bufpos>=buflen)
			bufpos-=buflen;

		plrAdvanceTo(bufpos<<(stereo+bit16));
		bufdeltatot-=bufdelta;
		playsamps+=bufdelta;
	}

	clipbusy--;
}

static void timerproc()
{
#ifndef NO_BACKGROUND_MIXER
	mixmain();
#endif
	if (plrIdle)
		plrIdle();
}

static void SET(int ch, int opt, int val)
{
	struct channel *chn;
	if (ch>=channelnum)
		ch=channelnum-1;
	if (ch<0)
		ch=0;
	chn=&channels[ch];
	switch (opt)
	{
		case mcpCReset:
			{
				int reswasmute;
				rstlbuf(chn);
				stopchan(chn);
				reswasmute=dwmixfa_state.voiceflags[ch]&MIXF_MUTE;
				memset(chn, 0, sizeof(struct channel));
				chn->handle=ch;
				dwmixfa_state.voiceflags[ch]=reswasmute;
			}
			break;

		case mcpCInstrument:
			{
				struct sampleinfo *samp;

				rstlbuf(chn);
				stopchan(chn);
				if ((val<0)||(val>=samplenum))
					break;
				samp=&samples[val];
				chn->samptype=samp->type;
				chn->length=samp->length;
				chn->orgrate=samp->samprate;
				chn->samp=samp->ptr;
				chn->orgloopstart=samp->loopstart;
				chn->orgloopend=samp->loopend;
				chn->orgsloopstart=samp->sloopstart;
				chn->orgsloopend=samp->sloopend;
				chn->dontramp=1;
				chn->newsamp=1;

				dwmixfa_state.voiceflags[ch]&=~(MIXF_PLAYING|MIXF_LOOPED);

				dwmixfa_state.freqw[ch]=0;
				dwmixfa_state.freqf[ch]=0;
				dwmixfa_state.fl1[ch]=0;
				dwmixfa_state.fb1[ch]=0;
				dwmixfa_state.ffreq[ch]=1;
				dwmixfa_state.freso[ch]=0;
				dwmixfa_state.smpposf[ch]=0;
				dwmixfa_state.smpposw[ch]=(float *)chn->samp;

				if (chn->samptype&mcpSampSLoop)
				{
					dwmixfa_state.voiceflags[ch]|=MIXF_LOOPED;
					chn->loopstart=chn->orgsloopstart;
					chn->loopend=chn->orgsloopend;
				} else if (chn->samptype&mcpSampLoop)
				{
					dwmixfa_state.voiceflags[ch]|=MIXF_LOOPED;
					chn->loopstart=chn->orgloopstart;
					chn->loopend=chn->orgloopend;
				}

				if (dwmixfa_state.voiceflags[ch]&MIXF_LOOPED)
				{
					dwmixfa_state.looplen[ch]=chn->loopend-chn->loopstart;
					dwmixfa_state.loopend[ch]=(float *)chn->samp+chn->loopend;
				} else {
					dwmixfa_state.looplen[ch]=chn->length;
					dwmixfa_state.loopend[ch]=(float *)chn->samp+chn->length-1;
				}
				setlbuf(chn);
			}
			break;
		case mcpCStatus:
			if (!val)
				stopchan(chn);
			else {
				if (dwmixfa_state.smpposw[ch] >= (float *)(chn->samp)+chn->length)
					break;
				dwmixfa_state.voiceflags[ch]|=MIXF_PLAYING;
				calcstep(chn);
			}
			break;
		case mcpCMute:
			if (val)
				dwmixfa_state.voiceflags[ch]|=MIXF_MUTE;
			else
				dwmixfa_state.voiceflags[ch]&=~MIXF_MUTE;
			calcvol(chn);
			break;
		case mcpCVolume:
			val=(val>0x200)?0x200:(val<0)?0:val;
			chn->orgvolx=(float)(val)/256.0;
			calcvol(chn);
			break;
		case mcpCPanning:
			val=(val>0x80)?0x80:(val<-0x80)?-0x80:val;
			chn->orgpan=(float)(val)/256.0;
			calcvol(chn);
			break;
		case mcpCSurround:
			chn->volopt=val?1:0;
			transformvol(chn);
			break;
		case mcpCLoop:
			rstlbuf(chn);
			dwmixfa_state.voiceflags[ch]&=~MIXF_LOOPED;

			if ((val==1)&&!(chn->samptype&mcpSampSLoop))
				val=2;
			if ((val==2)&&!(chn->samptype&mcpSampLoop))
				val=0;

			if (val==1)
			{
				dwmixfa_state.voiceflags[ch]|=MIXF_LOOPED;
				chn->loopstart=chn->orgsloopstart;
				chn->loopend=chn->orgsloopend;
			}
			if (val==2)
			{
				dwmixfa_state.voiceflags[ch]|=MIXF_LOOPED;
				chn->loopstart=chn->orgloopstart;
				chn->loopend=chn->orgloopend;
			}

			if (dwmixfa_state.voiceflags[ch]&MIXF_LOOPED)
			{
				dwmixfa_state.looplen[ch]=chn->loopend-chn->loopstart;
				dwmixfa_state.loopend[ch]=(float *)chn->samp+chn->loopend;
			} else {
				dwmixfa_state.looplen[ch]=chn->length;
				dwmixfa_state.loopend[ch]=(float *)chn->samp+chn->length-1;
			}
			setlbuf(chn);

			break;
		case mcpCPosition:
			{
				int poswasplaying;
				poswasplaying=dwmixfa_state.voiceflags[ch]&MIXF_PLAYING;
				stopchan(chn);
				chn->newsamp=1;
				if (val<0)
					val=0;
				if ((unsigned)val>=chn->length)
					val=chn->length-1;
				dwmixfa_state.smpposw[ch]=(float *)(chn->samp)+val;
				dwmixfa_state.smpposf[ch]=0;
				dwmixfa_state.voiceflags[ch]|=poswasplaying;
			}
			break;
		case mcpCPitch:
			chn->orgfrq=8363;
			chn->orgdiv=mcpGetFreq8363(-val);
			calcstep(chn);
			break;
		case mcpCPitchFix:
			chn->orgfrq=val;
			chn->orgdiv=0x10000;
			calcstep(chn);
			break;
		case mcpCPitch6848:
			chn->orgfrq=6848;
			chn->orgdiv=val;
			calcstep(chn);
			break;
		case mcpCFilterFreq:
			/*
			   cputs("ch");
			   cputs(utoa(ch,0,10));
			   cputs(": freq ");
			   cputs(utoa(val,0,16));
			   cputs("\r\n");
			 */
			if (!(val&128))
			{
				dwmixfa_state.ffreq[ch]=1;
				dwmixfa_state.freso[ch]=0;
				break;
			}
			dwmixfa_state.ffreq[ch]=33075.0*pow(2,(val-255)/24.0)/dwmixfa_state.samprate;
			if (dwmixfa_state.ffreq[ch]<0)
				dwmixfa_state.ffreq[ch]=0;
			if (dwmixfa_state.ffreq[ch]>1)
				dwmixfa_state.ffreq[ch]=1;
			break;
		case mcpCFilterRez:
			chn->orgfrez=val/300.0;
			if (chn->orgfrez>1)
				chn->orgfrez=1;
			if (chn->orgfrez==0 && dwmixfa_state.ffreq[ch]==0)
				dwmixfa_state.ffreq[ch]=1;
			break;
		case mcpGSpeed:
			orgspeed=val;
			calcspeed();
			break;
		case mcpMasterVolume:
			if (val>=0 && val<=64)
				mastervol=(float)(val)/64.0;
			calcvols();
			break;
		case mcpMasterPanning:
			if (val>=-64 && val<=64)
				masterpan=(float)(val)/128.0;
			calcvols();
			break;
		case mcpMasterBalance:
			if (val>=-64 && val<=64)
				masterbal=(float)(val)/128.0;
			calcvols();
			break;
		case mcpMasterSurround:
			mastersrnd=val?1:0;
			calcvols();
			break;
		case mcpMasterReverb:
			masterrvb=(val>=64)?63:(val<-64)?-64:val;
			break;
		case mcpMasterChorus:
			masterchr=(val>=64)?63:(val<-64)?-64:val;
			break;
		case mcpMasterSpeed:
			relspeed=(val<16)?16:val;
			calcspeed();
			break;
		case mcpMasterPitch:
			relpitch=val;
			calcsteps();
			break;
		case mcpMasterFilter:
			interpolation=val;
			break;
		case mcpMasterAmplify:
			amplify=val;
			if (channelnum)
				mixSetAmplify(amplify);
			calcvols();
			break;
		case mcpMasterPause:
			dopause=val;
			break;
	}
}

static int GET(int ch, int opt)
{
/*
	struct channel *chn;*/
	if (ch>=channelnum)
		ch=channelnum-1;
	if (ch<0)
		ch=0;
/*
	chn=&channels[ch];*/   /* No commands currently use the channel itself */
	switch (opt)
	{
		case mcpCStatus:
			return !!(dwmixfa_state.voiceflags[ch]&MIXF_PLAYING);
		case mcpCMute:
			return !!(dwmixfa_state.voiceflags[ch]&MIXF_MUTE);
		case mcpGTimer:
			if (dopause)
				return imuldiv(playsamps, 65536, dwmixfa_state.samprate);
			else
				return plrGetTimer()-imuldiv(pausesamps, 65536, dwmixfa_state.samprate);
		case mcpGCmdTimer:
			return umuldiv(cmdtimerpos, 256, dwmixfa_state.samprate);
		case mcpMasterReverb:
			return masterrvb;
		case mcpMasterChorus:
			return masterchr;
	}
	return 0;
}

static void Idle(void)
{
	mixmain();
	if (plrIdle)
		plrIdle();
}




/* Houston, we've got a problem there... the display mixer isn't
 * able to handle floating point values at all. shit.
 * (kebby, irgendwann mal: "ich will das nicht")

 * bla, toller hack: es funktioniert. (fd)
 */

static void GetMixChannel(unsigned int ch, struct mixchannel *chn, uint32_t rate)
{
	struct channel *c=&channels[ch];

	chn->samp=c->samp;
	chn->realsamp.fmt=c->samp;
	chn->length=c->length;
	chn->loopstart=c->loopstart;
	chn->loopend=c->loopend;
	chn->fpos=dwmixfa_state.smpposf[ch]>>16;
	chn->pos=dwmixfa_state.smpposw[ch]-(float*)c->samp;
	chn->vol.volfs[0]=fabs(c->vol[0]);
	chn->vol.volfs[1]=fabs(c->vol[1]);
	chn->step=imuldiv((dwmixfa_state.freqw[ch]<<16)|(dwmixfa_state.freqf[ch]>>16), dwmixfa_state.samprate, (signed)rate);
	chn->status=MIX_PLAYFLOAT;
	if (dwmixfa_state.voiceflags[ch]&MIXF_MUTE)
		chn->status|=MIX_MUTE;
	if (dwmixfa_state.voiceflags[ch]&MIXF_LOOPED)
		chn->status|=MIX_LOOPED;
	if (dwmixfa_state.voiceflags[ch]&MIXF_PLAYING)
		chn->status|=MIX_PLAYING;
	if (dwmixfa_state.voiceflags[ch]&MIXF_INTERPOLATE)
		chn->status|=MIX_INTERPOLATE;
}


static void getrealvol(int ch, int *l, int *r)
{
	getchanvol(ch, 256);
	if (dwmixfa_state.voll<0)
		dwmixfa_state.voll=-dwmixfa_state.voll;
	*l=(dwmixfa_state.voll>16319)?255:(dwmixfa_state.voll/64.0);
	if (dwmixfa_state.volr<0)
		dwmixfa_state.volr=-dwmixfa_state.volr;
	*r=(dwmixfa_state.volr>16319)?255:(dwmixfa_state.volr/64.0);
}

static int LoadSamples(struct sampleinfo *sil, int n)
{
	if (!mcpReduceSamples(sil, n, 0x40000000, mcpRedToMono|mcpRedToFloat|mcpRedNoPingPong))
		return 0;

#ifdef MIXER_DEBUG
	{
		int i;
		for (i=0;i<n;i++)
			fprintf(stderr, "sample #% 3d: %p - %p\n", i, sil[i].ptr, sil[i].ptr + sil[i].length*sizeof(float));
	}
#endif

	samples=sil;
	samplenum=n;

	return 1;
}

static int OpenPlayer(int chan, void (*proc)(void), struct ocpfilehandle_t *source_file)
{
	uint32_t currentrate;
	uint16_t mixfate;
	int i;

	playsamps=pausesamps=0;
	if (chan>MIXF_MAXCHAN)
		chan=MIXF_MAXCHAN;

	if (!plrPlay)
		return 0;

	currentrate=mcpMixProcRate/chan;
	mixfate=(currentrate>mcpMixMaxRate)?mcpMixMaxRate:currentrate;
	plrSetOptions(mixfate, mcpMixOpt);

	playerproc=proc;

	if (!(dwmixfa_state.tempbuf=malloc(sizeof(float)*(MIXF_MIXBUFLEN<<1))))
		return 0;
	if (!(channels=calloc(sizeof(struct channel), chan)))
	{
		free(channels);
		return 0;
	}

	mcpGetMasterSample=plrGetMasterSample;
	mcpGetRealMasterVolume=plrGetRealMasterVolume;

	if (!mixInit(GetMixChannel, 0, chan, amplify))
		return 0;
	mcpGetRealVolume=getrealvol;

	calcvols();

	for (i=0; i<chan; i++)
	{
		channels[i].handle=i;
		dwmixfa_state.voiceflags[i]=0;
	}


	if (!plrOpenPlayer(&plrbuf, &buflen, mcpMixBufSize * plrRate / 1000, source_file))
	{
		mixClose();
		return 0;
	}

	stereo=(plrOpt&PLR_STEREO)?1:0;
	bit16=(plrOpt&PLR_16BIT)?1:0;
	signedout=(plrOpt&PLR_SIGNEDOUT)?1:0;
	reversestereo=!!(plrOpt&PLR_REVERSESTEREO);
	dwmixfa_state.samprate=plrRate;
	bufpos=0;
	dopause=0;
	orgspeed=12800;

	channelnum=chan;
	mcpNChan=chan;
	mcpIdle=Idle;

	dwmixfa_state.isstereo=stereo;
	dwmixfa_state.outfmt=(bit16<<1)|(!signedout);
	dwmixfa_state.nvoices=channelnum;
	prepare_mixer();

	calcspeed();
	/*/  playerproc();*/  /* some timing is wrong here! */
	tickwidth=newtickwidth;
	tickplayed=0;
	cmdtimerpos=0;

	if (!pollInit(timerproc))
	{
		mcpNChan=0;
		mcpIdle=0;
		plrClosePlayer();
		mixClose();
		return 0;
	}
	{
		struct mixfpostprocregstruct *mode;

		for (mode=dwmixfa_state.postprocs; mode; mode=mode->next)
			if (mode->Init) mode->Init(dwmixfa_state.samprate, stereo);
	}
	return 1;
}

static void ClosePlayer()
{
	struct mixfpostprocregstruct *mode;

	mcpNChan=0;
	mcpIdle=0;

	pollClose();

	plrClosePlayer();

	channelnum=0;

	mixClose();

	for (mode=dwmixfa_state.postprocs; mode; mode=mode->next)
		if (mode->Close) mode->Close();

	free(channels);
	free(dwmixfa_state.tempbuf);
	dwmixfa_state.tempbuf = 0;
}

static int Init(const struct deviceinfo *dev)
{
	volramp=!!(dev->opt&MIXF_VOLRAMP);
	declick=!!(dev->opt&MIXF_DECLICK);

	calcinterpoltab();

	amplify=65535;
	relspeed=256;
	relpitch=256;
	interpolation=0;
	mastervol=64;
	masterbal=0;
	masterpan=0;
	mastersrnd=0;
	channelnum=0;

	mcpLoadSamples=LoadSamples;
	mcpOpenPlayer=OpenPlayer;
	mcpClosePlayer=ClosePlayer;
	mcpGet=GET;
	mcpSet=SET;

	return 1;
}

static void Close()
{
	mcpOpenPlayer=0;
}


static int Detect(struct deviceinfo *c)
{
	c->devtype=&mcpFMixer;
	c->port=-1;
	c->port2=-1;
/*
	c->irq=-1;
	c->irq2=-1;
	c->dma=-1;
	c->dma2=-1;
*/
	if (c->subtype==-1)
		c->subtype=0;
	c->chan=MIXF_MAXCHAN;
	c->mem=0;
	c->path[0]=0;
	return 1;
}



#include "dev/devigen.h"
#include "boot/psetting.h"
#include "dev/deviplay.h"
#include "boot/plinkman.h"

static struct mixfpostprocaddregstruct *postprocadds;

static uint32_t mixfGetOpt(const char *sec)
{
	uint32_t opt=0;
	if (cfGetProfileBool(sec, "volramp", 1, 1))
		opt|=MIXF_VOLRAMP;
	if (cfGetProfileBool(sec, "declick", 1, 1))
		opt|=MIXF_DECLICK;
	return opt;
}

void mixfRegisterPostProc(struct mixfpostprocregstruct *mode)
{
	mode->next=dwmixfa_state.postprocs;
	dwmixfa_state.postprocs=mode;
}

static void mixfInit(const char *sec)
{
	char regname[50];
	const char *regs;

	fprintf(stderr, "[devwmixf] INIT, ");
	fprintf(stderr, "using dwmixfa.c C version\n");

	dwmixfa_state.postprocs=0;
	regs=cfGetProfileString(sec, "postprocs", "");

	while (cfGetSpaceListEntry(regname, &regs, 49))
	{
		void *reg=_lnkGetSymbol(regname);
		if (reg)
			mixfRegisterPostProc((struct mixfpostprocregstruct*)reg);
	}

	postprocadds=0;
	regs=cfGetProfileString(sec, "postprocadds", "");
	while (cfGetSpaceListEntry(regname, &regs, 49))
	{
		void *reg=_lnkGetSymbol(regname);
		if (reg)
		{
			((struct mixfpostprocaddregstruct*)reg)->next=postprocadds;
			postprocadds=(struct mixfpostprocaddregstruct*)reg;
		}
	}
}

static int mixfProcKey(uint16_t key)
{
	struct mixfpostprocaddregstruct *mode;
	for (mode=postprocadds; mode; mode=mode->next)
	{
		int r=mode->ProcessKey(key);
		if (r)
			return r;
	}

	if (plrProcessKey)
		return plrProcessKey(key);
	return 0;
}

struct devaddstruct mcpFMixAdd = {mixfGetOpt, mixfInit, 0, mixfProcKey};
struct sounddevice mcpFMixer={SS_WAVETABLE|SS_NEEDPLAYER, 0, "FPU Mixer", Detect, Init, Close, &mcpFMixAdd};
char *dllinfo="driver mcpFMixer";

struct linkinfostruct dllextinfo = {.name = "devwmixf", .desc = "OpenCP Wavetable Device: FPU HighQuality Mixer (c) 1999-'22 Tammo Hinrichs, Fabian Giesen, Stian Skjelstad, Jindřich Makovička", .ver = DLLVERSION, .size = 0};
