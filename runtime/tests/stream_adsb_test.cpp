#include "../esp32/stream_adsb.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
using namespace flightwall;
struct Source {
    const std::string &data;size_t at=0,fail_at=static_cast<size_t>(-1);
    static int read(void *p){auto &s=*static_cast<Source *>(p);return s.at==s.fail_at?-2:s.at<s.data.size()?static_cast<unsigned char>(s.data[s.at++]):-1;}
};
int main(){
    DynamicJsonDocument scratch(4096);Batch out{},reference{};char row[4096];
    auto run=[&](const std::string &s,size_t fail=static_cast<size_t>(-1),size_t cap=4096){
        Source input{s,0,fail};return decode_adsb_stream({&input,Source::read},scratch,row,cap,out);
    };
    const std::string aircraft=R"({"hex":"abc123","flight":"DAL42 ","lat":26,"lon":-80,"seen_pos":0,"gs":300,"alt_baro":10000,"track":0,"baro_rate":600})";
    const std::string good="{\"now\":1,\"ignored\":{\"stuff\":[true,null,{},-2.3e-4]},\"ac\":["+aircraft+"],\"total\":1}";
    assert(run(good)==Result::Ok&&out.count==1&&scratch.isNull());
    assert(decode(Provider::AdsbFi,Encoding::Json,reinterpret_cast<const uint8_t *>(good.data()),good.size(),scratch,reference)==Result::Ok);
    assert(out.count==reference.count&&out.flights[0].altitude.value==reference.flights[0].altitude.value);
    assert(!std::strcmp(out.flights[0].callsign,"DAL42")&&out.flights[0].heading.valid&&out.flights[0].heading.value==0);
    for(size_t n=0;n<good.size();++n){
        assert(run(good.substr(0,n))!=Result::Ok&&out.count==0);
        assert(run(good,n)!=Result::Ok&&out.count==0);
    }
    assert(run("{\"aircraft\":[]}")==Result::Ok&&out.count==0);
    assert(run("{\"\\u0061c\":["+aircraft+"]}")==Result::Ok&&out.count==1);
    assert(run("{\"ac\":["+aircraft+","+aircraft+",null,{}]}")==Result::Ok&&out.count==1&&out.duplicates==1&&out.rejected==2);
    for(const auto bad:{"{}","[]","{\"ac\":null}","{\"ac\":[],\"aircraft\":[]}","{\"ac\":[]}x","{\"ac\":[{}],}",
                        "{\"ac\":[],\"x\":01}","{\"ac\":[],\"x\":1.}","{\"ac\":[],\"x\":tru}",
                        "{\"ac\":[],\"x\":\"bad\\q\"}","{\"ac\":[],\"x\":\"bad\\u123X\"}"})
        assert(run(bad)!=Result::Ok&&out.count==0);
    assert(run(good,static_cast<size_t>(-1),16)==Result::TooLarge&&out.count==0);
    StaticJsonDocument<1> tiny;Source source{good};
    assert(decode_adsb_stream({&source,Source::read},tiny,row,sizeof row,out)==Result::NoMemory&&out.count==0);
    std::string large="{\"ac\":[";
    for(unsigned i=0;i<150;++i){
        if(i)large+=',';
        char id[7];std::snprintf(id,sizeof id,"%06x",i);
        large+="{\"hex\":\""+std::string(id)+"\",\"lat\":26,\"lon\":-80,\"seen_pos\":0}";
    }
    large+="]}";assert(large.size()>4096);
    assert(run(large)==Result::Ok&&out.count==MaxFlights&&out.overflow);
    const std::string unknown_large="{\"ac\":[],\"metadata\":\""+std::string(64000,'x')+"\"}";
    assert(run(unknown_large)==Result::Ok&&out.count==0);
    assert(run("{\"ac\":[],\"metadata\":\""+std::string(MaxResponseBytes,'x')+"\"}")==Result::TooLarge&&out.count==0);
    std::string deep="{\"ac\":[],\"x\":";for(int i=0;i<15;++i)deep+='[';deep+='0';for(int i=0;i<15;++i)deep+=']';deep+='}';
    assert(run(deep)==Result::Invalid&&out.count==0);
    std::cout<<"Streaming ADS-B: one row at a time, 150 aircraft in 4 KiB scratch, complete envelope, truncation/IO/size/depth limits\n";
}
