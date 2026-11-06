#include "right_stick.h"
#include "input_mode.h"
#include "protocol.h"
#include "ps5.h"
#include "switch_pro.h"
#include "usb.h"
#include <Adafruit_TinyUSB.h>
#include <device/dcd.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace {
unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"SWIPE FAILED %s:%d: %s\n",__FILE__,__LINE__,#x); std::exit(1); } } while (0)
void put16(uint8_t *p, int v) { p[0] = uint8_t(v); p[1] = uint8_t(uint16_t(v) >> 8); }
void put32(uint8_t *p, uint32_t v) { for (unsigned i=0;i<4;++i) p[i]=uint8_t(v>>(8*i)); }
int16_t get16(const uint8_t *p) { return int16_t(uint16_t(p[0]) | uint16_t(p[1])<<8); }
void testVelocity()
{
	auto s = right_stick::defaults(); s.mode=1; s.smoothing_ms=0; s.noise_speed=0;
	for (uint32_t interval : {4000u,8000u,16000u}) {
		right_stick::Mapper m; m.observe(true,20000,1000,100000,120000,s);
		CHECK(m.axes(120000,s).x == 0);
		m.observe(true,int16_t(20000-int(interval/10)),1000,100000+interval,120000,s);
		auto out=m.axes(120000,s); CHECK(std::abs(int(out.x)+16384)<=1 && out.y==0);
		// USB submissions do not consume or recalculate the same delta.
		CHECK(m.axes(120000,s).x==out.x);
		m.observe(false,0,0,120001,120001,s); CHECK(m.axes(120001,s).x==0);
		m.observe(true,-25000,25000,120002,120002,s); CHECK(m.axes(120002,s).x==0);
	}
	for (uint16_t exponent : {uint16_t(50),uint16_t(100),uint16_t(150),uint16_t(200),uint16_t(300)}) {
		s.exponent=exponent;
		for (int displacement : {40,200,400,800,1600}) {
			right_stick::Mapper m; m.observe(true,0,0,0,0,s); m.observe(true,int16_t(displacement),0,4000,4000,s);
			double r=std::fmin(displacement/800.0,1.0), expected=32767*std::pow(r,exponent/100.0);
			CHECK(std::abs(m.axes(4000,s).x-expected)<2);
		}
	}
	s.exponent=100; s.anti_deadzone=200;
	right_stick::Mapper m; m.observe(true,0,0,0,0,s); CHECK(m.axes(0,s).x==0);
	m.observe(true,400,0,4000,4000,s); CHECK(std::abs(m.axes(4000,s).x-19660)<2);
	m.observe(true,400,0,8000,8000,s); CHECK(m.axes(8000,s).x==0);
	m.observe(true,800,0,12000,12000,s); CHECK(m.axes(62001,s).x==0);
	m.reset(); m.observe(true,0,0,0xfffffff0u,0xfffffff0u,s);
	m.observe(true,400,0,3984,3984,s); CHECK(std::abs(m.axes(3984,s).x-19660)<2);
	m.reset(); m.observe(true,0,0,1000,1000,s); m.observe(true,400,0,1000,1000,s);
	CHECK(m.axes(1000,s).x==0); m.observe(true,800,0,5000,5000,s); CHECK(m.axes(5000,s).x==32767);
	m.observe(true,-30000,0,60000,60000,s); CHECK(m.axes(60000,s).x==0); // long gap re-anchors
	m.observe(true,0,0,64000,120000,s); CHECK(m.axes(120000,s).x==0); // queued stale sample
	s.anti_deadzone=0; s.smoothing_ms=10; m.reset();
	m.observe(true,0,0,0,0,s); m.observe(true,400,0,4000,4000,s);
	CHECK(std::abs(m.axes(4000,s).x - 16383.5*(1-std::exp(-0.4)))<2);
	s.noise_speed=2; m.reset(); m.observe(true,0,0,0,0,s); m.observe(true,4,0,4000,4000,s); CHECK(m.axes(4000,s).x==0);
	s.smoothing_ms=0; s.noise_speed=0; m.reset(); m.observe(true,-32768,-32768,0,0,s);
	m.observe(true,32767,32767,4000,4000,s); auto diagonal=m.axes(4000,s);
	CHECK(std::abs(diagonal.x-23170)<2 && diagonal.x==diagonal.y);
}
void testProfilesAndMigration()
{
	using namespace input_mode;
	auto c=defaults(); c.bindings[0]=B; c.ps5.bindings[0]=X; c.switch_pro.bindings[0]=Y;
	c.ps5.right_stick.mode=1; c.ps5.right_stick.exponent=200; c.switch_pro.right_stick.mode=1;
	c.right_haptic_us=777; c.left_haptic_us=888; c.flags|=PERSIST_MODE;
	uint8_t record[RECORD_SIZE]; encodeRecord(c,91,record,{Mode::Switch,3}); Config decoded; uint32_t generation; BootState boot;
	CHECK(decodeRecord(record,decoded,generation,&boot) && generation==91 && boot.once==3);
	CHECK(decoded.bindings[0]==B && decoded.ps5.bindings[0]==X && decoded.switch_pro.bindings[0]==Y);
	CHECK(decoded.ps5.right_stick.exponent==200 && decoded.right_haptic_us==777 && decoded.left_haptic_us==888);
	CHECK(profileToken(c,Mode::Xbox)!=profileToken(c,Mode::PS5));
	for (unsigned i=0;i<RECORD_SIZE;++i) { record[i]^=1; CHECK(!decodeRecord(record,decoded,generation)); record[i]^=1; }
	// Frozen legacy layouts have independent buffers: v1/v2 are only 48 bytes.
	for (uint32_t version : {1u,2u,3u}) {
		uint8_t old[56]={}; memcpy(old,"SCXM",4); put32(old+4,version); put32(old+8,7);
		encodeConfig(c,old+12); if(version==1) old[13]&=7;
		if(version>=2) { old[36]=2; old[37]=255; }
		if(version==3) { put16(old+38,300); put16(old+40,500); }
		unsigned crc=version==3?48:40; put32(old+crc,sc::crc32(old,crc));
		CHECK(decodeRecord(old,decoded,generation,&boot));
		CHECK(decoded.bindings[0]==B && decoded.ps5.bindings[0]==B && decoded.switch_pro.bindings[0]==B);
		CHECK(decoded.right_stick.mode==0 && decoded.ps5.right_stick.mode==0 && decoded.switch_pro.right_stick.mode==0);
		CHECK(decoded.right_haptic_us==(version==3?300:100));
	}
	uint8_t raw[64]={1,0,1,60}, mapped[20]; raw[8]=0x80;
	for(auto mode : {Mode::Xbox,Mode::PS5,Mode::Switch}) {
		Router router; router.configure(c); router.changeMode(mode); router.observe(0,1,true,raw,1);
		router.xboxReport(1,mapped); CHECK(uint16_t(get16(mapped+2))==(mode==Mode::Xbox?0x2000:mode==Mode::PS5?0x4000:0x8000));
		Config enabled=defaults(); auto &p=profile(enabled,mode); p.right_stick={1,100,200,0,0,0};
		router.configure(enabled); raw[8]=0; raw[10]=0x10; put16(raw+20,20000);
		uint32_t sample=100000; router.observe(0,2,true,raw,112000,true,0,&sample);
		put16(raw+20,19200); sample=108000; router.observe(0,2,true,raw,112000,true,0,&sample);
		router.xboxReport(112000,mapped); CHECK(std::abs(int(get16(mapped+10))+16384)<=1);
		if(mode==Mode::PS5) { uint8_t out[63]; ps5::buildReport(mapped,112000,out); CHECK(out[2]==64 && out[3]==128); }
		if(mode==Mode::Switch) {
			switch_pro::enable(true,1,2); uint8_t command[11]={}; command[9]=3; command[10]=0x30;
			switch_pro::setReport(1,HID_REPORT_TYPE_OUTPUT,command,11); switch_pro::Report out;
			CHECK(switch_pro::nextReport(mapped,112000,out)); switch_pro::sent(out,112000);
			CHECK(switch_pro::nextReport(mapped,112000,out) && out.id==0x30);
			unsigned x=out.data[8] | ((out.data[9]&15)<<8); CHECK(x>=1147 && x<=1148); switch_pro::enable(false,0,0);
		}
		router.clearMotion(); router.xboxReport(112001,mapped); CHECK(get16(mapped+10)==0);
		raw[8]=0x80; raw[10]=0;
	}
}
bool aes(const uint8_t*,const uint8_t*,uint8_t*) { return false; }
bool rng(uint8_t *p,size_t n) { memset(p,7,n); return true; }
bool bonds(const sc::Database&) { return true; }
input_mode::Config saved;
bool fail_save;
unsigned saves;
bool persist(const input_mode::Config &c) { ++saves; if(fail_save) return false; saved=c; return true; }
void testFeatureIsolation()
{
	sc::Receiver receiver; sc::Database db{};
	CHECK(receiver.begin(db,{aes,rng,bonds,usb::input,usb::desktop}));
	usb::configureBoot(input_mode::Mode::Steam,nullptr,nullptr); saved=input_mode::defaults(); usb::configureInput(saved,persist);
	CHECK(usb::begin()); tud_event_hook_cb(0,DCD_EVENT_BUS_RESET,true); usb::process(receiver,16);
	auto request=[&](const uint8_t *data,unsigned n,uint8_t out[64]) {
		CHECK(mockFeatureSet(1,data,n,n)); usb::process(receiver,16); usb::publish(receiver);
		CHECK(Adafruit_USBD_HID::instances[1]->get(0,HID_REPORT_TYPE_FEATURE,out,64)==64);
		usb::process(receiver,16);
	};
	uint8_t read[]={0xfe,2,3,2}, reply[64], save[44]={0xfe,42,4,2}; request(read,4,reply);
	CHECK(reply[2]==2 && reply[6]==2 && reply[5]==36);
	memcpy(save+4,reply+8,4); memcpy(save+8,reply+12,36);
	save[10]=input_mode::Y; save[32]=1; put16(save+34,150);
	request(save,44,reply); CHECK(!reply[4] && saved.ps5.bindings[0]==input_mode::Y && saved.bindings[0]==input_mode::A);
	CHECK(saved.ps5.right_stick.mode==1 && saved.ps5.right_stick.exponent==150);
	request(save,44,reply); CHECK(reply[4]==2); // stale profile token
	read[3]=3; request(read,4,reply); CHECK(reply[14]==input_mode::A && reply[36]==0);
	save[3]=3; request(save,44,reply); CHECK(reply[4]==2); // another profile's token cannot authorize this one
	request(read,4,reply); memcpy(save+4,reply+8,4); memcpy(save+8,reply+12,36); save[10]=input_mode::B;
	fail_save=true; request(save,44,reply); CHECK(reply[4]==6 && saved.switch_pro.bindings[0]==input_mode::A); fail_save=false;
	// A legacy Xbox save and a global haptic save preserve the PS5 profile.
	uint8_t legacy[]={0xfe,0}; request(legacy,2,reply); uint8_t old[31]={0xfe,29,1};
	memcpy(old+3,reply+8,4); memcpy(old+7,reply+12,24); old[9]=input_mode::B;
	request(old,31,reply); CHECK(!reply[4] && saved.bindings[0]==input_mode::B && saved.ps5.bindings[0]==input_mode::Y && saved.ps5.right_stick.mode==1);
	uint8_t touch[11]={0xfe,9,2}; memcpy(touch+3,reply+42,4); put16(touch+7,444); put16(touch+9,555);
	request(touch,11,reply); CHECK(!reply[4] && saved.right_haptic_us==444 && saved.ps5.right_stick.exponent==150);
	// Cold-start model reloads all profiles from the encoded persistent record.
	uint8_t record[input_mode::RECORD_SIZE]; input_mode::encodeRecord(saved,19,record); input_mode::Config loaded; uint32_t generation;
	CHECK(input_mode::decodeRecord(record,loaded,generation)); usb::configureInput(loaded,persist);
	read[3]=2; request(read,4,reply); CHECK(reply[14]==input_mode::Y && reply[36]==1);
	tud_event_hook_cb(0,DCD_EVENT_BUS_RESET,true); usb::process(receiver,16);
}
}
void testRightStick()
{
	testVelocity(); testProfilesAndMigration(); testFeatureIsolation();
	std::printf("PASS: swipe velocity, curves, timestamps, independent profiles, legacy migration and FE transactions (%u assertions)\n",checks);
}
