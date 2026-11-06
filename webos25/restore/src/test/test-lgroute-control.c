#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sound/asound.h>

static long output[5] = {2,1,0,0,1};
static int unsupported, ignore_write, writes;
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
  if(command==SNDRV_CTL_IOCTL_ELEM_READ){
    if(!strcmp((char *)value->id.name,"Sndout EARC OnOff"))value->value.integer.value[0]=1;
    else memcpy(value->value.integer.value,output,sizeof output);
    return 0;
  }
  assert(command==SNDRV_CTL_IOCTL_ELEM_WRITE);
  if(!ignore_write)memcpy(output,value->value.integer.value,sizeof output);
  writes++;return 0;
}
#define LGROUTE_PT LGROUTE_RUN "/passthrough"
#define LGROUTE_HD LGROUTE_RUN "/passthrough-hd"
#define ioctl fake_ioctl
#include "../lgroute-control.c"
#undef ioctl

static void journal(const long before[5],const long applied[5])
{
  FILE *f=fopen(LGROUTE_STATE,"w");assert(f);
  fprintf(f,"LGR1 %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld\n",before[0],before[1],before[2],before[3],before[4],applied[0],applied[1],applied[2],applied[3],applied[4]);
  fclose(f);
}
int main(void)
{
  assert(!mkdir(LGROUTE_RUN,0700)||errno==EEXIST);
  long values[5],before[5]={2,1,0,0,1},applied[5]={2,1,0,0,0};
  assert(!lgroute_control_read(3,"Sndout EARC Outputtype",values,5));
  assert(!memcmp(values,before,sizeof values));
  unsupported=1;assert(lgroute_control_read(3,"Sndout EARC Outputtype",values,5)<0);unsupported=0;
  memcpy(output,applied,sizeof output);journal(before,applied);
  assert(!restore(3));assert(output[4]==1&&writes==1&&access(LGROUTE_STATE,F_OK));
  memcpy(output,applied,sizeof output);output[2]=8;journal(before,applied);
  assert(!restore(3));assert(output[2]==8&&output[4]==0&&writes==1);
  memcpy(output,applied,sizeof output);journal(before,applied);ignore_write=1;
  assert(restore(3)<0);assert(!access(LGROUTE_STATE,F_OK));ignore_write=0;
  assert(!restore(3));
  FILE *f=fopen(LGROUTE_STATE,"w");assert(f);fputs("bad state",f);fclose(f);
  assert(restore(3)<0);assert(!access(LGROUTE_STATE,F_OK));unlink(LGROUTE_STATE);
  assert(!lgroute_control_recover());
  long corrupt[5]={2,1,6,0,0};journal(before,corrupt);
  assert(restore(3)<0);assert(!access(LGROUTE_STATE,F_OK));unlink(LGROUTE_STATE);
  journal(before,applied);f=fopen(LGROUTE_STATE,"a");assert(f);fputs("garbage",f);fclose(f);
  assert(restore(3)<0);assert(!access(LGROUTE_STATE,F_OK));unlink(LGROUTE_STATE);
  FILE *pt=fopen(LGROUTE_PT,"w"), *hd=fopen(LGROUTE_HD,"w");assert(pt&&hd);fclose(pt);fclose(hd);
  f=fopen(LGROUTE_OUTPUT,"w");assert(f);fprintf(f,"%ld external_arc passThrough\n",(long)getpid());fclose(f);
  LgRouteControl control={3,-1};
  memcpy(output,before,sizeof output);assert(!lgroute_control_apply(&control));assert(output[4]==0);
  assert(!lgroute_control_apply(&control));assert(!restore(3));assert(output[4]==1);
  memcpy(output,before,sizeof output);ignore_write=1;assert(lgroute_control_apply(&control)<0);
  assert(!access(LGROUTE_STATE,F_OK));ignore_write=0;assert(!restore(3));
  memcpy(output,applied,sizeof output);journal(before,applied);
  int lock=open(LGROUTE_LOCK,O_CREAT|O_RDWR,0600);assert(lock>=0&&!flock(lock,LOCK_EX|LOCK_NB));
  assert(!lgroute_control_recover());assert(!access(LGROUTE_STATE,F_OK));
  close(lock);assert(!restore(3));
  unlink(LGROUTE_PT);unlink(LGROUTE_HD);unlink(LGROUTE_OUTPUT);unlink(LGROUTE_LOCK);
  puts("LG mixer: ABI, apply/readback, owned restore, foreign changes, failed restore, malformed journal and active-lock recovery pass");
  return 0;
}
