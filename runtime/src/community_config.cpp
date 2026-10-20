#include "community_config.h"
#include <cmath>
#include <cstring>
namespace flightwall {
namespace {
bool number(JsonVariantConst v,double &out){if(v.is<bool>()||!v.is<double>())return false;out=v.as<double>();return std::isfinite(out);}
bool flag(JsonVariantConst v,bool &out){if(v.isNull())return true;if(!v.is<bool>())return false;out=v.as<bool>();return true;}
bool bound(JsonVariantConst v,double &out,bool &present){present=!v.isNull();return !present||(number(v,out)&&out>=0);}
bool zone(JsonVariantConst v,ZoneRule &out){
    if(!v.is<JsonObjectConst>()||!number(v["latitude"],out.zone.latitude)||!number(v["longitude"],out.zone.longitude)||!number(v["radius_km"],out.zone.radius_km))return false;
    if(out.zone.latitude< -90||out.zone.latitude>90||out.zone.longitude< -180||out.zone.longitude>180||out.zone.radius_km<=0||out.zone.radius_km>463)return false;
    if(!bound(v["min_altitude"],out.min_altitude,out.has_min)||!bound(v["max_altitude"],out.max_altitude,out.has_max))return false;
    return !(out.has_min&&out.has_max&&out.min_altitude>out.max_altitude);
}
double sine(double x){const double y=x*x;return x*(1+y*(-1.0/6+y*(1.0/120+y*(-1.0/5040+y*(1.0/362880+y*(-1.0/39916800+y/6227020800.0))))));}
bool classified(const CommunityConfig &c,const Flight &f,const LogoCatalog &catalog){
    if(f.category==Category::Helicopter)return c.heli;
    if(f.category==Category::Light)return c.light;
    if(f.category==Category::SmallJet)return c.smalljet;
    auto logo=resolve_logo(f,catalog);
    return logo.airline_code[0]?c.commercial:c.other;
}
}
Result community_config(JsonVariantConst root,CommunityConfig &out){
    out=CommunityConfig{};if(!root.is<JsonObjectConst>())return Result::Invalid;
    for(const auto key:{"geo_requests","tracked_flights"})if(!root[key].isNull()&&!root[key].is<JsonArrayConst>())return Result::Invalid;
    out.skipped_geo=root["geo_requests"].size()+(root["geo_request"].isNull()?0:1);
    out.skipped_tracked=root["tracked_flights"].size();
    auto zones=root["radius_requests"];
    if(!zones.isNull()&&!zones.is<JsonArrayConst>())return Result::Invalid;
    if(zones.size()>16)return Result::TooLarge;
    if(zones.size()){for(JsonVariantConst value:zones.as<JsonArrayConst>())if(!zone(value,out.zones[out.count++]))return Result::Invalid;}
    else if(!root["radius_request"].isNull()){if(!zone(root["radius_request"],out.zones[0]))return Result::Invalid;out.count=1;}
    if(!out.count)return Result::Invalid;
    auto nested=root["data_filters"];if(!nested.isNull()&&!nested.is<JsonObjectConst>())return Result::Invalid;
    JsonVariantConst f=nested.isNull()?root:nested;
    if(!flag(f["show_commercial_ac"],out.commercial)||!flag(f["show_heli_ac"],out.heli)||!flag(f["show_light_ac"],out.light)||
       !flag(f["show_smalljet_ac"],out.smalljet)||!flag(f["show_other_ac"],out.other)||!flag(f["show_na_routes"],out.unknown_routes)||
       !bound(f["min_speed_mps"],out.min_speed,out.has_min_speed)||!bound(f["max_speed_mps"],out.max_speed,out.has_max_speed))return Result::Invalid;
    if(out.has_min_speed&&out.has_max_speed&&out.min_speed>out.max_speed)return Result::Invalid;
    auto airlines=f["airline_icao_filter"];
    if(!airlines.isNull()&&!airlines.is<JsonArrayConst>())return Result::Invalid;
    if(airlines.size()>128)return Result::TooLarge;
    for(JsonVariantConst a:airlines.as<JsonArrayConst>()){
        if(!a.is<const char*>()||a.as<JsonString>().size()!=3)return Result::Invalid;
        const char *p=a.as<const char*>();for(unsigned i=0;i<3;++i)if(p[i]<'A'||p[i]>'Z')return Result::Invalid;
        std::memcpy(out.airlines[out.airline_count++],p,4);
    }return Result::Ok;
}
double zone_rank(const ZoneRule &z,const Flight &f){
    if(!f.has_position)return 1e100;
    if((z.has_min||z.has_max)&&(!f.altitude.valid||f.altitude.unit!=Unit::Metres||
       (z.has_min&&f.altitude.value<z.min_altitude)||(z.has_max&&f.altitude.value>z.max_altitude)))return 1e100;
    constexpr double rad=0.017453292519943295,halfpi=1.5707963267948966;
    double delta=f.longitude-z.zone.longitude;if(delta>180)delta-=360;if(delta< -180)delta+=360;
    double a=sine((f.latitude-z.zone.latitude)*rad/2),b=sine(delta*rad/2);
    double lat1=z.zone.latitude*rad,lat2=f.latitude*rad;
    if(lat1<0)lat1=-lat1;
    if(lat2<0)lat2=-lat2;
    double chord=a*a+sine(halfpi-lat1)*sine(halfpi-lat2)*b*b;
    double limit=sine(z.zone.radius_km/(2*6371.0088));return chord/(limit*limit);
}
uint16_t matching_zones(const CommunityConfig &c,const Flight &f){uint16_t bits=0;for(size_t i=0;i<c.count;++i)if(zone_rank(c.zones[i],f)<=1)bits|=static_cast<uint16_t>(1u<<i);return bits;}
bool community_candidate(const CommunityConfig &c,const Flight &f,const LogoCatalog &catalog){
    if(!matching_zones(c,f))return false;
    if((c.has_min_speed||c.has_max_speed)&&(!f.speed.valid||f.speed.unit!=Unit::MetresPerSecond||
       (c.has_min_speed&&f.speed.value<c.min_speed)||(c.has_max_speed&&f.speed.value>c.max_speed)))return false;
    if(c.airline_count){auto logo=resolve_logo(f,catalog);bool match=false;for(size_t i=0;i<c.airline_count;++i)if(!std::strcmp(c.airlines[i],logo.airline_code))match=true;if(!match)return false;}
    return classified(c,f,catalog);
}
bool community_displayable(const CommunityConfig &c,const Flight &f,const LogoCatalog &catalog){
    return community_candidate(c,f,catalog)&&(c.unknown_routes||f.route_source!=RouteSource::Unknown);
}
}
