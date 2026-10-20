#include "flight_data.h"
#include <cmath>
#include <cstring>

namespace flightwall {
namespace {
bool space(unsigned char c) { return c==' ' || c=='\t' || c=='\r' || c=='\n'; }
bool code(const char *in, char *out, size_t length, bool digits=false) {
    out[0]=0;
    if (!in) return false;
    while (space(*in)) ++in;
    size_t n=std::strlen(in); while(n && space(in[n-1])) --n;
    if(n!=length) return false;
    for(size_t i=0;i<n;++i){
        unsigned char c=in[i]; if(c>='a'&&c<='z') c-=32;
        if(!((c>='A'&&c<='Z') || (digits&&c>='0'&&c<='9'))) {out[0]=0;return false;}
        out[i]=static_cast<char>(c);
    }
    out[n]=0;return true;
}
bool text(JsonVariantConst v, char *out, size_t cap, bool upper=false) {
    out[0]=0;if(!v.is<const char*>())return false;
    const auto s=v.as<JsonString>();const auto *p=reinterpret_cast<const unsigned char*>(s.c_str());
    size_t n=s.size(),start=0;while(start<n&&space(p[start]))++start;while(n>start&&space(p[n-1]))--n;
    size_t at=0;
    for(size_t i=start;i<n;){
        unsigned char c=p[i];size_t w=c<128?1:c>=0xc2&&c<=0xdf?2:c>=0xe0&&c<=0xef?3:c>=0xf0&&c<=0xf4?4:0;
        if(!w||i+w>n||c<32||c==127){out[0]=0;return false;}
        for(size_t j=1;j<w;++j)if((p[i+j]&0xc0)!=0x80){out[0]=0;return false;}
        if((w==3&&((c==0xe0&&p[i+1]<0xa0)||(c==0xed&&p[i+1]>=0xa0)))||
           (w==4&&((c==0xf0&&p[i+1]<0x90)||(c==0xf4&&p[i+1]>=0x90)))){out[0]=0;return false;}
        if(at+w>=cap)break;
        for(size_t j=0;j<w;++j){unsigned char x=p[i+j];if(upper&&x>='a'&&x<='z')x-=32;out[at++]=static_cast<char>(x);}
        i+=w;
    }
    out[at]=0;return at>0;
}
bool number(JsonVariantConst v,double &n) {
    if(v.is<bool>()||!v.is<double>())return false;
    n=v.as<double>();return std::isfinite(n);
}
Metric metric(JsonVariantConst v, Unit u, double scale=1) {
    Metric m{};m.unit=u;m.valid=number(v,m.value);if(m.valid)m.value*=scale;return m;
}
bool identifier(JsonVariantConst v,char out[8]) {
    char raw[16];if(!text(v,raw,sizeof raw,true))return false;
    size_t n=std::strlen(raw),start=raw[0]=='~'?1:0;
    if(n!=6+start)return false;
    for(size_t i=start;i<n;++i)if(!((raw[i]>='0'&&raw[i]<='9')||(raw[i]>='A'&&raw[i]<='F')))return false;
    std::memcpy(out,raw,n+1);return true;
}
void airport(JsonVariantConst o,Airport &a){
    char b[12];text(o["iata_code"],b,sizeof b);code(b,a.iata,3);
    text(o["icao_code"],b,sizeof b);code(b,a.icao,4);
    text(o["name"],a.name,sizeof a.name);
}
bool has_airport(const Airport &a){return a.iata[0]||a.icao[0]||a.name[0];}
void vendor_airport(JsonVariantConst row,const char *name,const char *iata,const char *icao,Airport &a){
    char b[12];text(row[name],a.name,sizeof a.name);
    text(row[iata],b,sizeof b);code(b,a.iata,3);
    text(row[icao],b,sizeof b);code(b,a.icao,4);
}
bool vendor(JsonVariantConst row,Flight &f){
    if(!identifier(row["icao"],f.aircraft_id))return false;
    text(row["cs"],f.callsign,sizeof f.callsign,true);text(row["reg"],f.registration,sizeof f.registration,true);
    text(row["ac_type"],f.aircraft_type,sizeof f.aircraft_type,true);text(row["ac_name"],f.aircraft_name,sizeof f.aircraft_name);
    text(row["al_name"],f.airline.name,sizeof f.airline.name);
    text(row["al_code"],f.airline.vendor_code,sizeof f.airline.vendor_code,true);
    text(row["al_paint"],f.airline.vendor_paint,sizeof f.airline.vendor_paint,true);
    text(row["al_op"],f.airline.vendor_operator,sizeof f.airline.vendor_operator,true);
    // al_code is the renderer's candidate, not proven operating-carrier identity.
    vendor_airport(row,"orig","orig_iata","orig_icao",f.origin);
    vendor_airport(row,"dest","dest_iata","dest_icao",f.destination);
    if(has_airport(f.origin)||has_airport(f.destination))f.route_source=RouteSource::Vendor;
    char category[16];text(row["ac_class"],category,sizeof category);
    f.category=!std::strcmp(category,"Heli")?Category::Helicopter:!std::strcmp(category,"Light")?Category::Light:
        !std::strcmp(category,"Smalljet")?Category::SmallJet:Category::Unknown;
    // Proven against the stock numeric parser and metric display conversions.
    f.altitude=metric(row["alt_b"],Unit::Metres);f.speed=metric(row["vel"],Unit::MetresPerSecond);
    f.vertical_rate=metric(row["vr"],Unit::MetresPerSecond);f.heading=metric(row["trk"],Unit::Degrees);
    if(row["gnd"].is<bool>()){f.has_ground_state=true;f.on_ground=row["gnd"].as<bool>();}
    return true;
}
bool adsb(JsonVariantConst row,Flight &f,const ParseOptions &options){
    if(!identifier(row["hex"],f.aircraft_id))return false;
    double lat=0,lon=0,age=0;
    if(!number(row["lat"],lat)||!number(row["lon"],lon)||lat< -90||lat>90||lon< -180||lon>180)return false;
    if(!number(row["seen_pos"],age)||age<0||age>options.max_position_age_seconds)return false;
    f.has_position=true;f.latitude=lat;f.longitude=lon;f.has_position_age=true;f.position_age_seconds=age;
    text(row["flight"],f.callsign,sizeof f.callsign,true);text(row["r"],f.registration,sizeof f.registration,true);
    text(row["t"],f.aircraft_type,sizeof f.aircraft_type,true);text(row["desc"],f.aircraft_name,sizeof f.aircraft_name);
    f.altitude=metric(row["alt_baro"],Unit::Metres,0.3048);f.speed=metric(row["gs"],Unit::MetresPerSecond,1852.0/3600);
    f.heading=metric(row["track"],Unit::Degrees);f.vertical_rate=metric(row["baro_rate"],Unit::MetresPerSecond,0.3048/60);
    if(row["alt_baro"].is<const char*>()&&!std::strcmp(row["alt_baro"].as<const char*>(),"ground")){
        f.has_ground_state=true;f.on_ground=true;f.altitude.valid=false;
    }else if(f.altitude.valid){f.has_ground_state=true;f.on_ground=false;}
    // ADS-B emitter category A7 explicitly identifies rotorcraft. Other aircraft
    // categories don't establish the vendor's Light/Smalljet classifications.
    if(row["category"].is<const char*>()&&!std::strcmp(row["category"].as<const char*>(),"A7"))f.category=Category::Helicopter;
    return true;
}
bool json_envelope(const uint8_t *p,size_t n){
    size_t at=0;while(at<n&&space(p[at]))++at;if(at==n||p[at]!='{')return false;
    unsigned depth=0;bool quoted=false,escape=false;
    for(;at<n;++at){unsigned char c=p[at];if(quoted){if(escape)escape=false;else if(c=='\\')escape=true;else if(c=='"')quoted=false;continue;}
        if(c=='"')quoted=true;else if(c=='{'||c=='['){if(++depth>12)return false;}
        else if(c=='}'||c==']'){if(!depth)return false;if(!--depth){++at;while(at<n&&space(p[at]))++at;return at==n;}}
    }return false;
}
struct Reader {
    const uint8_t *p;size_t n,at=0;
    int read(){return at<n?p[at++]:-1;}
    size_t readBytes(char *out,size_t count){if(count>n-at)count=n-at;std::memcpy(out,p+at,count);at+=count;return count;}
};
bool exists(const LogoCatalog &c,const char *path){return c.exists&&c.exists(c.user,path);}
bool logo_path(const LogoCatalog &catalog,const char *prefix,const char *key,Logo &out,LogoKind kind){
    const size_t p=std::strlen(prefix),k=std::strlen(key);if(p+k+5>sizeof out.path)return false;
    char path[64];std::memcpy(path,prefix,p);std::memcpy(path+p,key,k);std::memcpy(path+p+k,".png",5);
    if(!exists(catalog,path))return false;
    std::strcpy(out.path,path);out.kind=kind;return true;
}
} // namespace

bool json_object_envelope(const uint8_t *p,size_t n){return p&&n&&n<=MaxResponseBytes&&json_envelope(p,n);}

Result append_adsb_aircraft(JsonVariantConst row,Batch &out,const ParseOptions &options){
    if(out.provider!=Provider::AdsbFi||out.count>MaxFlights||
       !std::isfinite(options.max_position_age_seconds)||options.max_position_age_seconds<0)return Result::Invalid;
    Flight candidate{};candidate.provider=Provider::AdsbFi;
    if(!row.is<JsonObjectConst>()||!adsb(row,candidate,options)){++out.rejected;return Result::Ok;}
    if(options.allow&&!options.allow(options.selection_user,candidate))return Result::Ok;
    for(size_t i=0;i<out.count;++i)if(!std::strcmp(out.flights[i].aircraft_id,candidate.aircraft_id)){
        ++out.duplicates;return Result::Ok;
    }
    size_t slot=out.count;
    if(out.count==MaxFlights){
        out.overflow=true;
        if(!options.rank)return Result::Ok;
        slot=0;
        for(size_t i=1;i<out.count;++i)if(options.rank(options.selection_user,out.flights[i])>options.rank(options.selection_user,out.flights[slot]))slot=i;
        if(options.rank(options.selection_user,candidate)>=options.rank(options.selection_user,out.flights[slot]))return Result::Ok;
    }else ++out.count;
    out.flights[slot]=candidate;
    return Result::Ok;
}

Result decode(Provider provider,Encoding encoding,const uint8_t *input,size_t n,JsonDocument &doc,Batch &out,const ParseOptions &options){
    reset(out,provider);
    if(provider!=Provider::Vendor&&provider!=Provider::AdsbFi)return Result::Unsupported;
    if(!input||!n||!std::isfinite(options.max_position_age_seconds)||options.max_position_age_seconds<0)return Result::Invalid;
    if(n>MaxResponseBytes)return Result::TooLarge;
    if(encoding!=Encoding::Json&&encoding!=Encoding::MessagePack)return Result::Unsupported;
    if(provider==Provider::AdsbFi&&encoding!=Encoding::Json)return Result::Unsupported;
    if(encoding==Encoding::Json&&!json_envelope(input,n))return Result::Invalid;
    Reader reader{input,n,0};
    const auto err=encoding==Encoding::Json?deserializeJson(doc,reader,DeserializationOption::NestingLimit(12)):
        deserializeMsgPack(doc,reader,DeserializationOption::NestingLimit(12));
    if(err)return err==DeserializationError::NoMemory?Result::NoMemory:Result::Invalid;
    if(encoding==Encoding::MessagePack&&reader.at!=n)return Result::Invalid;
    JsonVariantConst root=doc.as<JsonVariantConst>();if(!root.is<JsonObjectConst>())return Result::Invalid;
    const char *compact=provider==Provider::Vendor?"f":"ac",*expanded=provider==Provider::Vendor?"flights":"aircraft";
    if(root.containsKey(compact)&&root.containsKey(expanded))return Result::Invalid;
    const auto list=root.containsKey(compact)?root[compact]:root[expanded];
    if(!list.is<JsonArrayConst>())return Result::Invalid;
    const auto overflow=root["ov"].isNull()?root["overflow"]:root["ov"];
    double ov=0;out.overflow=(overflow.is<bool>()&&overflow.as<bool>())||(number(overflow,ov)&&ov>0);
    for(JsonVariantConst row:list.as<JsonArrayConst>()){
        Flight candidate{};candidate.provider=provider;
        if(!row.is<JsonObjectConst>()||!(provider==Provider::Vendor?vendor(row,candidate):adsb(row,candidate,options))){++out.rejected;continue;}
        bool duplicate=false;for(size_t i=0;i<out.count;++i)if(!std::strcmp(out.flights[i].aircraft_id,candidate.aircraft_id)){duplicate=true;break;}
        if(duplicate){++out.duplicates;continue;}
        if(out.count==MaxFlights){out.overflow=true;continue;}
        out.flights[out.count++]=candidate;
    }
    return Result::Ok;
}

bool enrich_route(JsonVariantConst root,const char *requested,Flight &flight){
    if(flight.provider!=Provider::AdsbFi||flight.route_source!=RouteSource::Unknown||has_airport(flight.origin)||has_airport(flight.destination))return false;
    char wanted[16];if(!requested||!requested[0]||std::strlen(requested)>=sizeof wanted||
        !code(requested,wanted,std::strlen(requested),true))return false;
    // Callsigns are bounded to avoid truncation matching a different flight.
    if(std::strcmp(wanted,flight.callsign))return false;
    const auto route=root["response"]["flightroute"];char reported[16];
    if(!route.is<JsonObjectConst>()||!route["callsign_icao"].is<const char*>()||
       route["callsign_icao"].as<JsonString>().size()>=sizeof reported||
       !text(route["callsign_icao"],reported,sizeof reported,true)||std::strcmp(reported,wanted))return false;
    Airport origin{},destination{};airport(route["origin"],origin);airport(route["destination"],destination);
    if(!has_airport(origin)||!has_airport(destination))return false;
    flight.origin=origin;flight.destination=destination;flight.route_source=RouteSource::CallsignDatabase;
    const auto airline=route["airline"];char b[12];
    text(airline["icao"],b,sizeof b);code(b,flight.airline.icao,3);
    text(airline["iata"],b,sizeof b);code(b,flight.airline.iata,2,true);
    text(airline["name"],flight.airline.name,sizeof flight.airline.name);
    return true;
}

Logo resolve_logo(const Flight &flight,const LogoCatalog &catalog){
    Logo out{};out.kind=LogoKind::Text;char candidate[4]={0};
    if(!code(flight.airline.icao,candidate,3))code(flight.airline.vendor_code,candidate,3);
    if(!candidate[0]&&flight.airline.iata[0]&&catalog.crosswalk){
        bool ambiguous=false;
        for(size_t i=0;i<catalog.crosswalk_count;++i){const auto &row=catalog.crosswalk[i];
            if(row.iata&&!std::strcmp(row.iata,flight.airline.iata)){
                char mapped[4];if(!code(row.icao,mapped,3))continue;
                if(candidate[0]&&std::strcmp(candidate,mapped))ambiguous=true;
                std::strcpy(candidate,mapped);
            }
        }
        if(ambiguous)candidate[0]=0;
    }
    if(!candidate[0]&&std::strlen(flight.callsign)>3){
        char prefix[4]={flight.callsign[0],flight.callsign[1],flight.callsign[2],0};
        if(code(prefix,candidate,3)){
            Logo probe{};
            if(logo_path(catalog,"/owner/logos/",candidate,probe,LogoKind::Owner)||
               logo_path(catalog,"/images/airlines/square/",candidate,probe,LogoKind::Stock))out.inferred_from_callsign=true;
            else candidate[0]=0;
        }
    }
    if(candidate[0]){
        std::strcpy(out.airline_code,candidate);
        if(logo_path(catalog,"/owner/logos/",candidate,out,LogoKind::Owner)||
           logo_path(catalog,"/images/airlines/square/",candidate,out,LogoKind::Stock))return out;
    }
    const char *fallback=flight.category==Category::Helicopter?"heli":flight.category==Category::Light?"light":
        flight.category==Category::SmallJet?"smalljet":"default";
    if(!logo_path(catalog,"/images/airlines/square/",fallback,out,LogoKind::Category)&&std::strcmp(fallback,"default"))
        logo_path(catalog,"/images/airlines/square/","default",out,LogoKind::Category);
    return out;
}
} // namespace flightwall
