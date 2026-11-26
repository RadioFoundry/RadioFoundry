// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Actual radio/follower access invalidation at queued and prepared burst boundaries.
#include "hostlink/build_info.h"
#define main existing_radio_test_main
#include "radioCoreTest.c"
#undef main
// Keep the production service timestamp local distinct from the fake board's clock.
#define now p25_service_now
#include "../../core/radio/radio_p25.c"
#undef now
static p25_follow_t *access_follower(void) { return &mem->trunk.f; }
static int access_burst(p25_isp_t *out) { return p25_isp_parse(mem->trunk.burst,out); }
static void access_begin(void) {
 setup_codeplug_p25(); add(3,"Review",TS_CC1,RADIO_MODE_DMR,false,2);
 chans[3].kind=CODEPLUG_KIND_P25_TRUNK; chans[3].system=0;
 memset(&stored_trunk,0,sizeof stored_trunk);
 stored_trunk.sys=(p25_tsys_t){.wacn=TS_WACN,.sysid=TS_SYSID,.cc_nac=TS_NAC};
 stored_trunk.unit_id=4321;stored_trunk.talk_group=201;stored_trunk.ctl_count=1;
 stored_trunk.ctl[0]=(p25_ctl_t){.rx_hz=TS_CC1,.tx_hz=TS_CC1+TS_OFFSET,.chan=0x1001};
 stored_trunk.iden_set=2;
 stored_trunk.iden[1]=(p25_iden_t){.base_hz=TS_BASE,.spacing_hz=12500,.tx_offset_hz=TS_OFFSET};
 memset(&site,0,sizeof site);site.cc_hz=TS_CC1;site.cc_in_hz=TS_CC1+TS_OFFSET;
 site.voice_hz=TS_VOICE;site.on=true;
 start();p25_defaults();aud.feed=true;req(RREQ_ZONE_POS,2);run(2000);site.on=false;
}
int main(void) {
 CHECK(strstr(build_features_now(), "P25_LOCATION_REG,") != NULL);
#if CONFIG_P25_ZONE_SCAN
 // Literal zero-based channel-map boundaries, including all four packed positions and the final zone slot.
 static p25_trunk_t map;
 scan_channel_set(&map,0,1);scan_channel_set(&map,1,256);
 scan_channel_set(&map,2,257);scan_channel_set(&map,3,1024);
 CHECK(!memcmp(map.scan_channels,(uint8_t[]){0,255,0,255},4) && map.scan_channel_hi[0]==0xD0);
 CHECK(scan_channel(&map,0)==1 && scan_channel(&map,1)==256 && scan_channel(&map,2)==257 && scan_channel(&map,3)==1024);
 map.scan_applied=true;map.scan_flags=0xA5;map.base_ngroups=17;
 scan_channel_set(&map,CODEPLUG_ZONE_MAX-1,1024);
 CHECK(map.scan_channels[CODEPLUG_ZONE_MAX-1]==255 && map.scan_channel_hi[19]==0xC0 &&
       scan_channel(&map,CODEPLUG_ZONE_MAX-1)==1024 && map.scan_applied && map.scan_flags==0xA5 && map.base_ngroups==17);
 scan_channel_set(&map,2,1);
 CHECK(map.scan_channel_hi[0]==0xC0 && scan_channel(&map,3)==1024 && scan_channel(&map,2)==1);
#endif
 for(unsigned kind=0;kind<3;kind++) for(unsigned prepared=0;prepared<2;prepared++) {
  access_begin();p25_follow_t *f=access_follower();p25_isp_t q;
#if CONFIG_P25_ZONE_SCAN
  CHECK(!radio_p25_zone_scan_start(&zones[0],&chans[3],0) &&
        !radio_p25_zone_scan_start(&zones[0],&chans[3],CODEPLUG_CHANNELS_MAX+1));
#endif
  CHECK(f->view.reg==P25_REG_READY);
  p25_follow_ptt(f,true,now);p25_follow_tick(f,now+200);
  CHECK(access_burst(&q) && q.opcode==P25_ISP_GRP_V_REQ);
  if(prepared) CHECK(radio_p25_trunk_burst_start());
  uint8_t b[P25_TSBK_BYTES];p25_tsbk_t cmd={.kind=P25_TSBK_REG_CMD,.last=true};
  if(kind) { cmd.kind=kind==1?P25_TSBK_ACK:P25_TSBK_QUEUED;cmd.u.rsp.dst=4321;cmd.u.rsp.svc=P25_ISP_GRP_V_REQ; } else cmd.u.cmd.dst=4321;
  p25_tsbk_make(&cmd,b);p25_follow_tsbk(f,b,TS_NAC,now+201);
  CHECK(kind ? f->view.reg==P25_REG_READY : f->view.reg==P25_REG_WAIT && f->view.req!=P25_REQ_WAIT);
  bool accepted=prepared?radio_p25_tx_go():radio_p25_trunk_burst_start();
  printf("kind=%u prepared=%u queuedop=%02X reg=%u request=%u accepted=%u state=%u\n",kind,prepared,q.opcode,f->view.reg,f->view.req,accepted,p25_engine_tx_state(&mem->eng));
  CHECK(!accepted); if(prepared) CHECK(p25_engine_tx_state(&mem->eng)==AIR_TX_DONE);
  if (!kind) {
   if (prepared) radio_p25_tx_clear(); // the existing key/session owner retires DONE before another burst
   p25_follow_tick(f,now+500);
   CHECK(access_burst(&q) && q.opcode==P25_ISP_U_REG_REQ);
   CHECK(radio_p25_trunk_burst_start()); // the newly required registration is still allowed
   radio_p25_tx_abort();
  }
  req(RREQ_ZONE_POS,0);site.on=false;
 }
 printf("p25AccessContextTest: %u checks %u failures\n",checks,failures);return failures?1:0;
}
