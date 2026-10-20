#include "../src/flight_data.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace flightwall;
const char *vendor_fixture=R"({"v":2,"f":[{"icao":"ABC123","cs":"DAL42","reg":"N42DA","al_code":"DAL","al_paint":"AAL","al_op":"ZZZ","al_name":"Delta","orig":"Palm Beach Intl","orig_iata":"PBI","orig_icao":"KPBI","dest":"Atlanta","dest_iata":"ATL","dest_icao":"KATL","vel":200,"alt_b":10000,"trk":0,"gnd":false}],"ch":"untrusted","display_config":{"brightness_percent":100},"firmware":{"url":"https://invalid.example"}})";
const char *adsb_fixture=R"({"ac":[{"hex":"abc123","flight":" dal42  ","r":"N42DA","t":"A320","lat":26.07,"lon":-80.14,"seen_pos":0,"alt_baro":10000,"gs":300,"track":0,"baro_rate":600}],"total":1})";
const char *route_fixture=R"({"response":{"flightroute":{"callsign_icao":"DAL42","airline":{"icao":"DAL","iata":"DL","name":"Delta"},"origin":{"iata_code":"PBI","icao_code":"KPBI","name":"Palm Beach Intl"},"destination":{"iata_code":"ATL","icao_code":"KATL","name":"Atlanta"}}}})";
struct Store { std::vector<std::string> paths,queries; };
bool exists(void *ctx,const char *p){auto &s=*static_cast<Store*>(ctx);s.queries.emplace_back(p);for(const auto &v:s.paths)if(v==p)return true;return false;}
Result parse(const char *json,Provider provider,JsonDocument &doc,Batch &out){return decode(provider,Encoding::Json,reinterpret_cast<const uint8_t*>(json),std::strlen(json),doc,out);}
void near(double a,double b){assert(std::abs(a-b)<0.00001);}
int main(){
    DynamicJsonDocument doc(96*1024),other(8192);Batch batch{},vendor_batch{};
    assert(parse(vendor_fixture,Provider::Vendor,doc,vendor_batch)==Result::Ok&&vendor_batch.count==1);
    const auto &v=vendor_batch.flights[0];assert(v.route_source==RouteSource::Vendor&&v.speed.unit==Unit::MetresPerSecond);
    assert(v.altitude.unit==Unit::Metres);near(v.altitude.value,10000);near(v.speed.value,200);
    assert(!std::strcmp(v.origin.iata,"PBI")&&!std::strcmp(v.airline.vendor_paint,"AAL"));
    assert(v.heading.valid&&v.heading.value==0&&v.has_ground_state&&!v.on_ground);
    // Decode the same vendor record through MessagePack, including compact keys.
    std::string mp;serializeMsgPack(doc,mp);
    assert(decode(Provider::Vendor,Encoding::MessagePack,reinterpret_cast<const uint8_t*>(mp.data()),mp.size(),doc,batch)==Result::Ok);
    assert(batch.count==1&&!std::strcmp(batch.flights[0].callsign,"DAL42"));
    auto trailing=mp+"x";assert(decode(Provider::Vendor,Encoding::MessagePack,reinterpret_cast<const uint8_t*>(trailing.data()),trailing.size(),doc,batch)==Result::Invalid&&batch.count==0);
    for(size_t n=0;n<mp.size();++n)assert(decode(Provider::Vendor,Encoding::MessagePack,reinterpret_cast<const uint8_t*>(mp.data()),n,doc,batch)!=Result::Ok&&batch.count==0);
    assert(parse(adsb_fixture,Provider::AdsbFi,doc,batch)==Result::Ok&&batch.count==1);
    auto &f=batch.flights[0];assert(!std::strcmp(f.callsign,"DAL42")&&f.route_source==RouteSource::Unknown);
    near(f.altitude.value,3048);near(f.speed.value,154.3333333333);near(f.vertical_rate.value,3.048);
    assert(f.altitude.unit==Unit::Metres&&f.has_position&&f.position_age_seconds==0&&f.heading.valid&&f.heading.value==0);
    Store stock{{"/images/airlines/square/DAL.png","/images/airlines/square/default.png","/images/airlines/square/heli.png"},{}};
    LogoCatalog catalog{&stock,exists,nullptr,0};
    auto vl=resolve_logo(v,catalog),al=resolve_logo(f,catalog);
    assert(vl.kind==LogoKind::Stock&&al.kind==LogoKind::Stock&&!std::strcmp(vl.path,al.path)&&al.inferred_from_callsign);
    assert(deserializeJson(other,route_fixture)==DeserializationError::Ok);
    assert(!enrich_route(other.as<JsonVariantConst>(),"AAL42",f));
    assert(!enrich_route(other.as<JsonVariantConst>(),"AAAAAAAAAAAAAAAAAAAA",f));
    assert(enrich_route(other.as<JsonVariantConst>(),"DAL42",f));
    assert(f.route_source==RouteSource::CallsignDatabase&&!std::strcmp(f.airline.icao,"DAL")&&!std::strcmp(f.destination.iata,"ATL"));
    assert(!enrich_route(other.as<JsonVariantConst>(),"DAL42",f));
    auto explicit_logo=resolve_logo(f,catalog);assert(!explicit_logo.inferred_from_callsign&&!std::strcmp(vl.path,explicit_logo.path));
    stock.paths.emplace_back("/owner/logos/DAL.png");assert(resolve_logo(f,catalog).kind==LogoKind::Owner);
    Flight unknown{};unknown.category=Category::Helicopter;std::strcpy(unknown.callsign,"N12345");
    assert(resolve_logo(unknown,catalog).kind==LogoKind::Category);
    std::strcpy(unknown.airline.vendor_code,"../");auto safe=resolve_logo(unknown,catalog);assert(safe.kind==LogoKind::Category);
    for(const auto &path:stock.queries)assert(path.find("..") == std::string::npos);
    Store empty{};LogoCatalog no_assets{&empty,exists,nullptr,0};assert(resolve_logo(unknown,no_assets).kind==LogoKind::Text);
    AirlineCode crosswalk[]={{"DAL","DL"},{"AAL","AA"}};catalog.crosswalk=crosswalk;catalog.crosswalk_count=2;
    Flight iata{};std::strcpy(iata.airline.iata,"DL");assert(resolve_logo(iata,catalog).kind==LogoKind::Owner);
    AirlineCode ambiguous[]={{"DAL","DL"},{"AAL","DL"}};catalog.crosswalk=ambiguous;
    assert(resolve_logo(iata,catalog).kind==LogoKind::Category);
    // Missing, stale and invalid positions never become live overhead aircraft.
    const char *bad_rows=R"({"ac":[{"hex":"abc123","lat":26,"lon":-80,"seen_pos":61},{"hex":"def123","lat":91,"lon":0,"seen_pos":0},{"hex":"fed123","lat":26,"lon":0},{"hex":"123abc","lat":true,"lon":0,"seen_pos":0},null]})";
    assert(parse(bad_rows,Provider::AdsbFi,doc,batch)==Result::Ok&&batch.count==0&&batch.rejected==5);
    assert(parse(R"({"ac":[{"hex":"abc123","lat":0,"lon":0,"seen_pos":0,"alt_baro":"ground","gs":0,"category":"A7"}]})",Provider::AdsbFi,doc,batch)==Result::Ok);
    assert(batch.count==1&&batch.flights[0].on_ground&&!batch.flights[0].altitude.valid&&batch.flights[0].speed.valid&&batch.flights[0].speed.value==0&&batch.flights[0].category==Category::Helicopter);
    assert(parse(R"({"f":[{"icao":"ABC123"},{"icao":"abc123"},false,{"icao":"../../"}],"ov":2})",Provider::Vendor,doc,batch)==Result::Ok);
    assert(batch.count==1&&batch.duplicates==1&&batch.rejected==2&&batch.overflow);
    assert(parse(R"({"f":[]})",Provider::Vendor,doc,batch)==Result::Ok&&batch.count==0);
    for(const auto bad:{"{}","[]","{\"f\":null}","{\"f\":[],\"flights\":[]}","{\"f\":[]}x","{\"f\":[]}{}","{\"f\": [}"})
        assert(parse(bad,Provider::Vendor,doc,batch)!=Result::Ok&&batch.count==0);
    assert(parse(" \n{\"flights\":[]}\t",Provider::Vendor,doc,batch)==Result::Ok);
    StaticJsonDocument<1> tiny;assert(parse(vendor_fixture,Provider::Vendor,tiny,batch)==Result::NoMemory&&batch.count==0);
    std::string huge(MaxResponseBytes+1,' ');assert(decode(Provider::Vendor,Encoding::Json,reinterpret_cast<const uint8_t*>(huge.data()),huge.size(),doc,batch)==Result::TooLarge);
    std::string many="{\"f\":[";for(int i=0;i<40;++i){if(i)many+=",";char id[7];std::snprintf(id,sizeof id,"%06x",i);many+="{\"icao\":\""+std::string(id)+"\"}";}many+="]}";
    assert(parse(many.c_str(),Provider::Vendor,doc,batch)==Result::Ok&&batch.count==MaxFlights&&batch.overflow);
    // Display text stays bounded and valid; identifiers never become path input.
    assert(parse(R"({"f":[{"icao":"abc123","orig":"S\u00e3o Paulo","al_code":"../","al_name":"bad\u0000name"}]})",Provider::Vendor,doc,batch)==Result::Ok);
    assert(!std::strcmp(batch.flights[0].origin.name,"S\xc3\xa3o Paulo")&&batch.flights[0].airline.name[0]==0);
    std::cout<<"Flight adapters: JSON/MessagePack, units, stale/malformed bounds, route provenance, provider-independent logos and safe fallbacks passed\n";
}
