#include "provider_policy.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace flightwall {
namespace {
bool fields(JsonVariantConst source,JsonObject target,std::initializer_list<const char*> keys){
    if(!source.is<JsonObjectConst>())return false;
    for(const char *key:keys)if(source.containsKey(key)){
        auto value=source[key];
        if(value.is<JsonArrayConst>()||value.is<JsonObjectConst>())return false;
        if(value.is<const char*>()&&value.as<JsonString>().size()>128)return false;
        if(value.is<double>()&&!value.is<bool>()&&!std::isfinite(value.as<double>()))return false;
        target[key]=value;
    }
    return true;
}
bool radius(JsonVariantConst source,JsonObject target){
    if(!fields(source,target,{"id","name","latitude","longitude","radius_km","min_altitude","max_altitude"}))return false;
    for(const auto key:{"latitude","longitude","radius_km"})if(source[key].is<bool>()||!source[key].is<double>())return false;
    Request checked{};
    if(!make_request(Provider::AdsbFi,{source["latitude"].as<double>(),source["longitude"].as<double>(),source["radius_km"].as<double>()},checked))return false;
    target["type"]="radius";return true;
}
bool project(JsonVariantConst local,JsonObject target){
    if(!local.is<JsonObjectConst>())return false;
    if(local.containsKey("radius_request")){
        if(local["radius_request"].isNull())target["radius_request"]=nullptr;
        else if(!radius(local["radius_request"],target.createNestedObject("radius_request")))return false;
    }
    if(local.containsKey("radius_requests")){
        auto zones=local["radius_requests"];if(!zones.is<JsonArrayConst>()||zones.size()>16)return false;
        auto list=target.createNestedArray("radius_requests");
        for(JsonVariantConst zone:zones.as<JsonArrayConst>())if(!radius(zone,list.createNestedObject()))return false;
    }
    if(!local["geo_request"].isNull())return false; // Geometry contract still unverified.
    if(local.containsKey("geo_request"))target["geo_request"]=nullptr;
    if(local.containsKey("tracked_flights")){
        auto tracks=local["tracked_flights"];if(!tracks.is<JsonArrayConst>()||tracks.size()>32)return false;
        auto list=target.createNestedArray("tracked_flights");
        for(JsonVariantConst track:tracks.as<JsonArrayConst>()){
            if(!track["flight_number"].is<const char*>()||track["flight_number"].as<JsonString>().size()>16)return false;
            if(!fields(track,list.createNestedObject(),{"flight_number","created_at","auto_remove","show_distance_travelled","show_metrics"}))return false;
        }
    }
    const auto filters=local["data_filters"];
    if(!filters.isNull()&&!filters.is<JsonObjectConst>())return false;
    for(const auto key:{"show_light_ac","show_heli_ac","show_commercial_ac","show_smalljet_ac","show_other_ac","show_na_routes","min_speed_mps","max_speed_mps"}){
        const auto value=filters.containsKey(key)?filters[key]:local[key];if(value.isNull())continue;
        if(std::strncmp(key,"show_",5)==0){if(!value.is<bool>())return false;}
        else if(value.is<bool>()||!value.is<double>()||!std::isfinite(value.as<double>())||value.as<double>()<0)return false;
        target[key]=value;
    }
    auto airlines=filters.containsKey("airline_icao_filter")?filters["airline_icao_filter"]:local["airline_icao_filter"];
    if(!airlines.isNull()){
        if(!airlines.is<JsonArrayConst>()||airlines.size()>128)return false;
        auto list=target.createNestedArray("airline_icao_filter");
        for(JsonVariantConst airline:airlines.as<JsonArrayConst>()){
            if(!airline.is<const char*>()||airline.as<JsonString>().size()!=3)return false;
            const char *code=airline.as<const char*>();for(unsigned i=0;i<3;++i)if(code[i]<'A'||code[i]>'Z')return false;
            list.add(airline);
        }
    }
    return true;
}
}
bool make_request(Provider provider,const Zone &zone,Request &out){
    out=Request{};
    if(provider==Provider::Vendor){
        out.host="api.theflightwall.com";out.method="POST";out.accept="application/x-msgpack";
        out.needs_device_key=true;out.tls=true;std::strcpy(out.path,"/v2/flights");return true;
    }
    if(provider!=Provider::AdsbFi||!std::isfinite(zone.latitude)||!std::isfinite(zone.longitude)||
       !std::isfinite(zone.radius_km)||zone.latitude< -90||zone.latitude>90||zone.longitude< -180||zone.longitude>180||
       zone.radius_km<=0||zone.radius_km>463)return false;
    out.host="opendata.adsb.fi";out.method="GET";out.accept="application/json";out.tls=true;out.needs_device_key=false;
    // API radius is nautical miles, not local settings' kilometres.
    const auto n=std::snprintf(out.path,sizeof out.path,"/api/v3/lat/%.6f/lon/%.6f/dist/%.6f",
                              zone.latitude,zone.longitude,zone.radius_km/1.852);
    return n>0&&static_cast<size_t>(n)<sizeof out.path;
}
bool vendor_request_body(JsonVariantConst local,JsonDocument &out){
    out.clear();
    if(!project(local,out.to<JsonObject>())||out.overflowed()){out.clear();return false;}
    return true;
}
bool PollBudget::ready(uint32_t now)const{return !scheduled_||static_cast<int32_t>(now-next_)>=0;}
bool PollBudget::reserve(uint32_t now){if(!ready(now))return false;scheduled_=true;next_=now+5000;return true;}
void PollBudget::complete(uint32_t now,unsigned status,uint32_t retry){
    scheduled_=true;
    if(status==200){failures_=0;next_=now+5000;return;}
    if(failures_<7)++failures_;
    uint32_t delay=5000u<<(failures_-1);if(delay>300000)delay=300000;
    // Respect Retry-After without overflow; a malformed huge value cannot spin.
    if(retry>86400)retry=86400;
    if((status==429||status==503)&&retry*1000u>delay)delay=retry*1000u;
    if(status==429&&delay<60000)delay=60000;
    next_=now+delay;
}
void FrameState::select(Provider value,Batch &active){provider=value;has_good_frame=false;updated_ms=0;reset(active,value);}
bool FrameState::publish(Result result,const Batch &staging,uint32_t now,Batch &active){
    if(result!=Result::Ok||staging.provider!=provider||staging.count>MaxFlights)return false;
    active=staging;has_good_frame=true;updated_ms=now;return true;
}
bool FrameState::stale(uint32_t now,uint32_t age)const{return !has_good_frame||static_cast<uint32_t>(now-updated_ms)>age;}
} // namespace flightwall
