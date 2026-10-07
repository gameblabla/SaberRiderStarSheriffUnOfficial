/* Local Ymir core runner: no GUI and no writes to emulator settings/ROMs. */
#include <ymir/ymir.hpp>
#include <ymir/sys/memory.hpp>
#include <ymir/hw/smpc/peripheral/peripheral_impl_control_pad.hpp>
#include <array>
#include <fstream>
#include <iostream>
#include <string>
#include <map>
#include <memory>
#include <cmath>
#include <filesystem>
#include <vector>
#include <sstream>
struct Capture {unsigned field=0;std::vector<unsigned> starts;std::string out;};
static void pad(ymir::peripheral::PeripheralReport &report,void *ctx) {auto &c=*static_cast<Capture*>(ctx);auto b=ymir::peripheral::Button::All;for(auto at:c.starts)if(at&&c.field>=at&&c.field<at+10)b&=~ymir::peripheral::Button::Start;report.report.controlPad.buttons=b;}
static void image(uint32_t *fb,uint32_t w,uint32_t h,void *ctx) {
 auto &c=*static_cast<Capture*>(ctx);if(c.field%300)return;
 std::ofstream f(c.out+"/f"+std::to_string(c.field)+".ppm",std::ios::binary);f<<"P6\n"<<w<<" "<<h<<"\n255\n";
 for(unsigned i=0;i<w*h;i++){char rgb[3]={(char)fb[i],(char)(fb[i]>>8),(char)(fb[i]>>16)};f.write(rgb,3);}
}
struct Audio{double sum=0;unsigned samples=0,peak=0;};
static void sample(int16_t l,int16_t r,void *ctx){auto &a=*static_cast<Audio*>(ctx);a.sum+=(double)l*l+(double)r*r;a.samples+=2;a.peak=std::max(a.peak,(unsigned)std::max(std::abs((int)l),std::abs((int)r)));}
int main(int argc,char **argv) {
 if(argc!=9){std::cerr<<"disc bios symbols out fields pal lle_rom start_field\n";return 2;}
 Capture c;c.out=argv[4];std::filesystem::create_directories(c.out);
 std::array<uint8_t,ymir::sys::kIPLSize> ipl{};std::ifstream bios(argv[2],std::ios::binary);bios.read((char*)ipl.data(),ipl.size());if(!bios)return 3;
 auto sat=std::make_unique<ymir::Saturn>();sat->LoadIPL(ipl);
 if(std::string(argv[7])!="-"){std::array<uint8_t,ymir::sh1::kROMSize> rom{};std::ifstream r(argv[7],std::ios::binary);r.read((char*)rom.data(),rom.size());if(!r)return 4;sat->LoadCDBlockROM(rom);sat->configuration.cdblock.useLLE=true;}
 ymir::smpc::PersistentSMPCData persistent{};persistent.STE=true;sat->SMPC.LoadPersistentData(persistent);
 Audio a;sat->SCSP.SetSampleCallback({&a,sample});sat->VDP.UseSoftwareRenderer();sat->VDP.SetSoftwareRenderCallback({&c,image});
 ymir::media::Disc disc{};if(!ymir::media::LoadDisc(argv[1],disc,false,[](auto,auto msg){std::cerr<<msg<<"\n";}))return 5;sat->LoadDisc(std::move(disc));sat->AutodetectRegion();
 if(std::stoi(argv[6]))sat->SetVideoStandard(ymir::core::config::sys::VideoStandard::PAL);
 std::map<std::string,uint32_t> symbols;std::ifstream syms(argv[3]);uint32_t address;std::string type,name;
 std::string symbol_line;
 while(std::getline(syms,symbol_line)){std::istringstream row(symbol_line);if(row>>std::hex>>address>>type>>name)symbols[name]=address;}
 auto addr=[&](std::string n)->uint32_t {
  for(auto key:{"_"+n,n}) {
   auto it=symbols.lower_bound(key);
   if(it!=symbols.end()&&(it->first==key||it->first.starts_with(key+".")))return it->second;
  }
  return 0;
 };
 auto word=[&](std::string n){auto at=addr(n);return at?sat->mainBus.Peek<uint32_t>(at):0;};
 std::ofstream metrics(c.out+"/metrics.tsv");metrics<<"field\tframes\tdrops\terrors\tstatus\tstage\tvid_state\tvid_ud\tdecode_us\tpresent_us\tread_us\trms\tpeak\n";
 std::ofstream log(c.out+"/game.log");unsigned seen=0,frames=std::stoi(argv[5]);
 std::stringstream buttons(argv[8]);std::string press;while(std::getline(buttons,press,','))c.starts.push_back(std::stoi(press));auto &port=sat->SMPC.GetPeripheralPort1();port.SetPeripheralReportCallback({&c,pad});port.ConnectControlPad();
 for(c.field=1;c.field<=frames;c.field++) {
  sat->RunFrame();
  uint32_t la=addr("saber_log");if(la&&sat->mainBus.Peek<uint32_t>(la)==0x53414245){auto size=sat->mainBus.Peek<uint32_t>(la+8),head=sat->mainBus.Peek<uint32_t>(la+12);if(size&&size<=65536){if(head-seen>size)seen=head-size;if(seen<head)log<<"[field "<<c.field<<"] ";while(seen<head)log<<(char)sat->mainBus.Peek<uint8_t>(la+16+(seen++)%size);log.flush();}}
  if(c.field%60==0){metrics<<c.field<<'\t'<<(word("sat_movie_frames")+word("sv24_frames_decoded"))<<'\t'<<(word("sat_movie_drops")+word("sv24_video_drops"))<<'\t'<<(word("sat_movie_errors")+word("sv24_cd_errors")+word("sv24_decode_errors"))<<'\t'<<std::hex<<word("sv24_status")<<'\t'<<word("sv24_stage")<<'\t'<<word("vid_state")<<'\t'<<word("vid_ud")<<std::dec<<'\t'<<word("sat_movie_decode_us")<<'\t'<<word("sat_movie_present_us")<<'\t'<<word("sat_movie_read_us")<<'\t'<<std::sqrt(a.sum/std::max(1u,a.samples))<<'\t'<<a.peak<<'\n';metrics.flush();a={};}
 }
 std::ofstream registers(c.out+"/vdp2-registers.bin",std::ios::binary);
 std::ofstream cpu(c.out+"/cpu.txt");
 cpu<<std::hex<<"master PC "<<sat->masterSH2.GetProbe().PC()<<" slave PC "<<sat->slaveSH2.GetProbe().PC()<<'\n';
 for(unsigned i=0;i<0x120;i++){char b=(char)sat->mainBus.Peek<uint8_t>(0x25f80000u+i);registers.write(&b,1);}
 std::ofstream vram(c.out+"/vdp2-vram.bin",std::ios::binary);
 for(unsigned i=0;i<0x80000;i++){char b=(char)sat->mainBus.Peek<uint8_t>(0x25e00000u+i);vram.write(&b,1);}
 std::cout<<"completed "<<frames<<" fields; movie frames="<<(word("sat_movie_frames")+word("sv24_frames_decoded"))<<" drops="<<(word("sat_movie_drops")+word("sv24_video_drops"))<<" errors="<<(word("sat_movie_errors")+word("sv24_cd_errors")+word("sv24_decode_errors"))<<"\n";
}
