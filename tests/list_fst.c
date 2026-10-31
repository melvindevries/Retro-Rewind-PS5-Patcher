#include "../core/wii_disc.h"
#include <stdio.h>
int main(int argc __attribute__((unused)),char**argv){
  rr_ctx c; rr_ctx_init(&c); wbfs_img w; wii_part p; wii_fst f;
  if(wbfs_img_open(&c,&w,argv[1])||wii_find_data_partition(&c,&w,&p)||wii_read_fst(&c,&p,&f)){printf("ERR %s\n",c.err);return 1;}
  for(size_t i=0;i<f.n;i++)printf("%s\t%d\t%llx\t%u\n",f.e[i].name,f.e[i].is_dir,(unsigned long long)f.e[i].offset,f.e[i].size);
  printf("#dol %x fst %x/%x datasize %llx\n",f.dol_off,f.fst_off,f.fst_size,(unsigned long long)p.data_size);
  return 0;}
