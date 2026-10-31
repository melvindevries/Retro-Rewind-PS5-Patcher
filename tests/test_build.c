#include "../core/rr_script.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void logcb(void *u, int l, const char *m){ (void)u; if(l>=1) fprintf(stderr,"[%d] %s\n",l,m); }
int main(int argc, char **argv){
  if(argc<6){fprintf(stderr,"usage: base builder pack region(E|P|J) out\n");return 2;}
  rr_ctx c; rr_ctx_init(&c); c.log=logcb;
  wbfs_img w; wii_part p; wii_fst f; rr_vtree t; rr_build_cfg cfg; rr_build_result res;
  const char *R=argv[4];
  const char *src_id = R[0]=='E'?"RMCE01":R[0]=='P'?"RMCP01":"RMCJ01";
  const char *type = R[0]=='E'?"USA":R[0]=='P'?"EUR":"JPN";
  const char *dest = R[0]=='E'?"RMCETO":R[0]=='P'?"RMCPTO":"RMCJTO";
  const char *boot = R[0]=='E'?"RMCE":R[0]=='P'?"RMCP":"RMCJ";
  memset(&cfg,0,sizeof cfg);
  strcpy(cfg.disc_id,dest); strcpy(cfg.boot_id,boot); strcpy(cfg.ticket_id,"RMCR"); strcpy(cfg.tmd_id,"RMCR"); strcpy(cfg.name,"Mario Kart Retro Rewind");
  if(wbfs_img_open(&c,&w,argv[1])||wii_find_data_partition(&c,&w,&p)||wii_read_fst(&c,&p,&f)){fprintf(stderr,"ERR %s\n",c.err);return 1;}
  rr_vt_init(&t);
  if(rr_vt_load_base(&c,&t,&p,&f)){fprintf(stderr,"ERR %s\n",c.err);return 1;}
  char sp[4096]; snprintf(sp,sizeof sp,"%s/copy-files.sh",argv[2]);
  char *script; size_t sl; if(rr_read_text_file(sp,&script,&sl,1<<20)){fprintf(stderr,"cannot read script\n");return 1;}
  char v1[64],v2[64],v3[64],v4[64]; snprintf(v1,64,"SRC_ID=%s",src_id); snprintf(v2,64,"SRC_TYPE=%s",type); snprintf(v3,64,"DEST_ID=%s",dest); snprintf(v4,64,"DEST_NAME=%s","Mario Kart Retro Rewind");
  const char *vars[]={v1,v2,v3,v4,NULL};
  rr_script_roots roots={argv[2],argv[3]};
  if(rr_script_run(&c,&t,script,&roots,vars)){fprintf(stderr,"SCRIPT ERR %s\n",c.err);return 1;}
  if(wii_build_wbfs(&c,&p,&t,&cfg,argv[5],&res)){fprintf(stderr,"BUILD ERR %s\n",c.err);return 1;}
  printf("built: %llu bytes wbfs, %u clusters, %llu files\n",(unsigned long long)res.wbfs_size,res.clusters,(unsigned long long)res.file_count);
  uint64_t nf=0; if(rr_verify_wbfs(&c,argv[5],dest,res.plain_sha1,&nf)){fprintf(stderr,"VERIFY ERR %s\n",c.err);return 1;}
  printf("verified OK (%llu files)\n",(unsigned long long)nf);
  return 0;
}
