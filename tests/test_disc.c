#include "../core/wii_disc.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc __attribute__((unused)),char**argv){
  rr_ctx c; rr_ctx_init(&c);
  wbfs_img w; wii_part p; wii_fst f;
  if(wbfs_img_open(&c,&w,argv[1])){printf("ERR %s\n",c.err);return 1;}
  if(wii_find_data_partition(&c,&w,&p)){printf("ERR %s\n",c.err);return 1;}
  if(wii_read_fst(&c,&p,&f)){printf("ERR %s\n",c.err);return 1;}
  printf("partoff=%llx data_off=%llx size=%llx entries=%zu dol=%x fst=%x/%x\n",(unsigned long long)p.part_off,(unsigned long long)p.data_off,(unsigned long long)p.data_size,f.n,f.dol_off,f.fst_off,f.fst_size);
  for(size_t i=0;i<f.n&&i<8;i++)printf("%s %d %llx %u\n",f.e[i].name,f.e[i].is_dir,(unsigned long long)f.e[i].offset,f.e[i].size);
  /* full read verifies all H0 */
  uint8_t *b=malloc(1<<24); size_t files=0;
  for(size_t i=0;i<f.n;i++){ if(f.e[i].is_dir)continue; uint64_t o=0; while(o<f.e[i].size){size_t n=f.e[i].size-o>1<<24?1<<24:f.e[i].size-o; if(wii_part_read(&c,&p,f.e[i].offset+o,b,n)){printf("ERR %s\n",c.err);return 1;} o+=n;} files++;}
  printf("%zu files read\n",files);
  printf("all clusters H0-verified OK\n");
  return 0;}
