#include "../src/community_cache.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
using namespace flightwall;
bool exists(void *,const char *p){return !std::strcmp(p,"/images/airlines/square/DAL.png")||!std::strcmp(p,"/images/airlines/square/default.png");}
Flight flight(){Flight f{};f.provider=Provider::AdsbFi;std::strcpy(f.aircraft_id,"ABC123");std::strcpy(f.callsign,"DAL42");f.has_position=f.has_position_age=true;f.latitude=0;f.longitude=0;f.altitude={1000,Unit::Metres,true};f.speed={100,Unit::MetresPerSecond,true};return f;}
int main(){
    DynamicJsonDocument doc(8192);CommunityConfig c{};LogoCatalog logos{nullptr,exists,nullptr,0};
    assert(!deserializeJson(doc,R"({"radius_requests":[{"latitude":0,"longitude":0,"radius_km":100},{"latitude":0,"longitude":0.1,"radius_km":100}],"data_filters":{}})"));
    assert(community_config(doc.as<JsonVariantConst>(),c)==Result::Ok&&c.count==2&&c.commercial&&!c.unknown_routes);
    auto f=flight();assert(matching_zones(c,f)==3&&community_candidate(c,f,logos)&&!community_displayable(c,f,logos));
    f.category=Category::Helicopter;assert(!community_candidate(c,f,logos));c.heli=true;assert(community_candidate(c,f,logos));f.category=Category::Unknown;
    c.airline_count=1;std::strcpy(c.airlines[0],"AAL");assert(!community_candidate(c,f,logos));std::strcpy(c.airlines[0],"DAL");assert(community_candidate(c,f,logos));c.airline_count=0;
    c.has_min_speed=true;c.min_speed=101;assert(!community_candidate(c,f,logos));c.has_min_speed=false;
    c.zones[0].has_min=c.zones[1].has_min=true;c.zones[0].min_altitude=c.zones[1].min_altitude=1001;assert(!matching_zones(c,f));c.zones[0].has_min=c.zones[1].has_min=false;
    // Great-circle comparison agrees with host libm, including dateline/poles.
    constexpr double rad=0.017453292519943295;
    for(double lat:{-89.9,-70.0,0.0,70.0,89.9})for(double lon:{-179.9,-10.0,179.9}){
        ZoneRule z{};z.zone={lat,lon,100};f.latitude=-lat/2;f.longitude=-lon;
        double a=std::sin((f.latitude-lat)*rad/2),b=std::sin((f.longitude-lon)*rad/2);
        double expected=(a*a+std::cos(lat*rad)*std::cos(f.latitude*rad)*b*b)/std::pow(std::sin(100.0/(2*6371.0088)),2);
        assert(std::abs(zone_rank(z,f)-expected)<1e-5*(expected+1));
    }
    f=flight();CommunityCache cache{};cache.clear();Batch incoming{};incoming.provider=Provider::AdsbFi;incoming.count=1;incoming.flights[0]=f;
    cache.replace_zone(0,incoming,c,1000);cache.replace_zone(1,incoming,c,2000);assert(cache.batch.count==1);
    Flight lookup{};assert(cache.route_candidate(3000,lookup)&&!cache.route_candidate(4000,lookup));
    lookup.route_source=RouteSource::CallsignDatabase;std::strcpy(lookup.origin.iata,"DJT");std::strcpy(lookup.destination.iata,"ATL");cache.route_result(lookup);
    Batch display{};cache.display(c,logos,5000,display);assert(display.count==1&&!std::strcmp(display.flights[0].origin.iata,"DJT"));
    incoming.flights[0].altitude.value=2000;cache.replace_zone(0,incoming,c,6000);assert(cache.batch.flights[0].route_source==RouteSource::CallsignDatabase);
    incoming.count=0;cache.replace_zone(0,incoming,c,7000);assert(cache.batch.count==1);cache.replace_zone(1,incoming,c,8000);assert(cache.batch.count==0);
    incoming.count=1;incoming.flights[0]=f;cache.replace_zone(0,incoming,c,10000);cache.expire(70001);assert(cache.batch.count==0);
    cache.clear();cache.replace_zone(0,incoming,c,0xfffffff0u);cache.expire(1000);assert(cache.batch.count==1);
    assert(!deserializeJson(doc,R"({"radius_requests":[{"latitude":0,"longitude":0,"radius_km":100}],"tracked_flights":[{"flight_number":"DAL42"}],"geo_requests":[{"coordinates":[]}],"geo_request":{}})"));assert(community_config(doc.as<JsonVariantConst>(),c)==Result::Ok&&c.count==1&&c.skipped_tracked==1&&c.skipped_geo==2);
    assert(!deserializeJson(doc,R"({"radius_requests":[{"latitude":0,"longitude":0,"radius_km":100}],"tracked_flights":{}})"));assert(community_config(doc.as<JsonVariantConst>(),c)==Result::Invalid);
    assert(!deserializeJson(doc,R"({"radius_requests":[{"latitude":true,"longitude":0,"radius_km":100}]})"));assert(community_config(doc.as<JsonVariantConst>(),c)==Result::Invalid);
    assert(!deserializeJson(doc,R"({"radius_requests":[{"latitude":0,"longitude":0,"radius_km":100}],"data_filters":{"show_heli_ac":"yes"}})"));assert(community_config(doc.as<JsonVariantConst>(),c)==Result::Invalid);
    // Selection runs before bounded retention, retaining closest accepted rows.
    Batch selected{};selected.provider=Provider::AdsbFi;ParseOptions options{};
    options.rank=[](void *,const Flight &v){return v.latitude;};
    options.allow=[](void *,const Flight &v){return v.longitude==0;};
    for(int i=100;i>0;--i){doc.clear();doc["hex"]="ABC123";char id[7];std::snprintf(id,sizeof id,"%06x",i);doc["hex"]=id;doc["lat"]=i/10.0;doc["lon"]=0;doc["seen_pos"]=0;assert(append_adsb_aircraft(doc.as<JsonVariantConst>(),selected,options)==Result::Ok);}
    assert(selected.count==MaxFlights&&selected.overflow);for(size_t i=0;i<selected.count;++i)assert(selected.flights[i].latitude<=MaxFlights/10.0+1e-10);
    std::cout<<"Community: typed zone/filter settings, spherical matching, bounded nearest selection, multi-zone dedupe, route preservation, expiry and clock wrap passed\n";
}
