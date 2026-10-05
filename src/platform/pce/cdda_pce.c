#include "cdda_pce.h"
#include "campaign_pce.h"
#include "overlay_pce.h"
#include "video_pce.h"
#include "loader_pce.h"
#include "music.h"

/* HuC scsi_handshake/scsi_get_status: preserve IFU IRQ enables, acknowledge
 * REQ, read status and message, then wait for BSY to drop before selecting.
 * D9 mode 3 releases the bus; modes 1/2 may hold BSY for the whole song.
 * Repeat is implemented by checking sub-Q and restarting the same extent. */
#define BUS (*(volatile uint8_t*)0x1800)
#define DATA (*(volatile uint8_t*)0x1801)
#define ACK (*(volatile uint8_t*)0x1802)
#define RESET (*(volatile uint8_t*)0x1804)
#define FADE (*(volatile uint8_t*)0x180f)
#define CD_CODE PCE_BOSS
enum { CD_IDLE,CD_REPLY,CD_MESSAGE,CD_FREE };
enum { CD_TIMEOUT=0xfe,CD_PROTOCOL=0xfd,CD_PENDING=0xff };
volatile uint8_t pce_music_status;
static uint8_t state,command,start_track,end_track,irq_mask,last_tick;
static uint8_t status,received,subq,attempts,check_clock,request_track;
static uint16_t waited;
static uint8_t lead_out[3];
static bool last_track;
static bool active,repeat,request_repeat,stopping;

CD_CODE static bool wait_bus(uint8_t mask,uint8_t value) {
    /* Bound even a missing device or disabled VBlank callback. Interrupts
     * remain enabled so raster/PCM service continues during every wait. */
    for(uint16_t n=0;n<60000;++n)if((BUS&mask)==value)return true;
    pce_music_status=CD_TIMEOUT;
    return false;
}
CD_CODE static bool handshake(void) {
    ACK=(ACK&0x7f)|0x80;
    bool ok=wait_bus(0x40,0);
    ACK=ACK&0x7f;
    return ok;
}
CD_CODE static void release(void) {
    ACK=(ACK&0x1f)|(irq_mask&0x60);state=CD_IDLE;
}
CD_CODE static void recover(void) {
    /* Never pass a partial transaction to a BIOS command. A reset is only
     * used on protocol failure/time-out, with the IRQ mask preserved. */
    ACK=ACK&0x1f;
    RESET=RESET|2;
    video_wait();
    RESET=RESET&~2;
    video_wait();video_wait();
    ACK=(ACK&0x1f)|(irq_mask&0x60);
    state=CD_IDLE;active=false;pce_stall=1;
}
CD_CODE static bool send(uint8_t op,uint8_t mode,uint8_t track) {
    irq_mask=ACK&0x7f;
    ACK=irq_mask&0x1f;   /* BIOS IRQ2 must not consume our SCSI response. */
    if(!wait_bus(0x80,0)){recover();return false;}
    DATA=0x81;BUS=0x81;   /* one selection write; no spurious BUS=0 selection */
    if(!wait_bus(0x80,0x80)){recover();return false;}
    uint8_t length=op==3?6:10;
    for(uint8_t k=0;k<length;++k) {
        uint8_t byte=k==0?op:k==1?mode:k==2?track:k==9?0x80:0;
        if(op==0xd9&&last_track) {
            if(k>=2&&k<=4)byte=lead_out[k-2];
            if(k==9)byte=0x40;   /* absolute BCD MSF, from the disc TOC */
        }
        if(op==3)byte=k==0?3:k==4?10:0;
        if(!wait_bus(0xf8,0xd0)){recover();return false;}
        DATA=byte;
        if(!handshake()){recover();return false;}
    }
    command=op;status=CD_PENDING;received=0;subq=0xff;
    state=CD_REPLY;waited=0;last_tick=pce_ticks;
    return true;
}
CD_CODE static void cue(void) {
    active=false;pce_music_status=CD_PENDING;
    FADE=0;   /* A prior BIOS fade must not leave CD-DA muted. */
    if(!send(0xd8,0,start_track)&&++attempts<3)send(0xd8,0,start_track);
}
CD_CODE static void failed(void) {
    active=false;
    if(stopping)return;
    if(++attempts<3)cue();
}
CD_CODE static void complete(void) {
    uint8_t op=command;
    release();
    if(status) {
        pce_music_status=status;
        if(stopping){recover();return;}
        if(status==2&&op!=3){if(!send(3,0,0))failed();}
        else failed();
        return;
    }
    if(op==3){failed();return;}   /* sense data clears CHECK CONDITION */
    if(op==0xda){active=false;pce_music_status=0;return;}
    if(op==0xd9) {
        active=true;pce_music_status=0;check_clock=0;attempts=0;
        return;
    }
    if(stopping)return;   /* cancelled cue: no D9 and no repeat restart */
    if(op==0xd8) {
        if(last_track) {if(!send(0xde,1,0))failed();}
        else if(!send(0xd9,3,end_track))failed();
    } else if(op==0xde) {
        if(received!=4){pce_music_status=CD_PROTOCOL;failed();return;}
        if(!send(0xd9,3,0))failed();
    } else if(op==0xdd) {
        if(received!=10){pce_music_status=CD_PROTOCOL;failed();return;}
        if(subq==3||subq==1) {
            active=false;
            if(repeat)cue();
        }
    }
}
CD_CODE static void poll(void) {
    uint8_t elapsed=pce_ticks-last_tick;last_tick=pce_ticks;
    if(state==CD_IDLE) {
        if(!active||stopping)return;
        uint16_t next=(uint16_t)check_clock+elapsed;
        check_clock=next>60?60:next;
        if(check_clock==60){check_clock=0;if(!send(0xdd,0,0))failed();}
        return;
    }
    waited+=elapsed;
    if(waited>=600){pce_music_status=CD_TIMEOUT;recover();failed();return;}
    /* Drain short replies in this frame when available. Every phase still
     * has a deadline; a delayed drive simply resumes on the next tick. */
    for(uint8_t n=0;n<16&&state!=CD_IDLE;++n) {
        uint8_t phase=BUS&0xf8;
        if(state==CD_FREE) {
            if(!(phase&0x80)){complete();return;}
            return;
        }
        if(!(phase&0x40))return;
        if(state==CD_REPLY&&phase==0xc8) {
            uint8_t byte=DATA;
            if(command==0xde&&received<3)lead_out[received]=byte;
            if(!received)subq=byte;
            if(received<255)++received;
        } else if(state==CD_REPLY&&phase==0xd8) {
            status=DATA;state=CD_MESSAGE;
        } else if(state==CD_MESSAGE&&phase==0xf8) {
            uint8_t message=DATA;
            if(message){pce_music_status=CD_PROTOCOL;recover();failed();return;}
            state=CD_FREE;
        } else {pce_music_status=CD_PROTOCOL;recover();failed();return;}
        if(!handshake()){recover();failed();return;}
    }
}
CD_CODE static void stop(void) {
    stopping=true;
    /* Complete only the outstanding transaction, never start the cancelled
     * track while the loader is waiting for exclusive drive ownership. */
    for(uint16_t n=0;state!=CD_IDLE&&n<610;++n){poll();if(state!=CD_IDLE)video_wait();}
    if(state!=CD_IDLE){pce_music_status=CD_TIMEOUT;recover();}
    if(active) {
        if(send(0xda,0,0)) {
            for(uint16_t n=0;state!=CD_IDLE&&n<610;++n){poll();if(state!=CD_IDLE)video_wait();}
            if(state!=CD_IDLE){pce_music_status=CD_TIMEOUT;recover();}
        }
    }
    active=false;stopping=false;
}
CD_CODE static void start(void) {
    stop();
    uint8_t level=pce_options.music&3;
    if(!level||request_track>=CDDA_TRACK_COUNT)return;
    last_track=request_track==CDDA_TRACK_COUNT-1;
    uint8_t track=request_track+CDDA_FIRST_TRACK;
    start_track=(track/10)*16+track%10;++track;
    end_track=(track/10)*16+track%10;
    repeat=request_repeat;attempts=0;cue();
}
void cdda_start(uint8_t track,bool loop) {request_track=track;request_repeat=loop;overlay_call(0x7b,start);}
void cdda_stop(void) {overlay_call(0x7b,stop);}
void cdda_tick(void) {if(active||state!=CD_IDLE)overlay_call(0x7b,poll);}
bool cdda_busy(void) {return active||state!=CD_IDLE;}
