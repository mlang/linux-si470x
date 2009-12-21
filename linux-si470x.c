#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include <linux/videodev2.h>

#define DEFAULT_RADIO_DEVICE "/dev/radio0"
#define DEFAULT_AUDIO_DEVICE "hw:Music"
#define MAX_VOLUME 100
#define MAX_CHANNELS 2

#define HAVE_JACK 1

static int verbose = 0;

static int frequencyDivider;
static float minFrequency, currentFrequency, maxFrequency;

static void
setTunerVolume(int fd, unsigned int volume) {
  struct v4l2_control control = {
    .id = V4L2_CID_AUDIO_MUTE,
    .value = (volume==0? 1 : 0)
  };
  
  if (ioctl(fd, VIDIOC_S_CTRL, &control) != -1) {
    struct v4l2_queryctrl queryctrl = {
      .id = V4L2_CID_AUDIO_VOLUME
    };

    if (ioctl(fd, VIDIOC_QUERYCTRL, &queryctrl) != -1) {
      if (volume > MAX_VOLUME) volume = MAX_VOLUME;
      memset(&control, 0, sizeof(control));
      control.id = V4L2_CID_AUDIO_VOLUME;
      control.value =
	queryctrl.minimum
	+ volume * (queryctrl.maximum - queryctrl.minimum) / MAX_VOLUME;
      if (ioctl(fd, VIDIOC_S_CTRL, &control) != -1) {
	return;
      } else {
	perror("ioctl VIDIOC_S_CTRL");
      }
    } else {
      perror("ioctl VIDIOC_QUERYCTRL");
    }
  } else {
    perror("ioctl VIDIOC_S_CTRL");
  }
}

static void
setTunerFrequency(int fd, struct v4l2_tuner *tuner, float newFrequency) {
  if (newFrequency < maxFrequency && newFrequency > minFrequency) {
    struct v4l2_frequency freq;

    memset(&freq, 0, sizeof(freq));
    freq.tuner = 0;
    freq.type = V4L2_TUNER_RADIO;
    freq.frequency = newFrequency * frequencyDivider;
    if (ioctl(fd, VIDIOC_S_FREQUENCY, &freq) != -1) {
      return;
    } else {
      perror("ioctl VIDIOC_S_FREQUENCY");
    }
  } else {
    printf("%.2f is not in range (%.2f - %.2f)\n",
	   newFrequency, minFrequency, maxFrequency);
  }
}

static float
getTunerFrequency(int fd) {
  struct v4l2_frequency freq;

  memset(&freq, 0, sizeof(freq));
  freq.tuner = 0;
  freq.type = V4L2_TUNER_RADIO;
  if (ioctl(fd, VIDIOC_G_FREQUENCY, &freq) != -1) {
    return freq.frequency / (float)frequencyDivider;
  } else {
    perror("ioctl VIDIOC_G_FREQUENCY");
  }
  return 0;
}

static float
seekTunerFrequency(int fd, int up) {
  struct v4l2_hw_freq_seek freqSeek;

  memset(&freqSeek, 0, sizeof(freqSeek));
  freqSeek.tuner = 0;
  freqSeek.type = V4L2_TUNER_RADIO;
  freqSeek.seek_upward = up? 1 : 0;
  freqSeek.wrap_around = 1;
  if (ioctl(fd, VIDIOC_S_HW_FREQ_SEEK, &freqSeek) != -1) {
    return getTunerFrequency(fd);
  } else {
    perror("ioctl VIDIOC_S_HW_FREQ_SEEK");
  }

  return 0;
}

/* Radio (Broadcast) Data System */

static const char *programTypes[30] = {
  "News", "Current affairs", "Information", "Sport",
  "Education", "Drama", "Culture", "Science", "Varied", "Pop music",
  "Rock music", "Easy listening", "Light classical", "Serious classical",
  "Other music", "Weather", "Finance", "Children's programmes",
  "Social affairs", "Religion" "Phone-in", "Travel", "Leisure", "Jazz music",
  "Country music", "National music", "Oldies music", "Folk music",
  "Documentary", "Alarm test", "Alarm"
};

typedef struct {
  uint16_t id;
  float freq;
  char name[8+1];
  unsigned char tp;
  unsigned char ta;

  char type;
} ProgramData;

static ProgramData *programs = NULL;
static int programCount = 0;

static ProgramData *
getProgram(uint16_t id) {
  for (int i = 0; i < programCount; i++) {
    ProgramData *pd = &programs[i];
    if (pd->id == id) return pd;
  }

  programs = realloc(programs, ++programCount*sizeof(*programs));
  {
    ProgramData *pd = &programs[programCount - 1];
    memset(pd, 0, sizeof(*pd));
    pd->id = id;
    return pd;
  }
}

/* For restoring cannonical mode upon exit */

static struct termios savedTerminalSettings;

static void
parent_sigterm_handler(int signal) {
  tcsetattr(0, TCSAFLUSH, &savedTerminalSettings);
  kill(0, signal);
}

static void
disableCannonicalMode() {
  struct termios termios_p;
  const int fd = 0;
  tcgetattr(fd, &termios_p);
  savedTerminalSettings = termios_p;
  termios_p.c_lflag &= ~(ECHO|ICANON);
  tcsetattr(fd, TCSAFLUSH, &termios_p);
  signal(SIGTERM, parent_sigterm_handler);
  signal(SIGINT, parent_sigterm_handler);
}

static int
EONAF_handleFrequencyPair(ProgramData *this, ProgramData *other, float f1, float f2) {
  if (this->freq >= minFrequency) {
    if (f1 >= (this->freq-.04) && f1 <= (this->freq+.04)) {
      other->freq = f2;
      return 1;
    }
  }
  return 0;
}

static void
nextProgram(int fd, struct v4l2_tuner *tuner) {
  int i;

  if (programCount <= 1) return;
  for (i = 0; i < programCount; i++) {
    if (currentFrequency >= programs[i].freq-.09
     && currentFrequency <= programs[i].freq+.09) {
      int next = (i == programCount - 1)? 0 : i + 1;
      while (next != i) {
        float freq = programs[next].freq;
        if (freq >= minFrequency) {
          if (programs[next].name[0])
            printf("Switching to %s (%.2f)\n", programs[next].name, freq);
          setTunerFrequency(fd, tuner, freq);
          currentFrequency = freq;
          return;
        }
        if (next == programCount - 1) next = 0; else next += 1;
      }
      printf("No other stations known\n");
      return;
    }
  }
}

static inline void
decodeRds(int fd, struct v4l2_tuner *tuner) {
  typedef enum {
    TYPE_0A = 0, TYPE_0B, /* Basic tuning and switching information */
    TYPE_1A, TYPE_1B,     /* Program-item number and slow labeling codes */
    TYPE_2A, TYPE_2B,     /* Radiotext */
    TYPE_3A,              /* Applications Identification for Open Data */
    TYPE_3B,              /* Open data application */
    TYPE_4A,              /* Clock-time and date */
    TYPE_4B,              /* Open data application */
    TYPE_5A, TYPE_5B,     /* Transparent data channels or ODA */
    TYPE_6A, TYPE_6B,     /* In house applications or ODA */
    TYPE_7A,              /* Radio paging or ODA */
    TYPE_7B,              /* Open data application */
    TYPE_8A, TYPE_8B,     /* Traffic Message Channel or ODA */
    TYPE_9A, TYPE_9B,     /* Emergency warning systems or ODA */
    TYPE_10A,             /* Program Type Name */
    TYPE_10B,             /* Open data */
    TYPE_11A, TYPE_11B,   /* Open data application */
    TYPE_12A, TYPE_12B,   /* Open data application */
    TYPE_13A,             /* Enhanced Radio paging or ODA */
    TYPE_13B,             /* Open data application */
    TYPE_14A, TYPE_14B,   /* Enhanced Other Networks information */
    TYPE_15A,
    TYPE_15B              /* Fast tuning and switching information */
  } RDS_GroupType;

  struct rds_data {
    uint8_t lsb;
    uint8_t msb;
    uint8_t block;
  } __attribute__((packed)) rdsData;
  ssize_t count;
  int blockCount = 0;
  int errorCount = 0;

  RDS_GroupType groupType;
  unsigned char groupData[2*4];
  unsigned char lastGroupData[2*4];

  ProgramData *thisProgram = NULL;

  char programName[8+1] = {0};
  char *lastProgramName = NULL;

  char stereoKnown = 0;
  char isStereo;
  char ta = 0;

  int freqCounter = 0;

  char radioText[4*0X10 + 1] = {0};
  char radioTextabFlag = 0;

  memset(radioText, ' ', 4*0X10);
  radioText[4*0X10] = 0;

  if (isatty(STDIN_FILENO)) {
    disableCannonicalMode();
  }
  while (1) {
    struct pollfd fds[] = {
      { .fd = fd, .events = POLLIN },
      { .fd = STDIN_FILENO, .events = POLLIN }
    };
    const int fdCount = sizeof(fds)/sizeof(*fds);
    int pollval = poll(fds, fdCount, 1000);

    if (pollval == 0) {
      if (verbose) printf("No RDS data\n");
      continue;
    } else if (pollval == -1) {
      perror("poll");
      break;
    }
    
    for (int i = 0; i < fdCount; i++) {
      if (fds[i].revents & fds[i].events) {
        if (fds[i].fd == fd) {
          count = read(fd, &rdsData, sizeof(rdsData));
          if (count == 0) break;
          if (count != sizeof(rdsData)) {
            printf("ERR: Incomplete RDS block, count was %d\n", (int)count);
            continue;
          }
        } else if (fds[i].fd == STDIN_FILENO) {
          uint8_t c;
          count = read(STDIN_FILENO, &c, 1);
          if (count == 1) {
            switch (c) {
            case 'n': nextProgram(fd, tuner); break;
            case '+': {
              currentFrequency += .05;
              if (currentFrequency > maxFrequency)
                currentFrequency = minFrequency;
              setTunerFrequency(fd, tuner, currentFrequency);
              printf("Frequency tuned to %.2f\n", currentFrequency);
              break;
            }
            case '-': {
              currentFrequency -= .05;
              if (currentFrequency < minFrequency)
                currentFrequency = maxFrequency;
              setTunerFrequency(fd, tuner, currentFrequency);
              printf("Frequency tuned to %.2f\n", currentFrequency);
              break;
            }
            default:
              printf("Keyboard: %d (%X)\n", c, c);
            }
            continue;
          } else if (count == 0) {
            break;
          }
        }
      }
    }

    int blockNumber = rdsData.block & 0X07;
    int error = (rdsData.block&0X80)==0X80;

    blockCount += 1;

    if (error) {
      errorCount += 1;
      if (verbose) printf("%d errors in %d blocks so far\n",
			  errorCount, blockCount);
      continue;
    }

    if (blockNumber == 0) {
      thisProgram = getProgram(rdsData.msb<<8|rdsData.lsb);
      thisProgram->freq = currentFrequency;
    }
    if (blockNumber == 1) {
      int ptyCode = ((rdsData.msb << 3) & 0X18) | ((rdsData.lsb >> 5) & 0X07);

      if (thisProgram != NULL && ptyCode != 0) {
	if (thisProgram->type != ptyCode) {
          thisProgram->type = ptyCode;
          if (ptyCode > 0) printf("Program type: %s\n",
                                  programTypes[ptyCode-1]);
        }
      }
      groupType = (RDS_GroupType)rdsData.msb>>3;
    }
    groupData[2*blockNumber] = rdsData.msb;
    groupData[2*blockNumber+1] = rdsData.lsb;
    if (blockNumber == 3) {
      if (memcmp(groupData, lastGroupData, sizeof(groupData)) == 0)
        continue;
      switch (groupType) {
      case TYPE_0A: {
        char TP = (groupData[2] & 0x04) == 0X04;
	char isTrafficAnnouncement = (groupData[3] & 0x10) == 0X10;
	char isMusic = (groupData[3] & 0x08) == 0X08;
	int index = (groupData[3] & 0x03) << 1;

	if (TP && isTrafficAnnouncement != ta) {
	  ta = isTrafficAnnouncement;
	  printf("Traffic announcement %s\n", ta? "on" : "off");
	}
	programName[index] = groupData[6];
	programName[index+1] = groupData[7];
	if (strlen(programName) && index == 6) {
	  if (lastProgramName == NULL
	   || strcmp(programName, lastProgramName) != 0) {
	    printf("Program: %s\n", programName);
	    if (lastProgramName != NULL) free(lastProgramName);
	    lastProgramName = strdup(programName);
	  }
	  programName[0] = 0;
	}
	switch (groupData[3]&0X03) {
	case 3:
	  if (!stereoKnown) {
	    isStereo = ((groupData[3]&0X04)==0X04);
	    stereoKnown = 1;
	    printf("Program is %s\n", isStereo? "stereo" : "mono");
	  }
	  if (isStereo != ((groupData[3]&0X04)==0X04)) {
	    isStereo = ((groupData[3]&0X04)==0X04);
	    printf("Program is %s\n", isStereo? "stereo" : "mono");
	  }
	  break;
	}

	if ((groupData[4]>=224)&&(groupData[4]<=249)) {
	  freqCounter = groupData[4] - 224;
	  if (freqCounter) {
	    if ((groupData[5]>=1) && ((groupData[5]<=204))) {
	      float f = ((100*(groupData[5]-1))+87600)/1000.0;
	      freqCounter -= 1;
	    }
	  }
	} else if (freqCounter > 0) {
	  float f1 = ((100*(groupData[4]-1))+87600)/1000.0;
	  float f2 = ((100*(groupData[5]-1))+87600)/1000.0;
          freqCounter -= 2;
	  if (freqCounter == 0) {
	    //printf("AFlist done\n");
	  }
	}
	break;
      }
      case TYPE_2A: {
	int index = groupData[3]&0X0F;
	int newabFlag = (groupData[3]&0X10)==0X10;
	if (newabFlag != radioTextabFlag) {
	  radioTextabFlag = newabFlag;

	  {
	    int i = 63;
	    while (i >= 0) {
	      if (radioText[i] == 0) {
		i -= 1;
		continue;
	      }
	      if ((radioText[i] == ' ') || (radioText[i] == '\r')) {
		radioText[i] = 0;
		i -= 1;
		continue;
	      }
	      break;
	    }
	  }
	  if (strlen(radioText) > 0) {
	    printf("Text: %s\n", radioText);
	  }
	  
	  memset(radioText, ' ', 4*0X10);
	}
	for (int i = 0; i < 4; i++) {
	  radioText[4*index + i] = groupData[4+i];
	}
              }
              break;
      case TYPE_4A: {
	const int monthDays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
	const int julianDate = ((groupData[3]&0X03)<<15)
                             | (groupData[4]<<7)
	                     | (groupData[5]>>1);
	int year = (int)(((double)julianDate - 15078.2)/365.25);
	int month = (int)(((julianDate - 14956.1)-(int)(year*365.25))/30.6001);
	int day = julianDate-14956-(int)(year*365.25)-(int)(month*30.6001);
	int utcHour = ((groupData[5]&0X01)<<4)
	            | ((groupData[6]&0XF0)>>4);
	int utcMinute = ((groupData[6]&0X0F)<<2)
	              | ((groupData[7]&0XC0)>>6);
	int utcOffset = groupData[7]&0X1F;
	int K = ((month == 14)||(month == 15))? 1 : 0;

	if (groupData[7]&0X20) utcOffset = -utcOffset;

	year = year + K + 1900;
	month = month - 1 - (K*12);

	{ /* Calculate local time */
	  int localHour = utcHour; 
	  int localMinute  = utcMinute + (utcOffset*30);

	  while (localMinute < 0) { localMinute += 60, localHour -= 1; }
	  while (localMinute >= 60) { localMinute -= 60, localHour += 1; }
	  if (localHour < 0) {
	    localHour += 24, day -= 1;
	    if (day < 1) {
	      month -= 1;
	      if (month < 1) {
		month = 12;
		year -= 1;
	      }
	      day = monthDays[month-1];
	      if (((year % 4) == 0) && (month == 2)) day = 29;
	    }
	  }
	  if (localHour >= 24) {
	    localHour -= 24, day += 1;
	    int maxDay = (((year%4)==0)&&(month==2))? 29 : monthDays[month-1];
	    if (day > maxDay) {
	      month += 1;
	      if (month > 12) {
		month = 1, year += 1;
	      }
	    }
	  }

	  printf("Date: %04d-%02d-%02d %02d:%02d (%c%02d:%02d)\n",
		 year, month, day, localHour, localMinute,
		 (utcOffset > 0)? '+' : '-',
		 utcOffset*30 / 60, (utcOffset*30) % 60);
	}
	break;
      }
      case TYPE_8A: {
	typedef enum {TMC_GROUP=0, TMC_SINGLE, TMC_SYSTEM, TMC_TUNING} TMC_Type;
	TMC_Type tmctype = (groupData[3]&0X18)>>3;
	int CI = groupData[3]&0X07;
        int extent = (groupData[4]&0X38)>>3;
	int event = ((groupData[4]&0X07)<<8)|groupData[5];
        uint16_t location = groupData[6]<<8|groupData[7];
	switch (tmctype) {
	case TMC_SINGLE: {
          int duration = CI;
          char *durStr = NULL;
          switch (duration) {
          case 0: durStr = "unknown"; break;
          case 1: durStr = "15 minutes"; break;
          case 2: durStr = "30 minutes"; break;
          case 3: durStr = "1 hour"; break;
          case 4: durStr = "2 hours"; break;
          case 5: durStr = "3 hour"; break;
          case 6: durStr = "4 hour"; break;
          case 7: durStr = "rest of the day"; break;
          }
	  printf("TMC(single): evt=%X, loc=%X, extent=%X, dur=%s\n",
		 event, location, extent, durStr);
	  break;
	}
	default:
	  if (verbose) printf("TMC: Type=%X, CI=%X, event=%X, loc=%X\n",
		 tmctype, CI, event, location);
	}
	break;
      }
      case TYPE_14A: {
	int TPON = (groupData[3]&0X10)==0X10;
	int variantType = groupData[3]&0X0F;
	int info = (groupData[4]<<8)|(groupData[5]);
	int PION = (groupData[6]<<8)|(groupData[7]);
	ProgramData *otherProgram = getProgram(PION);
        switch (variantType) {
	case 0:
	case 1:
	case 2:
	case 3: {
	  otherProgram->name[2*variantType] = groupData[4];
	  otherProgram->name[2*variantType+1] = groupData[5];
	  break;
	}
        case 5: {
          uint8_t lsb = groupData[5];
          uint8_t msb = groupData[4];
          if (thisProgram != NULL
           && EONAF_handleFrequencyPair(thisProgram, otherProgram,
                                    ((100*(msb-1))+87600)/1000.0,
                                    ((100*(lsb-1))+87600)/1000.0)) {
            if (verbose && otherProgram->name && otherProgram->name[0])
              printf("%s is on %.2fMHz\n", otherProgram->name, otherProgram->freq);
          }
	  break;
        }
        case 0XD: {
          int TAON = groupData[5]&0X01;
          if (TPON && TAON) {
	    if (TAON != otherProgram->ta) {
	      if (otherProgram->name && otherProgram->name[0])
		printf("Traffic Announcement on %s is %s\n",
		       otherProgram->name, TAON? "on" : "off");
	      else
		printf("Traffic Announcement on %X is %s\n",
		       PION, TAON? "on" : "off");
	      otherProgram->ta = TAON;
	    }
          }
          break;
        }
        default:
          if (verbose) printf("EON: TPON=%d, v=%X, info=%X, PION=%X\n",
				  TPON, variantType, info, PION);

	break;
        }
      }
      default:
        if (verbose > 1) {
          printf("Group(%X): %02X%02X-%02X%02X-%02X%02X-%02X%02X\n",
                 groupType,
                 groupData[0], groupData[1], groupData[2], groupData[3],
	         groupData[4], groupData[5], groupData[6], groupData[7]);
        }
      }
      memcpy(lastGroupData, groupData, sizeof(groupData));
      memset(groupData, 0, sizeof(groupData));
    }
  }

  if (lastProgramName != NULL) free(lastProgramName);
  tcsetattr(0, TCSAFLUSH, &savedTerminalSettings);
}

/* Audio I/O */

#include <alsa/asoundlib.h>
#include <samplerate.h>

static snd_pcm_t *alsa_handle;

static unsigned int inputSampleRate = 96000;
static char num_channels = 2;
static unsigned int period_size = 2048, num_periods = 4; /* 85ms */

static unsigned int resample_quality = 3;

#ifdef HAVE_JACK
#include <alloca.h>
#include <math.h>

#include <jack/jack.h>

static jack_client_t *jackClient;
static jack_port_t *jackPorts[MAX_CHANNELS];
static SRC_STATE *srcs[MAX_CHANNELS];

static int jackSampleRate, jackBufferSize;

static int quit = 0;
static double resample_mean = 1.0;
static double static_resample_factor = 1.0;

static double *offset_array;
static double *window_array;
static int offset_differential_index = 0;
static double offset_integral = 0;

static int target_delay = 0; /* the delay which the program should try to approach. */
static int max_diff = 0;     /* the diff value, when a hard readpointer skip should occur */
static int catch_factor = 100000, catch_factor2 = 10000;
static double pclamp = 15.0;
static double controlquant = 10000.0;
static const int smooth_size = 512;

// Debug stuff:

volatile float output_resampling_factor = 1.0;
volatile int output_new_delay = 0;
volatile float output_offset = 0.0;
volatile float output_integral = 0.0;
volatile float output_diff = 0.0;

typedef struct {
  snd_pcm_format_t format_id;
  size_t sample_size;
  void (*soundcard_to_jack) (jack_default_audio_sample_t *dst, char *src,
			     unsigned long nsamples, unsigned long src_skip);
} alsa_format_t;

#define SAMPLE_16BIT_SCALING  32767.0f

static void
sample_move_dS_s16(jack_default_audio_sample_t *dst, char *src,
		   unsigned long nsamples, unsigned long src_skip) 
{
  while (nsamples--) {
    *dst = (*((short *)src)) / SAMPLE_16BIT_SCALING;
    dst += 1, src += src_skip;
  }
}	

static alsa_format_t formats[] = {
  { SND_PCM_FORMAT_S16, 2, sample_move_dS_s16 }
};
#define NUMFORMATS (sizeof(formats)/sizeof(formats[0]))
static int format = 0;
static int
set_hwformat(snd_pcm_t *handle, snd_pcm_hw_params_t *params) {
  int err;

  for (int i=0; i<NUMFORMATS; i++) {
    err = snd_pcm_hw_params_set_format(handle, params, formats[i].format_id);
    if (err == 0) {
      format = i;
      break;
    }
  }

  return err;
}

static int
xrun_recovery(snd_pcm_t *handle, int err) {
  if (err == -EPIPE) { /* under-run */
    err = snd_pcm_prepare(handle);
    if (err < 0)
      printf("Can't recovery from underrun, prepare failed: %s\n",
	     snd_strerror(err));
  } else if (err == -EAGAIN) {
    while ((err = snd_pcm_resume(handle)) == -EAGAIN)
      usleep(100);	/* wait until the suspend flag is released */
    if (err < 0) {
      err = snd_pcm_prepare(handle);
      if (err < 0)
	printf("Can't recovery from suspend, prepare failed: %s\n",
	       snd_strerror(err));
    }
  }
  return err;
}

static inline int
set_hwparams(snd_pcm_t *handle, snd_pcm_hw_params_t *params,
             snd_pcm_access_t access, unsigned int rate, unsigned int channels,
             unsigned int period, unsigned int nperiods) {
  int err, dir=0;
  unsigned int buffer_time;
  unsigned int period_time;
  unsigned int rrate;

  /* choose all parameters */
  if ((err = snd_pcm_hw_params_any(handle, params)) == 0) {
    if ((err = snd_pcm_hw_params_set_access(handle, params, access)) == 0) {
      if ((err = set_hwformat(handle, params)) == 0) {
	unsigned int rchannels = channels;
	if ((err = snd_pcm_hw_params_set_channels_near(handle, params,
						       &rchannels)) == 0) {
	  if (rchannels != channels) {
	    printf("WARNING: channel count does not match (requested %d got %d)\n",
		   channels, rchannels);
	    num_channels = rchannels;
	  }
	  rrate = rate;
	  if ((err = snd_pcm_hw_params_set_rate_near(handle, params,
						     &rrate, NULL)) >= 0) {
	    if (rrate != rate) {
	      printf("WARNING: Rate doesn't match (requested %iHz, get %iHz)\n", rate,
		     rrate);
	      inputSampleRate = rrate;
	    }
	    buffer_time = 1000000*(uint64_t)period*nperiods/rrate;
            printf("buffer_time = %d\n", buffer_time);
	    if ((err = snd_pcm_hw_params_set_buffer_time_near(handle, params,
							      &buffer_time,
							      &dir)) >= 0) {
              snd_pcm_uframes_t real_buffer_size;
	      if ((err = snd_pcm_hw_params_get_buffer_size(params,
							   &real_buffer_size))
		  >= 0) {
                printf("Buffer size: %d\n", (int)real_buffer_size);
		if (real_buffer_size != nperiods * period) {
		  printf("WARNING: buffer size does not match: "
			 "requested %d, got %d\n",
			 nperiods * period, (int) real_buffer_size);
		}
		/* set the period time */
		printf("period_time = %d\n", period_time = 1000000U*(uint64_t)period/rrate);
		if ((err = snd_pcm_hw_params_set_period_time_near(handle,
								  params,
								  &period_time,
								  &dir))
		    == 0) {
                  snd_pcm_uframes_t real_period_size;
		  if ((err = snd_pcm_hw_params_get_period_size(params,
							       &real_period_size, NULL))
		      == 0) {
                    printf("Period size: %d\n", (int)real_period_size);
		    if (real_period_size != period) {
		      printf("WARNING: period size does not match: "
			     "requested %i, got %i\n",
			     period, (int)real_period_size);
		    }

		    /* write the parameters to device */
		    if ((err = snd_pcm_hw_params(handle, params)) == 0) {
                      if (verbose)
                        printf("Input buffer time: %.1fms\n",
                               1000.0/(rrate/(float)real_buffer_size));
		      return 0;
		    } else {
		      printf("Unable to set hw params for capture: %s\n",
			     snd_strerror(err));
		    }
		  } else {
		    printf("Unable to get period size back: %s\n",
                           snd_strerror(err));
		  }
		} else {
		  printf("Unable to set period time %i for capture: %s\n",
			 (int)(1000000*(uint64_t)period/rate),
                         snd_strerror(err));
		}
	      } else {
		printf("Unable to get buffer size back: %s\n",
		       snd_strerror(err));
	      }
	    } else {
	      printf("Unable to set buffer time %i for capture: %s\n",
		     (int)(1000000*(uint64_t)period*nperiods/rate),
                     snd_strerror(err));
	    }
	  } else {
	    printf("Rate %iHz not available for playback: %s\n",
		   rate, snd_strerror(err));
	  }
	} else {
	  printf("Channels count (%i) not available for record: %s\n",
		 channels, snd_strerror(err));
	}
      } else {
	printf("Sample format not available for playback: %s\n",
	       snd_strerror(err));
      }
    } else {
      printf("Access type not available for capture: %s\n", snd_strerror(err));
    }
  } else {
    printf("No configurations available for capture: %s\n", snd_strerror(err));
  }

  return err;
}

static inline int
set_swparams(snd_pcm_t *handle, snd_pcm_sw_params_t *swparams, int period) {
  int err;

  if ((err = snd_pcm_sw_params_current(handle, swparams)) == 0) {
    /* start the transfer when the buffer is full */
    if ((err = snd_pcm_sw_params_set_start_threshold(handle, swparams,
						     period)) >= 0) {
      if ((err = snd_pcm_sw_params_set_stop_threshold(handle, swparams,
						      -1)) >= 0) {
	if ((err = snd_pcm_sw_params_set_avail_min(handle, swparams,
						   2*period)) >= 0) {
	  if ((err = snd_pcm_sw_params(handle, swparams)) == 0) {
	    return 0;
	  } else {
	    printf("Unable to set sw params for capture: %s\n",
		   snd_strerror(err));
	  }
	} else {
	  printf("Unable to set avail min for capture: %s\n",
		 snd_strerror(err));
	}
      } else {
	printf("Unable to set start threshold mode for capture: %s\n",
	       snd_strerror(err));
      }
    } else {
      printf("Unable to set start threshold mode for capture: %s\n",
	     snd_strerror(err));
    }
  } else {
    printf("Unable to determine current sw params for capture: %s\n",
	   snd_strerror(err));
  }

  return err;
}

static inline snd_pcm_t *
openAudioIn(char *device, int rate, int channels, int period, int nperiods) {
  int err;
  snd_pcm_t *handle;

  if ((err = snd_pcm_open(&(handle), device,
			  SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK)) == 0) {
    snd_pcm_hw_params_t *hwparams;

    snd_pcm_hw_params_alloca(&hwparams);
    if ((err = set_hwparams(handle, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED,
			    rate, channels, period, nperiods)) == 0) {
      snd_pcm_sw_params_t *swparams;

      snd_pcm_sw_params_alloca(&swparams);
      if ((err = set_swparams(handle, swparams, period)) == 0) {
	snd_pcm_start(handle);
	snd_pcm_wait(handle, 100);

	return handle;
      } else {
	printf("Setting of swparams failed: %s\n", snd_strerror(err));
      }
    } else {
      printf("Setting of hwparams failed: %s\n", snd_strerror(err));
    }
  } else {
    printf("AudioIn open error: %s\n", snd_strerror(err));
  }

  return NULL;
}

#define MIN_RESAMPLE_FACTOR 0.25
#define MAX_RESAMPLE_FACTOR 4.0

static int process(jack_nframes_t nframes, void *arg) {
  int err;
  snd_pcm_sframes_t delay = snd_pcm_avail(alsa_handle);
  int i;

  delay -= jack_frames_since_cycle_start(jackClient);
  if (delay > (target_delay+max_diff)) {
    const int skipFrames = delay-target_delay;
    char tmp[skipFrames * formats[format].sample_size * num_channels]; 
    printf("Skipping %d frames\n", skipFrames);
    int count = skipFrames;
    while (count > 0) {
      int amount = snd_pcm_readi(alsa_handle, tmp, skipFrames);
      if (amount == -EAGAIN) continue;
      if (amount < 0) {
	xrun_recovery(alsa_handle, amount);
	continue;
      }
      count -= amount;
    }
    output_new_delay = (int)delay;

    delay += skipFrames;

    // Set the resample_rate... we need to adjust the offset integral, to do this.
    // first look at the PI controller, this code is just a special case, which should never execute once
    // everything is swung in. 
    offset_integral = - (resample_mean - static_resample_factor)
                    * catch_factor * catch_factor2;
    // Also clear the array. we are beginning a new control cycle.
    for (i=0; i<smooth_size; i++) offset_array[i] = 0.0;
  }
  if (delay < (target_delay-max_diff)) {
    int rewound = snd_pcm_rewind(alsa_handle, target_delay - delay);
    printf("Rewound %d, delay was %d\n", rewound, (int)delay);

    output_new_delay = (int)delay;
    delay += rewound;

    // Set the resample_rate... we need to adjust the offset integral, to do this.
    offset_integral = - (resample_mean - static_resample_factor)
                    * catch_factor * catch_factor2;
    for (i=0; i<smooth_size; i++) offset_array[i] = 0.0;
  }
  /* ok... now we should have target_delay +- max_diff on the alsa side.
   *
   * calculate the number of frames, we want to get.
   */

  double offset = delay - target_delay;

  // Save offset.
  offset_array[(offset_differential_index++)%smooth_size] = offset;

  // Build the mean of the windowed offset array
  // basically fir lowpassing.
  double smooth_offset = 0.0;
  for (i=0; i<smooth_size; i++)
    smooth_offset += offset_array[(i + offset_differential_index-1)
				  % smooth_size] * window_array[i];
  smooth_offset /= (double)smooth_size;

  // this is the integral of the smoothed_offset
  offset_integral += smooth_offset;

  // Clamp offset.
  // the smooth offset still contains unwanted noise
  // which would go straigth onto the resample coeff.
  // it only used in the P component and the I component is used for the fine tuning anyways.
  if (fabs(smooth_offset) < pclamp) smooth_offset = 0.0;

  // ok. now this is the PI controller. 
  // u(t) = K * ( e(t) + 1/T \int e(t') dt' )
  // K = 1/catch_factor and T = catch_factor2
  double current_resample_factor = static_resample_factor
                                 - smooth_offset / (double)catch_factor
                                 - offset_integral / (double)catch_factor
                                 / (double)catch_factor2;

  // quantize around resample_mean, so that noise in the integral component doesnt hurt.
  current_resample_factor = floor((current_resample_factor - resample_mean)
				  * controlquant + 0.5)
                          / controlquant + resample_mean;

  output_resampling_factor = (float)current_resample_factor;
  output_diff = (float) smooth_offset;
  output_integral = (float) offset_integral;
  output_offset = (float) offset;

  // Clamp a bit.
  if (current_resample_factor < MIN_RESAMPLE_FACTOR)
    current_resample_factor = MIN_RESAMPLE_FACTOR;
  else if (current_resample_factor > MAX_RESAMPLE_FACTOR)
    current_resample_factor = MAX_RESAMPLE_FACTOR;

  // Calculate resample_mean so we can init ourselves to saner values.
  resample_mean = 0.9999 * resample_mean + 0.0001 * current_resample_factor;

  {
    char *outbuf;
    float *resampbuf;
    int rlen = ceil(((double)nframes) / current_resample_factor)+2;
    int framesToRead = rlen;
    int readOffset = 0;
    assert(rlen > 2);

    outbuf = alloca(rlen * formats[format].sample_size * num_channels);

    resampbuf = alloca(rlen * sizeof( float ));

    // get the data...
    int iterations = 10;
    while (framesToRead > 0 && --iterations) {
      err = snd_pcm_readi(alsa_handle, outbuf+readOffset, framesToRead);
      if (err == -EAGAIN) { usleep(100); continue; }
      if (err < 0) {
	if (xrun_recovery(alsa_handle, err) < 0) {
	  printf("xrun_recover failed: %s\n", snd_strerror(err));
	  exit(EXIT_FAILURE);
	}
	continue;
      }
      readOffset += err * formats[format].sample_size * num_channels;
      framesToRead -= err;
    }

    int channel = 0;
    SRC_DATA src;
    int unusedFrames = 0;
    for (channel = 0; channel < num_channels; channel++) {
      float *buf = jack_port_get_buffer(jackPorts[channel], nframes);
      SRC_STATE *src_state = srcs[channel];

      formats[format].soundcard_to_jack(resampbuf,
					outbuf + format[formats].sample_size
					* channel, rlen,
					num_channels
					* format[formats].sample_size);

      src.data_in = resampbuf;
      src.input_frames = rlen;

      src.data_out = buf;
      src.output_frames = nframes;
      src.end_of_input = 0;
      src.src_ratio = current_resample_factor;

      src_process(src_state, &src);

      unusedFrames = rlen - src.input_frames_used;
    }

    if (unusedFrames) {
      if (verbose > 1) printf("putback = %d\n", unusedFrames);
      snd_pcm_rewind(alsa_handle, unusedFrames);
    }
  }

  return 0;
}

/**
 * Allocate the necessary jack ports...
 */
static void
alloc_ports(int n_capture) {
  for (int chn = 0; chn < n_capture; chn++) {
    char buf[32];
    snprintf(buf, sizeof(buf) - 1, "capture_%u", chn+1);

    jack_port_t *port = jack_port_register(jackClient, buf,
					   JACK_DEFAULT_AUDIO_TYPE,
                                           JackPortIsOutput, 0);

    if (port == NULL) {
      printf("cannot register port %s\n", buf);
      exit(EXIT_FAILURE);
    }

    srcs[chn] = src_new(4-resample_quality, 1, NULL);
    jackPorts[chn] = port;
  }
}

static void
jack_shutdown(void *arg) {
  exit(1);
}

static void
sigterm_handler(int signal) {
  quit = 1;
}

static inline double hann(double x) { return 0.5 * (1.0 - cos(2*M_PI * x)); }
static int
setupSmoothing() {
  if ((offset_array = malloc(sizeof(double) * smooth_size)) != NULL) {
    if ((window_array = malloc(sizeof(double) * smooth_size)) != NULL) {
      int i;
      for (i=0; i<smooth_size; i++) {
	offset_array[i] = 0.0;
	window_array[i] = hann((double)i / ((double) smooth_size - 1.0));
      }

      return 1;
    } else {
      fprintf(stderr, "no memory for window_array\n");
    }
  } else {
    fprintf(stderr, "no memory for offset_array\n");
  }
  return 0;
}
#endif

int
main(int argc, char *argv[]) {
  int fd, option;
  float newFreq = 0;
  char *outFile = NULL;
  char *device = DEFAULT_RADIO_DEVICE;
  char *alsaDevice = DEFAULT_AUDIO_DEVICE;
  int seekUp = 0, useJack = 0;

  while ((option = getopt(argc, argv, "a:d:jF:o:sv")) != -1) {
    switch (option) {
    case 'a':
      alsaDevice = optarg;
      break;
    case 'd':
      device = optarg;
      break;
    case 'F':
      newFreq = strtof(optarg, (char **)NULL);
      break;
    case 'j':
      useJack = 1;
      break;
    case 'o':
      outFile = optarg;
      break;
    case 's':
      seekUp = 1;
      break;
    case 'v':
      verbose += 1;
      break;
    default:
      fprintf(stderr, "Usage: %s [-d DEVICE] [-a ALSADEV] [-F FREQ] "
	              "[[-j] | [-o OUT.ogg]] [-v]\n"
	              "\n"
	              "Options\n"
	              "\t-d DEVICE\tRadio device (default %s)\n"
	              "\t-a ALSADEV\tAudio device to read from (default %s)\n"
	              "\t-j\t\tUse JACK for output\n"
	              "\t-o FILE.ogg\tWrite output to file\n"
	              "\t-F FREQ\t\tSet frequency (in MHz)\n"
	              "\t-v\t\tIncrease verbosity\n",
              argv[0],
	      DEFAULT_RADIO_DEVICE, DEFAULT_AUDIO_DEVICE);
      exit(EXIT_FAILURE);
    }
  }

  if ((fd = open(device, O_RDONLY)) > 0) {
    struct v4l2_tuner tuner;

    memset(&tuner, 0, sizeof(tuner));

    if (ioctl(fd, VIDIOC_G_TUNER, &tuner) != -1) {
      struct v4l2_capability caps;

      printf("Tuner: %s (%s), %d\n",
	     tuner.name, tuner.audmode&V4L2_TUNER_MODE_STEREO?"stereo":"mono",
	     tuner.signal);
      if (ioctl(fd, VIDIOC_QUERYCAP, &caps) != -1) {
	printf("Capabilities: %X\n", caps.capabilities);
      } else perror("ioctl VIDIOC_QUERYCAP");

      if (tuner.type == V4L2_TUNER_RADIO) {
	int cpid;

	frequencyDivider = (tuner.capability & V4L2_TUNER_CAP_LOW)? 16000: 16;
	minFrequency = ((float)tuner.rangelow)/frequencyDivider;
	maxFrequency = ((float)tuner.rangehigh)/frequencyDivider;
	
	printf("Radio: %.1f <= %.1f <= %.1f\n",
	       minFrequency, getTunerFrequency(fd), maxFrequency);

	if (newFreq != 0) {
	  setTunerFrequency(fd, &tuner, currentFrequency = newFreq);
	} else {
	  currentFrequency = getTunerFrequency(fd);
	}

	if (seekUp) {
	  float freq = seekTunerFrequency(fd, 0);
	  if (freq >= minFrequency/2) {
	    currentFrequency = freq;
	    printf("Seek stopped at %.2f\n", freq);
	  } else {
	    printf("Seek failed\n");
	  }
	}

	setTunerVolume(fd, 100);

	cpid = fork();
	      
	if (cpid == 0) {
	  char command[0XFF];
 
	  if (outFile != NULL) {
	    snprintf(command, sizeof(command)/sizeof(*command),
		     "arecord -q -D '%s' -r96000 -c2 -f S16_LE |"
		     "oggenc -Q --resample 48000 -q 5 -o '%s' -",
		     alsaDevice, outFile);
	    execl("/bin/sh", "sh", "-c", command, (char *)0);
	  } else {
	    if (useJack) {
#ifdef HAVE_JACK
	      const char *jack_name = "si470x";

	      if (setupSmoothing()) {
		if ((alsa_handle = openAudioIn(alsaDevice,
						inputSampleRate, num_channels,
						period_size, num_periods))
		    != NULL) {
		  if ((jackClient = jack_client_open(jack_name, 0, NULL)) 
		      != NULL) {
		    jack_set_process_callback(jackClient, process, 0);
		    jack_on_shutdown(jackClient, jack_shutdown, 0);
		    jackSampleRate = jack_get_sample_rate(jackClient);

		    static_resample_factor = (double)jackSampleRate
		                           / (double)inputSampleRate;
		    resample_mean = static_resample_factor;
      
		    jackBufferSize = jack_get_buffer_size(jackClient);
		    if (!target_delay)
		      target_delay = (num_periods*period_size / 2)
			           + jackBufferSize/2;
		    if (!max_diff)
		      max_diff = num_periods*period_size - target_delay;	
      
                    if (verbose > 1)
                      printf("target_delay=%d\nmax_diff=%d\n",
                             target_delay, max_diff);
		    alloc_ports(num_channels);

		    if (jack_activate(jackClient) == 0) {
		      signal(SIGTERM, sigterm_handler);
		      signal(SIGINT, sigterm_handler);

		      int i;
		      const char **port = jack_get_ports(jackClient, NULL, NULL,
							 JackPortIsInput);
		      for (i = 0; i < num_channels && *port; i++) {
			if (*port[0]) {
			  jack_connect(jackClient,
				       jack_port_name(jackPorts[i]),
				       *port);
			  port++;
			}
		      }
		      while (!quit) {
			usleep(250000);
			if (verbose > 0 && output_new_delay > 0) {
			  printf("delay = %d\n", output_new_delay);
                          output_new_delay = 0;
                        }
                        if (verbose > 1)
			  printf("srcfactor: %f, diff = %f, offset = %f, integral=%f\n",
				 output_resampling_factor, output_diff,
                                 output_offset, output_integral);
		      }

                      jack_deactivate(jackClient);
		    } else {
		      fprintf(stderr, "cannot activate JACK client\n");
		    }
		    jack_client_close(jackClient);
		    src_delete(srcs[0]); src_delete(srcs[1]);

		    exit(0);
		  } else {
		    fprintf (stderr, "jack server not running?\n");
		  }
		}
	      }

	      exit(1);
#else
	      printf("Jack support not compiled in\n");
#endif
	    } else {
	      snprintf(command, sizeof(command)/sizeof(*command),
		       "arecord -q -D '%s' -r96000 -c2 -f S16_LE |"
		       "aplay -q -B -",
		       alsaDevice);
	      execl("/bin/sh", "sh", "-c", command, (char *)0);
	    }
	  }
	  perror("execl");
	  return 1;
	} else {
	  if (caps.capabilities & V4L2_CAP_RDS_CAPTURE) {
	    decodeRds(fd, &tuner);
	  } else {
	    printf("Radio Data System not supported, "
		   "try linux-2.6.32 or later\n");
	    while (1) sleep(1);
	  }
	  //kill(-cpid, SIGTERM);
	}
      } else {
	printf("%s is not a FM radio\n", device);
      }
    } else {
      perror("ioctl VIDIOC_G_TUNER");
    }

    close(fd);
  } else {
    switch (errno) {
    case ENOENT:
      printf("Device %s does not exist\n", device);
      break;
    default:
      perror("open");
    }
  }

  return 1;
}
