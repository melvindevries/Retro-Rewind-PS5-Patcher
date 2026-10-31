#include "../core/wii_disc.h"
#include <stdio.h>
#include <stdlib.h>
static void hx(const char*t,const uint8_t*b,size_t n){printf("%s:",t);for(size_t i=0;i<n;i++){if(i%32==0)printf("\n %04zx:",i);printf(" %02x",b[i]);}printf("\n");}
int main(int argc,char**argv){
  rr_ctx c; rr_ctx_init(&c); wbfs_img w; wii_part p;
  if(wbfs_img_open(&c,&w,argv[1])||wii_find_data_partition(&c,&w,&p)){printf("ERR %s\n",c.err);return 1;}
  uint8_t h[0x800]; wbfs_img_read(&c,&w,0,h,0x100); hx("disc hdr",h,0x40);
  wbfs_img_read(&c,&w,0x40000,h,0x40); hx("ptab",h,0x20);
  wbfs_img_read(&c,&w,0x4e000,h,0x100); hx("region@4e000",h,0x20);
  uint8_t ph[0x2c0]; wbfs_img_read(&c,&w,p.part_off,ph,0x2c0); hx("part hdr tail",ph+0x2a4,0x1c);
  hx("ticket",p.ticket,sizeof p.ticket); hx("tmd",p.tmd,p.tmd_size); hx("cert-head",p.cert,0x40);
  printf("cert size %u\n",p.cert_size);
  uint8_t b[0x2440+0x2000]; wii_part_read(&c,&p,0,b,0x2440); hx("boot.bin",b,0x440); hx("bi2",b+0x440,0x40);
  uint8_t ap[0x20]; wii_part_read(&c,&p,0x2440,ap,0x20); hx("apploader hdr",ap,0x20);
  return 0;}
