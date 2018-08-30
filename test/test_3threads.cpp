#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <sys/prctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include "push_sinc_resampler.h"
#include "echo_cancellation.h"
#include "audio_buffer.h"
#include "audio_util.h"

#define MAX_SAMPLERATE   (48000)
#define MAX_SAMPLES_10MS ((MAX_SAMPLERATE*10)/1000)
#define MAX_SUBBANS_NUM  (3)
#define MIC_SAMPLERATE			44100
#define REF_SAMPLERATE			44100
#define AEC_SAMPLERATE			44100
#define MIC_IN_FRAMESAMPLES 	480
#define REF_IN_FRAMESAMPLES 	480
#define MIC_OUT_FRAMESAMPLES 	441		  	// 10ms
#define REF_OUT_FRAMESAMPLES 	441		  	// 10ms
#define AEC_FRAMESAMPLES 		441		   	// 10ms
#define MIC_RINGBUFF_LEN (MIC_SAMPLERATE*100/1000) // 100ms
#define REF_RINGBUFF_LEN (REF_SAMPLERATE*140/1000) // 140ms

using namespace webrtc;

typedef struct 
{
    RingBuffer *pring;
    char sfilepath[80];
	int  worksamplerate;
    int  framesamples;
    volatile bool bcontinue[256];
	volatile bool bfinished[256];
	volatile bool bcreated[256];
	volatile bool *pbcontinue;
	volatile bool *pbfinished;
	volatile bool *pbcreated;
}MicThreadParams;

typedef struct
{
    RingBuffer *pring;
    char sfilepath[80];
	int  worksamplerate;
    int  framesamples;
    volatile bool bcontinue[256];
	volatile bool bfinished[256];
	volatile bool bcreated[256];
	volatile bool *pbcontinue;
	volatile bool *pbfinished;
	volatile bool *pbcreated;
}RefThreadParams;

typedef struct
{
    RingBuffer *pmicring;
	RingBuffer *prefring;
    char sfilepath[80];
    int  nmicdelaysamples;
    int  nrefdelaysamples;
    int  micframesamples;
    int  refframesamples;
    int  aecframesamples;
    int  worksamplerate;
    int  framesamples;
    volatile bool bcontinue[256];
	volatile bool bfinished[256];
	volatile bool bcreated[256];
	volatile bool *pbcontinue;
	volatile bool *pbfinished;
	volatile bool *pbcreated;	
}AecThreadParams;

static void S16ToFloatS16(const short *src, size_t size, float *dest);
static void *micpthreadfunc(void *args);
static void *refpthreadfunc(void *args);
static void *aecpthreadfunc(void *args);
static int difftimeval(struct timeval *curr, struct timeval *last)
{
    return (curr->tv_sec - last->tv_sec)*1000000 + (curr->tv_usec - last->tv_usec);
}

static MicThreadParams micparams;
static RefThreadParams refparams;
static AecThreadParams aecparams;

int main(int argc, char **argv)
{
	RingBuffer *pmicring = NULL;
	RingBuffer *prefring = NULL;
	pthread_t micthread;
	pthread_t refthread;
	pthread_t aecthread;
	int status = 0;

	pmicring = WebRtc_CreateBuffer(MIC_RINGBUFF_LEN, sizeof(short));
	if (NULL == pmicring)
	{
		printf("[main] WebRtc_CreateBuffer for pmicring failed\n");
		goto exitproc;
	}
	WebRtc_InitBuffer(pmicring);

	prefring = WebRtc_CreateBuffer(REF_RINGBUFF_LEN, sizeof(short));
	if (NULL == prefring)
	{
		printf("[main] WebRtc_CreateBuffer for prefring failed\n");
		goto exitproc;
	}
	WebRtc_InitBuffer(prefring);

	strcpy(aecparams.sfilepath, "data/aec44.1k_3.pcm");
	aecparams.pmicring = pmicring;
	aecparams.prefring = prefring;
	aecparams.nmicdelaysamples = 40*44100/1000;
	aecparams.nrefdelaysamples = 0;
	aecparams.micframesamples  = MIC_OUT_FRAMESAMPLES;
	aecparams.refframesamples  = REF_OUT_FRAMESAMPLES;
	aecparams.aecframesamples  = AEC_FRAMESAMPLES;
	aecparams.worksamplerate   = 48000;
	aecparams.framesamples     = AEC_FRAMESAMPLES;
	aecparams.pbcontinue = (volatile bool *)(((long)&(aecparams.bcontinue[0]) + 127) & (~127));
	aecparams.pbfinished = (volatile bool *)(((long)&(aecparams.bfinished[0]) + 127) & (~127));
	aecparams.pbcreated  = (volatile bool *)(((long)&(aecparams.bcreated[0]) + 127) & (~127));
	*aecparams.pbcontinue = true;
	*aecparams.pbfinished = false;
	*aecparams.pbcreated = false;
	printf("[main] aecparams addr:%p\n", &aecparams);
	status = pthread_create(&aecthread, NULL, aecpthreadfunc, &aecparams);
	if (status)
	{
		printf("[main] pthread_create for aec failed, return %d\n", status);
		goto exitproc;
	}
	*aecparams.pbcreated = true;

	strcpy(micparams.sfilepath, "data/mic44.1k.pcm");
	micparams.worksamplerate = MIC_SAMPLERATE;
	micparams.framesamples = MIC_IN_FRAMESAMPLES;
	micparams.pbcontinue = (volatile bool *)(((long)&(micparams.bcontinue[0]) + 127) & (~127));
	micparams.pbfinished = (volatile bool *)(((long)&(micparams.bfinished[0]) + 127) & (~127));
	micparams.pbcreated  = (volatile bool *)(((long)&(micparams.bcreated[0]) + 127) & (~127));
	*micparams.pbcontinue = true;
	*micparams.pbfinished = false;
	*micparams.pbcreated = false;
	micparams.pring = pmicring;
	printf("[main] micparams addr:%p\n", &micparams);
	status = pthread_create(&micthread, NULL, micpthreadfunc, &micparams);
	if (status)
	{
		printf("[main] pthread_create for mic failed, return %d\n", status);
		goto exitproc;
	}
	*micparams.pbcreated = true;

	strcpy(refparams.sfilepath, "data/ref44.1k.pcm");
	refparams.worksamplerate = REF_SAMPLERATE;
	refparams.framesamples = REF_IN_FRAMESAMPLES;
	refparams.pbcontinue = (volatile bool *)(((long)&(refparams.bcontinue[0]) + 127) & (~127));
	refparams.pbfinished = (volatile bool *)(((long)&(refparams.bfinished[0]) + 127) & (~127));
	refparams.pbcreated  = (volatile bool *)(((long)&(refparams.bcreated[0]) + 127) & (~127));
	*refparams.pbcontinue = true;
	*refparams.pbfinished = false;
	*refparams.pbcreated = false;
	refparams.pring = prefring;
	printf("[main] refparams addr:%p\n", &refparams);
	status = pthread_create(&refthread, NULL, refpthreadfunc, &refparams);
	if (status)
	{
		printf("[main] pthread_create for ref failed, return %d\n", status);
		goto exitproc;
	}
	*refparams.pbcreated = true;

exitproc:
	while(*refparams.pbcreated && !*refparams.pbfinished)
	{
		usleep(1000);
	}
	while(*micparams.pbcreated && !*micparams.pbfinished)
	{
		usleep(1000);
	}

	// finish read all data for mic and ref, so we could finish aec	
	*aecparams.pbcontinue = false;

	while(*aecparams.pbcreated && !*aecparams.pbfinished)
	{
		usleep(1000);
	}

	pthread_join(refthread,NULL);
	pthread_join(micthread,NULL);
	pthread_join(aecthread,NULL);

	if (prefring)
	{
		WebRtc_FreeBuffer(prefring);
	}
	if (pmicring)
	{
		WebRtc_FreeBuffer(pmicring);
	}

    return 0;
}

void S16ToFloatS16(const short *src, size_t size, float *dest)
{
    for (size_t i = 0; i < size; ++i)
        dest[i] = (float)(src[i]);    
}

static void *micpthreadfunc(void *args)
{
    MicThreadParams *pmicparams = (MicThreadParams *)args;
    short smic[MAX_SAMPLES_10MS];
    FILE *fmic = NULL;
	FILE *fcpy = NULL;
	FILE *fdbg = NULL;
	struct timeval time0 = {0};
    struct timeval time1 = {0};
	struct timeval time2 = {0};
	struct timeval time3 = {0};
    int dispatchcycle = 0;
	int intervaltime = 0;
	int dispatchtime = 0;
	int processtime = 0;
    int sleeptime = 0;
	
	prctl(PR_SET_NAME, "micpthread");

	fdbg = fopen("debug/debug_micpthread.log", "w");
	if (NULL == fdbg)
	{
		printf("[micpthread] fopen debug/debug_micpthread.log failed\n");
	}
	fcpy = fopen("debug/cpy_micpthread.pcm", "wb");
	if (NULL == fcpy)
	{
		printf("[micpthread] fopen debug/cpy_micpthread.log failed\n");
	}

	printf("[micpthread] micparams addr: %p, framesamples: %d\n", args, pmicparams->framesamples);
    if (NULL == pmicparams)
    {
        printf("[micpthread] mic pthread params is null\n");
		*pmicparams->pbfinished = true;
        return NULL;
    }

	fmic = fopen(pmicparams->sfilepath, "rb");
	if (NULL == fmic)
    {
        printf("[micpthread] fopen %s failed\n", pmicparams->sfilepath);
		*pmicparams->pbfinished = true;
		return NULL;
    }
	printf("[micpthread] fopen fmic: %p, path: %s\n", fmic, pmicparams->sfilepath);

	*pmicparams->pbfinished = false;

	dispatchcycle = (pmicparams->framesamples*1000000)/pmicparams->worksamplerate;
	printf("[micpthread] dispatchcycle: %d\n", dispatchcycle);
	gettimeofday(&time2, NULL);
	gettimeofday(&time3, NULL);

    while(*pmicparams->pbcontinue)
    {
		gettimeofday(&time0, NULL);
		intervaltime = difftimeval(&time0, &time3);
		time3 = time0;

        if (fread(smic, sizeof(short), pmicparams->framesamples, fmic) != pmicparams->framesamples)
        {
            printf("[micpthread] finish read all mic data\n");
			*pmicparams->pbcontinue = false;
            break;
        }

		int nGetFrameTimes = 0;
		while (1)
		{
			if (WebRtc_available_write(pmicparams->pring) >= pmicparams->framesamples || !*pmicparams->pbcontinue)
			{
				if (nGetFrameTimes > 0)
				{
					printf("[micpthread] retry %d times to get enough buffer for write\n", nGetFrameTimes);
				}
				
				break;
			}

			nGetFrameTimes++;

			usleep(100);
		}

        WebRtc_WriteBuffer(pmicparams->pring, smic, pmicparams->framesamples);

		if (fcpy)
		{
			fwrite(smic, sizeof(short), pmicparams->framesamples, fcpy);
		}

		gettimeofday(&time1, NULL);

		// time0 - time2 dispatch time
		// time1 - time0 process  time
		dispatchtime = difftimeval(&time0, &time2);
		processtime = difftimeval(&time1, &time0);
		sleeptime = dispatchcycle - (dispatchtime + processtime);
		if (intervaltime > dispatchcycle)
		{
			sleeptime -= intervaltime - dispatchcycle;
		}
		if (fdbg)
		{
			fprintf(fdbg, "[micpthread] dispatch: %d, %d, %d, %d\n", dispatchtime, processtime, sleeptime, intervaltime);
		}
		if (sleeptime > 0) 
		{
			usleep(sleeptime);
		}
		gettimeofday(&time2, NULL);
    }

    if (fmic)
    {
        fclose(fmic);
    }
   
    printf("[micpthread] mic pthread exiting...\n");

	*pmicparams->pbfinished = true;

    return NULL;
}

static void *refpthreadfunc(void *args)
{
    RefThreadParams *prefparams = (RefThreadParams *)args;
    short sref[MAX_SAMPLES_10MS];
    FILE *fref = NULL;
	FILE *fdbg = NULL;
	FILE *fcpy = NULL;
	struct timeval time0 = {0};
    struct timeval time1 = {0};
	struct timeval time2 = {0};
	struct timeval time3 = {0};
    int dispatchcycle = 0;
	int intervaltime = 0;
	int dispatchtime = 0;
	int processtime = 0;
    int sleeptime = 0;

	prctl(PR_SET_NAME, "refpthread");

	fdbg = fopen("debug/debug_refpthread.log", "w");
	if (NULL == fdbg)
	{
		printf("[refpthread] fopen debug/debug_refpthread.log failed\n");
	}
	fcpy = fopen("debug/cpy_refpthread.pcm", "wb");
	if (NULL == fcpy)
	{
		printf("[refpthread] fopen debug/cpy_refpthread.log failed\n");
	}

	printf("[refpthread] refparams addr: %p, framesamples: %d\n", args, prefparams->framesamples);
    if (NULL == prefparams)
    {
        printf("[refpthread] ref pthread params is null\n");
		*prefparams->pbfinished = true;
        return NULL;
    }

	fref = fopen(prefparams->sfilepath, "rb");
	if (NULL == fref)
    {
        printf("[refpthread] fopen %s failed\n", prefparams->sfilepath);
		*prefparams->pbfinished = true;
		return NULL;
    }

	printf("[refpthread] fopen fref: %p, path: %s\n", fref, prefparams->sfilepath);
	
	*prefparams->pbfinished = false;

	dispatchcycle = (prefparams->framesamples*1000000)/prefparams->worksamplerate;
	printf("[refpthread] dispatchcycle: %d\n", dispatchcycle);
	gettimeofday(&time2, NULL);
	gettimeofday(&time3, NULL);
    while(*prefparams->pbcontinue)
    {
		gettimeofday(&time0, NULL);
		intervaltime = difftimeval(&time0, &time3);
		time3 = time0;

        if (fread(sref, sizeof(short), prefparams->framesamples, fref) != prefparams->framesamples)
        {
            printf("[refpthread] finish read all ref data\n");
			*prefparams->pbcontinue = false;
            break;
        }

		int nGetFrameTimes = 0;
		while (1)
		{
			if (WebRtc_available_write(prefparams->pring) >= prefparams->framesamples || !*prefparams->pbcontinue)
			{
				if (nGetFrameTimes > 0)
				{
					printf("[micpthread] retry %d times to get enough buffer for write\n", nGetFrameTimes);
				}
				
				break;
			}

			nGetFrameTimes++;

			usleep(100);
		}

        WebRtc_WriteBuffer(prefparams->pring, sref, prefparams->framesamples);

		if (fcpy)
		{
			fwrite(sref, sizeof(short), prefparams->framesamples, fcpy);
		}

		gettimeofday(&time1, NULL);

		// time0 - time2 dispatch time
		// time1 - time0 process  time
		dispatchtime = difftimeval(&time0, &time2);
		processtime = difftimeval(&time1, &time0);
		sleeptime = dispatchcycle - (dispatchtime + processtime);
		if (intervaltime > dispatchcycle)
		{
			sleeptime -= intervaltime - dispatchcycle;
		}
		if (fdbg)
		{
			fprintf(fdbg, "[refpthread] dispatch: %d, %d, %d, %d\n", dispatchtime, processtime, sleeptime, intervaltime);
		}
		if (sleeptime > 0) 
		{
			usleep(sleeptime);
		}
		gettimeofday(&time2, NULL);
    }

    if (fref)
    {
        fclose(fref);
    }
  
    printf("[refpthread] ref pthread exiting...\n");

	*prefparams->pbfinished = true;

    return NULL;
}

static void *aecpthreadfunc(void *args)
{
    PushSincResampler *pmicresampler = NULL;
    PushSincResampler *prefresampler = NULL;
    PushSincResampler *paecresampler = NULL;
    AudioBuffer  *pmicbuffer = NULL;
    AudioBuffer  *prefbuffer = NULL;
    AecThreadParams *paecparams = (AecThreadParams *)args;
    short smic[MAX_SAMPLES_10MS];
	short sref[MAX_SAMPLES_10MS];
    short saec[MAX_SAMPLES_10MS];
    FILE *faec = NULL;
	FILE *fdbg = NULL;
	FILE *fmic0 = NULL;
	FILE *fmic1 = NULL;
	FILE *fref0 = NULL;
	FILE *fref1 = NULL;
	void *haec = NULL;
    
    bool  bmicresample = false;
    bool  brefresample = false;
    bool  baecresample = false;
	bool  bmicdelayed  = false;
    bool  brefdelayed  = false;
	int   subframesamples = 0;
	int   framesamples = 0;
    int   numbands = 0;
	int   status = 0;

	struct timeval time0 = {0};
    struct timeval time1 = {0};
	struct timeval time2 = {0};
	struct timeval time3 = {0};
    int dispatchcycle = 0;
	int intervaltime = 0;
	int dispatchtime = 0;
	int processtime = 0;
    int sleeptime = 0;

	prctl(PR_SET_NAME, "aecpthread");

	fdbg = fopen("debug/debug_aecpthread.log", "w");
	if (NULL == fdbg)
	{
		printf("[aecpthread] fopen debug/debug_aecpthread.log failed\n");
	}
	fmic0 = fopen("debug/mic0_aecpthread.pcm", "wb");
	if (NULL == fmic0)
	{
		printf("[aecpthread] fopen debug/mic0_aecpthread.pcm failed\n");
	}
	fmic1 = fopen("debug/mic1_aecpthread.pcm", "wb");
	if (NULL == fmic1)
	{
		printf("[aecpthread] fopen debug/mic1_aecpthread.pcm failed\n");
	}
	fref0 = fopen("debug/ref0_aecpthread.pcm", "wb");
	if (NULL == fref0)
	{
		printf("[aecpthread] fopen debug/ref0_aecpthread.pcm failed\n");
	}
	fref1 = fopen("debug/ref1_aecpthread.pcm", "wb");
	if (NULL == fref1)
	{
		printf("[aecpthread] fopen debug/ref1_aecpthread.pcm failed\n");
	}

	printf("[aecpthread] aecparams addr: %p, %d, %d, %d\n", args, paecparams->micframesamples, paecparams->refframesamples, paecparams->aecframesamples);
    if (NULL == paecparams)
    {
        printf("[aecpthread] aec pthread params is null\n");
		*paecparams->pbfinished = true;
        return NULL;
    }

	StreamConfig streamcfg(paecparams->worksamplerate, 1, false);
		
	framesamples = (paecparams->worksamplerate*10)/1000;

	pmicbuffer = new AudioBuffer(framesamples, 1, framesamples, 1, framesamples);
    if (NULL == pmicbuffer)
    {
        printf("[aecpthread] new AudioBuffer(%d, %d, %d, %d, %d) for pmicbuffer failed\n",
                framesamples, 1, framesamples, 1, framesamples);
        goto exitproc;
    }
    prefbuffer = new AudioBuffer(framesamples, 1, framesamples, 1, framesamples);
    if (NULL == prefbuffer)
    {
        printf("[aecpthread] new AudioBuffer(%d, %d, %d, %d, %d) for prefbuffer failed\n",
                framesamples, 1, framesamples, 1, framesamples);
        goto exitproc;
    }
     
    numbands = pmicbuffer->num_bands();
    subframesamples = pmicbuffer->num_frames_per_band();
    printf("[aecpthread] frame len: %d, bands num: %d, subband frame len: %d\n",
                framesamples, numbands, subframesamples);	

    haec = WebRtcAec_Create();
    if (NULL == haec)
    {
        printf("[aecpthread] create aec failed\n");
        goto exitproc;
    }

    status = WebRtcAec_Init(haec, paecparams->worksamplerate, paecparams->worksamplerate);
    if (status)
    {
        printf("[aecpthread] init aec failed, return %d\n", status);
        goto exitproc;
    }

    if (paecparams->micframesamples != framesamples)
    {
		bmicresample = true;
        pmicresampler = new PushSincResampler(paecparams->micframesamples, framesamples);
        if (NULL == pmicresampler)
        {
            printf("[aecpthread] new PushSincResampler(%d, %d) for pmicresampler failed\n", paecparams->micframesamples, framesamples);
            goto exitproc;
        }        
    }

    if (paecparams->refframesamples != framesamples)
    {
		brefresample = true;
        prefresampler = new PushSincResampler(paecparams->refframesamples, framesamples);
        if (NULL == prefresampler)
        {
            printf("[aecpthread] new PushSincResampler(%d, %d) for prefresampler failed\n", paecparams->refframesamples, framesamples);
            goto exitproc;
        }        
    }

    if (paecparams->aecframesamples != framesamples)
    {
		baecresample = true;
        paecresampler = new PushSincResampler(framesamples, paecparams->aecframesamples);
        if (NULL == paecresampler)
        {
            printf("[aecpthread] new PushSincResampler(%d, %d) for paecresampler failed\n", framesamples, paecparams->aecframesamples);
            goto exitproc;
        }        
    }

	faec = fopen(paecparams->sfilepath, "wb");
	if (NULL == faec)
    {
        printf("[aecpthread] fopen %s failed\n", paecparams->sfilepath);
		goto exitproc;
    }

	*paecparams->pbfinished = false;

	dispatchcycle = (paecparams->framesamples*5*1000)/paecparams->aecframesamples;
	printf("[aecpthread] dispatchcycle: %d\n", dispatchcycle);
	gettimeofday(&time2, NULL);
	gettimeofday(&time3, NULL);
    while(*paecparams->pbcontinue)
    {
		float dref[MAX_SAMPLES_10MS];
		float dmic[MAX_SAMPLES_10MS];
		float daec[MAX_SAMPLES_10MS];

		gettimeofday(&time0, NULL);
		intervaltime = difftimeval(&time0, &time3);
		time3 = time0;

		if (!bmicdelayed) 
		{
			if (paecparams->nmicdelaysamples <= WebRtc_available_read(paecparams->pmicring))
			{
				bmicdelayed = true;
			}
		}
	    if (bmicdelayed)
	    {
			while (1)
			{
				if (WebRtc_available_read(paecparams->pmicring) >= paecparams->micframesamples || !*paecparams->pbcontinue)
				{
					break;
				}

				usleep(100);
			}
	    	WebRtc_ReadBuffer(paecparams->pmicring, NULL, smic, paecparams->micframesamples);
			if (fmic0)
			{
				fwrite(smic, sizeof(short), paecparams->micframesamples, fmic0);
			}
			if (bmicresample)
			{
				pmicresampler->Resample(smic, paecparams->micframesamples, smic, framesamples);
			}
	    }

		if (!brefdelayed) 
		{
			if (paecparams->nrefdelaysamples <= WebRtc_available_read(paecparams->prefring))
			{
				brefdelayed = true;
			}
		}
        if (brefdelayed)
        {
			int nGetFrameTimes = 0;
			while (1)
			{
				if (WebRtc_available_read(paecparams->prefring) >= paecparams->refframesamples || !*paecparams->pbcontinue)
				{
					break;
				}

				usleep(100);
			}
        	WebRtc_ReadBuffer(paecparams->prefring, NULL, sref, paecparams->refframesamples);
			if (fref0)
			{
				fwrite(sref, sizeof(short), paecparams->refframesamples, fref0);
			}
			if (brefresample)
			{
				prefresampler->Resample(sref, paecparams->refframesamples, sref, framesamples);
			}
        }
#if 1
		printf("[aecpthread] mic buffered:%d, ref buffered:%d, delayed:%d %d\n", 
			WebRtc_available_read(paecparams->pmicring), 
			WebRtc_available_read(paecparams->prefring),
			bmicdelayed, brefdelayed);
#endif
		if (!bmicdelayed || !brefdelayed)
		{
			memset(smic, 0, sizeof(short)*framesamples);
			memset(sref, 0, sizeof(short)*framesamples);
		}

		if (fmic1)
		{
			fwrite(smic, sizeof(short), framesamples, fmic1);
		}
		if (fref1)
		{
			fwrite(sref, sizeof(short), framesamples, fref1);
		}

        S16ToFloat(smic, framesamples, dmic);
        float* const pafmic = (float* const)(&dmic[0]);
        const float* const* ppafmic = &pafmic;
	    pmicbuffer->CopyFrom(ppafmic, streamcfg);
        pmicbuffer->SplitIntoFrequencyBands();

        S16ToFloat(sref, framesamples, dref);
        float* const pafref = (float* const)(&dref[0]);
        const float* const* ppafref = &pafref;
	    prefbuffer->CopyFrom(ppafref, streamcfg);        
        prefbuffer->SplitIntoFrequencyBands();

        status = WebRtcAec_BufferFarend(haec, prefbuffer->split_bands_const_f(0)[0], subframesamples);
        if (status) 
        {
            printf("[aecpthread] WebRtcAec_BufferFarend failed, return %d\n", status);
        }

        status = WebRtcAec_Process(haec, pmicbuffer->split_bands_const_f(0), numbands, pmicbuffer->split_bands_f(0), subframesamples, 10, 0);
        if (status) 
        {
            printf("[aecpthread] WebRtcAec_Process failed, return %d\n", status);
        }

        pmicbuffer->MergeFrequencyBands();

        float* const  pafaec = (float* const)(&daec[0]);
        float* const* ppafaec = &pafaec;
	    pmicbuffer->CopyTo(streamcfg, ppafaec);
        FloatToS16(daec, framesamples, saec);

        if (baecresample)
        {
			paecresampler->Resample(saec, framesamples, saec, paecparams->aecframesamples);
        }

        fwrite(saec, sizeof(short), paecparams->aecframesamples, faec);

		gettimeofday(&time1, NULL);

		// time0 - time2 dispatch time
		// time1 - time0 process  time
		dispatchtime = difftimeval(&time0, &time2);
		processtime = difftimeval(&time1, &time0);
		sleeptime = dispatchcycle - (dispatchtime + processtime);
		if (intervaltime > dispatchcycle)
		{
			sleeptime -= intervaltime - dispatchcycle;
		}
		if (fdbg)
		{
			fprintf(fdbg, "[aecpthread] dispatch: %d, %d, %d, %d\n", dispatchtime, processtime, sleeptime, intervaltime);
		}
		if (sleeptime > 0) 
		{
			usleep(sleeptime);
		}
		gettimeofday(&time2, NULL);
    }

exitproc:
    if (faec)
    {
        fclose(faec);
    }
	if (paecresampler)
	{
		delete paecresampler;
	}
	if (prefresampler)
	{
		delete prefresampler;
	}
	if (pmicresampler)
	{
		delete pmicresampler;
	}
    if (haec)
    {
        WebRtcAec_Free(haec);
    }
    if (prefbuffer)
    {
        delete prefbuffer;
    }
    if (pmicbuffer)
    {
        delete pmicbuffer;
    }
    
    printf("[aecpthread] aec pthread exiting...\n");

	*paecparams->pbfinished = true;

    return NULL;    
}
