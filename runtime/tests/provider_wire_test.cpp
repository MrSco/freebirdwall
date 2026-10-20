#include "../esp32/provider_wire.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <iostream>
using namespace flightwall;
namespace {
bool exists(void *,const char *path) {
    return !std::strcmp(path,"/images/airlines/square/DAL.png")||
        !std::strcmp(path,"/images/airlines/square/AAL.png")||
        !std::strcmp(path,"/images/airlines/square/default.png");
}
struct Output { size_t calls=0;bool succeed=true; };
bool decode_stock(const uint8_t *wire,size_t length,void *ptr) {
    auto &out=*static_cast<Output *>(ptr);++out.calls;
    DynamicJsonDocument doc(4096);
    assert(deserializeMsgPack(doc,wire,length)==DeserializationError::Ok);
    assert(doc["v"]==1&&doc["f"].size()<=StockMaxFlights);
    assert(doc["ov"].is<unsigned>()&&!doc["ov"].is<bool>());
    assert(!doc.containsKey("ch")&&!doc.containsKey("tf")&&!doc.containsKey("slu"));
    return out.succeed;
}
}
int main() {
    Batch batch{};batch.provider=Provider::AdsbFi;batch.count=2;
    for(size_t i=0;i<batch.count;++i) {
        auto &f=batch.flights[i];f.provider=batch.provider;
        std::strcpy(f.aircraft_id,i?"DEF123":"ABC123");
        std::strcpy(f.callsign,i?"AAL123":"DAL42");
        f.altitude={3048,Unit::Metres,true};f.speed={0,Unit::MetresPerSecond,true};
        f.heading={0,Unit::Degrees,true};f.vertical_rate={-3.048,Unit::MetresPerSecond,true};
        f.has_ground_state=true;f.on_ground=false;
    }
    std::strcpy(batch.flights[0].origin.iata,"DJT");
    std::strcpy(batch.flights[0].origin.name,"Donald J Trump Intl");
    LogoCatalog catalog{nullptr,exists,nullptr,0};
    DynamicJsonDocument scratch(8192),decoded(8192);uint8_t wire[8192];size_t length=0;
    assert(encode_stock_frame(batch,catalog,scratch,wire,sizeof wire,length)==Result::Ok);
    assert(scratch.isNull()&&length>0);
    assert(deserializeMsgPack(decoded,wire,length)==DeserializationError::Ok);
    auto rows=decoded["f"].as<JsonArrayConst>();
    assert(decoded["v"]==1&&rows.size()==2&&!decoded["ov"].as<bool>());
    assert(decoded["ov"].is<unsigned>()&&!decoded["ov"].is<bool>());
    assert(!std::strcmp(rows[0]["al_code"],"DAL")&&!std::strcmp(rows[1]["al_code"],"AAL"));
    assert(!std::strcmp(rows[0]["orig_iata"],"DJT")); // Existing display hook applies owner's aliases.
    assert(rows[0]["alt_b"]==3048&&rows[0]["vel"]==0&&rows[0]["trk"]==0);
    assert(std::abs(rows[0]["vr"].as<double>()+3.048)<1e-9&&!rows[0]["gnd"].as<bool>());
    // Unknown units, missing values and non-finite numbers never become wire metrics.
    batch.flights[0].altitude.unit=Unit::VendorUnverified;
    batch.flights[0].speed.valid=false;
    batch.flights[0].heading.value=std::numeric_limits<double>::quiet_NaN();
    assert(encode_stock_frame(batch,catalog,scratch,wire,sizeof wire,length)==Result::Ok);
    assert(deserializeMsgPack(decoded,wire,length)==DeserializationError::Ok);
    assert(!decoded["f"][0].containsKey("alt_b")&&!decoded["f"][0].containsKey("vel")&&!decoded["f"][0].containsKey("trk"));
    Output output{};
    assert(deliver_stock_frame(batch,catalog,scratch,wire,sizeof wire,&output,decode_stock)&&output.calls==1);
    output.succeed=false;
    assert(!deliver_stock_frame(batch,catalog,scratch,wire,sizeof wire,&output,decode_stock)&&output.calls==2);
    assert(!deliver_stock_frame(batch,catalog,scratch,wire,1,&output,decode_stock)&&output.calls==2);
    assert(!deliver_stock_frame(batch,catalog,scratch,wire,sizeof wire,nullptr,decode_stock));
    StaticJsonDocument<1> tiny;
    assert(encode_stock_frame(batch,catalog,tiny,wire,sizeof wire,length)==Result::NoMemory&&length==0);
    assert(encode_stock_frame(batch,catalog,scratch,wire,1,length)==Result::TooLarge&&length==0);
    batch.count=6;
    for(size_t i=2;i<batch.count;++i)batch.flights[i]=batch.flights[1];
    assert(encode_stock_frame(batch,catalog,scratch,wire,sizeof wire,length)==Result::Ok);
    assert(deserializeMsgPack(decoded,wire,length)==DeserializationError::Ok);
    assert(decoded["f"].size()==5&&decoded["ov"].as<bool>());
    batch.count=MaxFlights+1;
    assert(encode_stock_frame(batch,catalog,scratch,wire,sizeof wire,length)==Result::Invalid&&length==0);
    batch.count=1;std::memset(batch.flights[0].callsign,'A',sizeof batch.flights[0].callsign);
    assert(encode_stock_frame(batch,catalog,scratch,wire,sizeof wire,length)==Result::Invalid&&length==0);
    batch.count=0;
    assert(encode_stock_frame(batch,catalog,scratch,wire,sizeof wire,length)==Result::Ok);
    assert(deserializeMsgPack(decoded,wire,length)==DeserializationError::Ok&&decoded["f"].size()==0);
    std::cout<<"Stock v1 wire: SI, five-row bound, independent airline logos, no control metadata, failures not delivered\n";
}
