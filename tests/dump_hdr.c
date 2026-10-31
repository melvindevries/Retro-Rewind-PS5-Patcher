#include "../core/wii_disc.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char**argv){
  rr_ctx c; rr_ctx_init(&c); wbfs_img w; wii_part p; int cl=atoi(argv[2]);
  if(wbfs_img_open(&c,&w,argv[1])||wii_find_data_partition(&c,&w,&p)){puts(c.err);return 1;}
  uint8_t raw[0x8000],dec[0x400],iv[16]={0};
  wbfs_img_read(&c,&w,p.part_off+p.data_off+(uint64_t)cl*0x8000,raw,0x8000);
  rr_aes128_cbc_decrypt(&p.aes,iv,raw,dec,0x400);
  for(int i=0x280;i<0x400;i++){ if((i-0x280)%20==0)printf("\n%03x:",i); printf("%02x",dec[i]); }
  printf("\n");return 0;}
