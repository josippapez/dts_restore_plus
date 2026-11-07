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
  sound_output("auto");char mode[64];assert(lgroute_sound_output(mode)&&!strcmp(mode,"auto"));
  unlink(LGROUTE_PT);assert(!lgroute_control_receiver());
  unlink(LGROUTE_HD);unlink(LGROUTE_OUTPUT);assert(!lgroute_sound_output(mode));
  puts("LG mixer: ABI, Pass Through only, read-only mixer state and unavailable eARC pass");
  return 0;
}
