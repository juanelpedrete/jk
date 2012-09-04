#include "spg.h"
#include "Renderer_if.h"
#include "regs.h"
#include "threadedPvr.h"

#include <dc/sh4/sh4_opcode_list.h>

//SPG emulation; Scanline/Raster beam registers & interrupts
//Time to emulate that stuff correctly ;)

u32 in_vblank=0;
u32 clc_pvr_scanline;
u32 pvr_numscanlines=512;
u32 prv_cur_scanline=-1;
u32 vblk_cnt=0;

u32 last_fps=0;

volatile bool render_end_pending=false;
u32 render_end_pending_cycles;

//54 mhz pixel clock :)
#define PIXEL_CLOCK (54*1000*1000/2)
u32 Line_Cycles=0;
u32 Frame_Cycles=0;
void CalculateSync()
{
	//clc_pvr_scanline=0;

	u32 pixel_clock;
	float scale_x=1,scale_y=1;

	if (FB_R_CTRL.vclk_div)
	{
		//VGA :)
		pixel_clock=PIXEL_CLOCK;
	}
	else
	{
		//It is half for NTSC/PAL
		pixel_clock=PIXEL_CLOCK/2;
	}

	//We need to caclulate the pixel clock

//	u32 sync_cycles=(SPG_LOAD.hcount+1)*(SPG_LOAD.vcount+1);
	pvr_numscanlines=SPG_LOAD.vcount+1;
	
	Line_Cycles=(u32)((u64)DCclock*(u64)(SPG_LOAD.hcount+1)/(u64)pixel_clock);

	if (SPG_CONTROL.interlace)
	{
		//this is a temp hack
		Line_Cycles/=2;
//		u32 interl_mode=VO_CONTROL.field_mode;
		
		//if (interl_mode==2)//3 will be funny =P
		//	scale_y=0.5f;//single interlace
		//else
			scale_y=1;
	}
	else
	{
		if (FB_R_CTRL.vclk_div)
		{
			scale_y=1.0f;//non interlaced vga mode has full resolution :)
		}
		else
			scale_y=0.5f;//non interlaced modes have half resolution
	}

	SetFBScale(scale_x,scale_y);
	
	//Frame_Cycles=(u64)DCclock*(u64)sync_cycles/(u64)pixel_clock;
	
	Frame_Cycles=pvr_numscanlines*Line_Cycles;
}

extern u32 op_usage[0x10000];
extern u8* DynarecCache;
extern u32 DynarecCacheSize;

extern u64 time_dr_start;
extern u64 time_update_system;
extern u64 time_rw_regs;
extern u64 time_gdrom;
extern u64 time_lookup;
extern u64 time_pref;
extern u64 time_ta;
extern u32 gdromaccesses;

void PrintBlocksRunCount();

void spgVBL()
{
	u32 curtime=timeGetTime();
    
    //Vblank counter
	vblk_cnt++;
	params.RaiseInterrupt(holly_HBLank);// -> This turned out to be HBlank btw , needs to be emulated ;(
	//TODO : rend_if_VBlank();
	VBlank();//notify for vblank :)
	UpdateRRect();
	if ((curtime-last_fps)>500)
	{
		double spd_fps=(double)(FrameCount)/(double)((double)(curtime-(double)last_fps)/1000);
		double spd_vbs=(double)(vblk_cnt)/(double)((double)(curtime-(double)last_fps)/1000);
		double spd_cpu=spd_vbs*Frame_Cycles;
		spd_cpu/=1000000;	//mrhz kthx
		double fullvbs=(spd_vbs/spd_cpu)*200;
        
//		wchar fpsStr[256];
		const char* mode=0;
		const char* res=0;

		res=SPG_CONTROL.interlace?"480i":"240p";

		if (SPG_CONTROL.NTSC==0 && SPG_CONTROL.PAL==1)
			mode="PAL";
		else if (SPG_CONTROL.NTSC==1 && SPG_CONTROL.PAL==0)
			mode="NTSC";
		else
		{
			res=SPG_CONTROL.interlace?"480i":"480p";
			mode="VGA";
		}

		if(kbhit())
		{
			u32 i,j;
			u32 cumulcnt[0x200]={0};
			switch(getch())
			{
				case 'x':
					threaded_term();
					exit(0);
					break;
				case 'o': 
					printf("---- IFB op usage\n");

					for(i=0;i<0x10000;++i)
					{
						if (op_usage[i])
						{
							sh4_opcodelistentry* opentry=OpDesc[i];

							for(j=0;j<sizeof(opcodes)/sizeof(sh4_opcodelistentry);++j)
							{
								if (opentry==&opcodes[j])
								{
									cumulcnt[j]+=op_usage[i];
								}
							}
						}
					}

					for(i=0;i<sizeof(opcodes)/sizeof(sh4_opcodelistentry);++i)
					{
						if (cumulcnt[i])
						{
							printf("%12d\t%04x\t%s\n",cumulcnt[i],opcodes[i].rez,opcodes[i].disasm1);
						}
					}
					break;
				case 's': 
					printf("---- shil op usage\n");

					for(i=0;i<0x10000;++i)
					{
						if (op_usage[i])
						{
							printf("%12d\t%4d\n",op_usage[i],i);
						}
					}
					break;
				case 'z':
					printf("---- stats reset\n");
					
					time_dr_start=mftb();
					time_update_system=0;
					time_rw_regs=0;
					time_gdrom=0;
					time_lookup=0;
					time_pref=0;
					time_ta=0;
					
					for(i=0;i<0x10000;++i)
					{
						op_usage[i]=0;
					}
					break;
				case 'd':
				{
					printf("---- dumping dynarec mem pool @%08x\n",DynarecCache);
					FILE * f=fopen("uda:/nulldc-mempool.bin","wb");
					if(f){
						fwrite(DynarecCache,1,DynarecCacheSize,f);
						fclose(f);
					}
					break;
				}
				case 't':
					printf("UpdateSystem: %.3f%%\n",100.0f*(float)time_update_system/(mftb()-time_dr_start));
					printf("pref: %.3f%%\n",100.0f*(float)time_pref/(mftb()-time_dr_start));
					printf("RW regs: %.3f%%\n",100.0f*(float)time_rw_regs/(mftb()-time_dr_start));
					printf("GDROM: %.3f%%\n",100.0f*(float)time_gdrom/(mftb()-time_dr_start));
					printf("lookup: %.3f%%\n",100.0f*(float)time_lookup/(mftb()-time_dr_start));
					printf("ta wait: %.3f%%\n",100.0f*(float)time_ta/(mftb()-time_dr_start));
					break;
				case 'b':
					PrintBlocksRunCount();
					break;

			}
		}

		printf("%4.2f%% - VPS:%4.1f(%s%s%4.2f) RPS:%4.1f Sh4 %4.2fMhz Gdr %dKB/s\n", 
			spd_cpu*100/200,spd_vbs,
			mode,res,fullvbs,
			spd_fps,spd_cpu,(u32)((float)gdromaccesses/(curtime-last_fps)));
        
        gdromaccesses=0;
		VertexCount=0;
		last_fps=curtime;
		FrameCount=0;
		vblk_cnt=0;
	}
}

//called from sh4 context , should update pvr/ta state and everything else
void FASTCALL spgUpdatePvr(u32 cycles)
{
	if (Line_Cycles==0)
		return;
	clc_pvr_scanline += cycles;

	if (clc_pvr_scanline >  Line_Cycles)//60 ~herz = 200 mhz / 60=3333333.333 cycles per screen refresh
	{
		//ok .. here , after much effort , we did one line
		//now , we must check for raster beam interrupts and vblank
		prv_cur_scanline=(prv_cur_scanline+1)%pvr_numscanlines;
		clc_pvr_scanline -= Line_Cycles;
		//Check for scanline interrupts -- really need to test the scanline values
		
		if (SPG_VBLANK_INT.vblank_in_interrupt_line_number == prv_cur_scanline)
			params.RaiseInterrupt(holly_SCANINT1);

		if (SPG_VBLANK_INT.vblank_out_interrupt_line_number == prv_cur_scanline)
			params.RaiseInterrupt(holly_SCANINT2);

		if (SPG_VBLANK.vstart == prv_cur_scanline)
			in_vblank=1;

		if (SPG_VBLANK.vbend == prv_cur_scanline)
			in_vblank=0;

		if (SPG_CONTROL.interlace)
			SPG_STATUS.fieldnum=~SPG_STATUS.fieldnum;
		else
			SPG_STATUS.fieldnum=0;

		SPG_STATUS.vsync=in_vblank;
		SPG_STATUS.scanline=prv_cur_scanline;

		//Vblank start -- really need to test the scanline values
		if (prv_cur_scanline==0)
		{
			spgVBL();
		}
	}
	
	if (render_end_pending)
	{
		if (render_end_pending_cycles<cycles)
		{
            HandleLocks();
            threaded_call(EndRender);
		}
		render_end_pending_cycles-=cycles;
	}
}


bool spg_Init()
{
	return true;
}

void spg_Term()
{
}

void spg_Reset(bool Manual)
{
	CalculateSync();
}