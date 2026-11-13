#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sound/asound.h>

static long output[5] = {2,1,0,0,1}, earc = 1;
static int unsupported;
static int writes, write_fails, open_fails;
static const char *missing;
static int fake_open(const char *name, int flags)
{
  assert(!strcmp(name,"/dev/snd/controlC0"));
  if(open_fails){errno=ENOENT;return -1;}
  return open("/dev/null",flags);
}
static int fake_ioctl(int fd, unsigned long command, ...)
{
  (void)fd;
  va_list args; va_start(args,command); void *argument=va_arg(args,void *); va_end(args);
  if(command==SNDRV_CTL_IOCTL_ELEM_INFO){
    struct snd_ctl_elem_info *info=argument;
    if(missing&&!strcmp((char *)info->id.name,missing)){errno=ENOENT;return -1;}
    info->type=unsupported?SNDRV_CTL_ELEM_TYPE_BOOLEAN:SNDRV_CTL_ELEM_TYPE_INTEGER;
    info->count=!strcmp((char *)info->id.name,"Sndout EARC OnOff")?1:5;
    info->access=SNDRV_CTL_ELEM_ACCESS_READWRITE;return 0;
  }
  struct snd_ctl_elem_value *value=argument;
  if(command==SNDRV_CTL_IOCTL_ELEM_WRITE){
    assert(!strcmp((char *)value->id.name,"Sndout EARC Outputtype"));
    writes++;
    if(write_fails){errno=EIO;return -1;}
    memcpy(output,value->value.integer.value,sizeof output);return 0;
  }
  assert(command==SNDRV_CTL_IOCTL_ELEM_READ);
  if(!strcmp((char *)value->id.name,"Sndout EARC OnOff"))value->value.integer.value[0]=earc;
  else memcpy(value->value.integer.value,output,sizeof output);
  return 0;
}
#define LGROUTE_PT LGROUTE_RUN "/passthrough"
#define LGROUTE_HD LGROUTE_RUN "/passthrough-hd"
#define ioctl fake_ioctl
#define open fake_open
#include "../lgroute-control.c"
#undef open
#undef ioctl

static void sound_output_on(const char *out,const char *mode)
{
  FILE *f=fopen(LGROUTE_OUTPUT,"w");assert(f);
  fprintf(f,"%ld %s %s\n",(long)getpid(),out,mode);fclose(f);
}
static void sound_output(const char *mode){sound_output_on("external_arc",mode);}
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
  long state[5];char why[LGROUTE_WHY];
  output[4]=1;assert(ready(3,state,why)&&state[4]==1);
  output[4]=0;assert(ready(3,state,why)&&state[4]==0);
  /* A tester's log said only "unavailable": the reason names the control and what it read. */
  earc=0;assert(!ready(3,state,why));earc=1;
  assert(!strcmp(why,"Sndout EARC OnOff is 0, not on"));
  output[2]=99;assert(!ready(3,state,why));output[2]=0;
  assert(!strcmp(why,"Sndout EARC Outputtype is 2,1,99,0,0, not a known output type"));
  missing="Sndout EARC OnOff";assert(!ready(3,state,why));
  assert(!strcmp(why,"Sndout EARC OnOff could not be read: No such file or directory"));
  missing="Sndout EARC Outputtype";assert(!ready(3,state,why));missing=NULL;
  assert(!strcmp(why,"Sndout EARC Outputtype could not be read: No such file or directory"));
  unsupported=1;assert(!ready(3,state,why));unsupported=0;
  assert(!strcmp(why,"Sndout EARC OnOff could not be read: Operation not supported"));
  assert(!writes);
  open_fails=1;
  assert(lgroute_control_unmixed_why(why)<0);
  assert(!strcmp(why,"/dev/snd/controlC0 could not be opened: No such file or directory"));
  assert(lgroute_control_unmix()<0&&!writes);open_fails=0;
  long expected[5]={2,6,8,192000,0};
  memcpy(output,expected,sizeof output);output[4]=1;
  assert(!lgroute_control_unmix()&&writes==1);
  assert(!memcmp(output,expected,sizeof output));
  assert(lgroute_control_unmixed_why(why)==1&&writes==1);
  output[4]=1;write_fails=1;
  assert(lgroute_control_unmix()<0&&writes==2&&output[4]==1);write_fails=0;
  missing="Sndout EARC Outputtype";
  assert(lgroute_control_unmix()<0&&writes==2);missing=NULL;
  unsupported=1;assert(lgroute_control_unmix()<0&&writes==2);unsupported=0;
  output[2]=99;assert(lgroute_control_unmix()<0&&writes==2);
  memcpy(output,expected,sizeof output);
  sound_output("auto");char mode[64];assert(lgroute_sound_output(mode)&&!strcmp(mode,"auto"));
  assert(lgroute_core_output(mode)&&!strcmp(mode,"auto"));
  /* Optical carries the DTS core, never the HD carrier. */
  sound_output_on("external_optical","passThrough");
  assert(lgroute_core_output(mode)&&!strcmp(mode,"passThrough"));
  assert(!lgroute_sound_output(mode));assert(!lgroute_control_receiver());
  sound_output_on("tv_speaker","passThrough");assert(!lgroute_core_output(mode));
  FILE *f=fopen(LGROUTE_OUTPUT,"w");assert(f);fprintf(f,"0 external_optical passThrough\n");fclose(f);
  assert(!lgroute_core_output(mode));
  sound_output("auto");
  unlink(LGROUTE_PT);assert(!lgroute_control_receiver());
  unlink(LGROUTE_HD);unlink(LGROUTE_OUTPUT);assert(!lgroute_sound_output(mode));assert(!lgroute_core_output(mode));
  puts("LG mixer: ABI, Pass Through only, optical core-only, mix-only writes and unavailable eARC with its reason pass");
  return 0;
}
