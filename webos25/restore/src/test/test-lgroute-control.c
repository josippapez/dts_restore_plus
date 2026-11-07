#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sound/asound.h>

static long output[5] = {2,1,0,0,1}, earc = 1;
static int unsupported;
static int fake_ioctl(int fd, unsigned long command, ...)
{
  (void)fd;
  va_list args; va_start(args,command); void *argument=va_arg(args,void *); va_end(args);
  if(command==SNDRV_CTL_IOCTL_ELEM_INFO){
    struct snd_ctl_elem_info *info=argument;
    info->type=unsupported?SNDRV_CTL_ELEM_TYPE_BOOLEAN:SNDRV_CTL_ELEM_TYPE_INTEGER;
    info->count=!strcmp((char *)info->id.name,"Sndout EARC OnOff")?1:5;
    info->access=SNDRV_CTL_ELEM_ACCESS_READWRITE;return 0;
  }
  struct snd_ctl_elem_value *value=argument;
  /* LG owns the mixer word; the helper must never write it. */
  assert(command==SNDRV_CTL_IOCTL_ELEM_READ);
  if(!strcmp((char *)value->id.name,"Sndout EARC OnOff"))value->value.integer.value[0]=earc;
  else memcpy(value->value.integer.value,output,sizeof output);
  return 0;
}
#define LGROUTE_PT LGROUTE_RUN "/passthrough"
#define LGROUTE_HD LGROUTE_RUN "/passthrough-hd"
#define LGROUTE_LUNA_SEND LGROUTE_RUN "/luna-send"
#define ioctl fake_ioctl
#include "../lgroute-control.c"
#undef ioctl

static void sound_output(const char *mode)
{
  FILE *f=fopen(LGROUTE_OUTPUT,"w");assert(f);
  fprintf(f,"%ld external_arc %s\n",(long)getpid(),mode);fclose(f);
}
int main(void)
{
  assert(!mkdir(LGROUTE_RUN,0700)||errno==EEXIST);
  long values[5];
  assert(!lgroute_control_read(3,"Sndout EARC Outputtype",values,5));
  assert(!memcmp(values,output,sizeof values));
  unsupported=1;assert(lgroute_control_read(3,"Sndout EARC Outputtype",values,5)<0);unsupported=0;
  FILE *pt=fopen(LGROUTE_PT,"w"), *hd=fopen(LGROUTE_HD,"w");assert(pt&&hd);fclose(pt);fclose(hd);
  sound_output("auto");assert(!lgroute_control_receiver());
  sound_output("pcm");assert(!lgroute_control_receiver());
  sound_output("passThrough");assert(lgroute_control_receiver());
  long state[5];
  output[4]=1;assert(ready(3,state)&&state[4]==1);
  output[4]=0;assert(ready(3,state)&&state[4]==0);
  earc=0;assert(!ready(3,state));earc=1;
  output[2]=99;assert(!ready(3,state));output[2]=0;
  unlink(LGROUTE_PT);assert(!lgroute_control_receiver());
  unlink(LGROUTE_HD);unlink(LGROUTE_OUTPUT);

  FILE *luna=fopen(LGROUTE_LUNA_SEND,"w");assert(luna);
  fputs("#!/bin/sh\necho \"$5 $6\" >> " LGROUTE_RUN "/luna.log\n"
        "case $5 in *getAudioInputProperty) echo '{\"returnValue\":true,\"audioInputProperty\":["
        "{\"codec\":\"ac3\",\"pipelineId\":\"_other\",\"immersive\":\"none\"},"
        "{\"codec\":\"dts\",\"pipelineId\":\"_p1\",\"immersive\":\"none\"}]}' ;;"
        " *) echo '{\"returnValue\":true}' ;; esac\n",luna);
  fclose(luna);assert(!chmod(LGROUTE_LUNA_SEND,0700));
  char codec[32],immersive[16];
  assert(!lgroute_input_query("_p1",codec,sizeof codec,immersive,sizeof immersive));
  assert(!strcmp(codec,"dts")&&!strcmp(immersive,"none"));
  assert(lgroute_input_query("_none",codec,sizeof codec,immersive,sizeof immersive)<0);
  assert(!lgroute_input_set("_p1","ADEC1","pcm","8","none"));
  assert(!lgroute_input_set("_p1","ADEC1","dts",NULL,"none"));
  /* Values reach a JSON payload; quotes or braces are refused. */
  assert(lgroute_input_set("_p1\",\"x","ADEC1","pcm","8","none")<0);
  assert(lgroute_input_set("","ADEC1","pcm","8","none")<0);
  char log[2048]={0};FILE *l=fopen(LGROUTE_RUN "/luna.log","r");assert(l);
  size_t n=fread(log,1,sizeof log-1,l);fclose(l);log[n]=0;
  assert(strstr(log,"tv/setQualityAttribute {\"categoryType\":\"audioInputInfo\",\"requestType\":\"setAudioInputInfo\","
      "\"pipelineInfo\":{\"pipelineId\":\"_p1\",\"resource\":\"ADEC1\",\"sourceType\":\"MEDIA\"},"
      "\"categoryInfo\":{\"codec\":\"pcm\",\"channel\":\"8\",\"immersive\":\"none\"}}"));
  assert(strstr(log,"\"categoryInfo\":{\"codec\":\"dts\",\"immersive\":\"none\"}}"));
  assert(!strstr(log,"_p1\\\""));
  unlink(LGROUTE_RUN "/luna.log");unlink(LGROUTE_LUNA_SEND);
  puts("LG mixer: ABI, Pass Through only, read-only mixer state, unavailable eARC and PCM input report pass");
  return 0;
}
