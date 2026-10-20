#include "community_cache.h"
#include <cstring>
namespace flightwall {
void CommunityCache::clear(){reset(batch,Provider::AdsbFi);overflow_zones=0;for(size_t i=0;i<MaxFlights;++i){received[i]=lookup[i]=0;zones[i]=0;}}
void CommunityCache::erase(size_t i){
    --batch.count;
    for(;i<batch.count;++i){batch.flights[i]=batch.flights[i+1];received[i]=received[i+1];lookup[i]=lookup[i+1];zones[i]=zones[i+1];}
}
void CommunityCache::expire(uint32_t now){
    for(size_t i=0;i<batch.count;){const auto &f=batch.flights[i];double age=f.position_age_seconds+static_cast<uint32_t>(now-received[i])/1000.0;if(!zones[i]||!f.has_position_age||age>60)erase(i);else ++i;}
    if(!batch.count)overflow_zones=0;
    batch.overflow=overflow_zones!=0;
}
void CommunityCache::replace_zone(size_t index,const Batch &incoming,const CommunityConfig &config,uint32_t now){
    if(index>=config.count||incoming.count>MaxFlights||incoming.provider!=Provider::AdsbFi)return;
    overflow_zones&=static_cast<uint16_t>(~(1u<<index));
    if(incoming.overflow)overflow_zones|=static_cast<uint16_t>(1u<<index);
    for(size_t i=0;i<batch.count;++i)zones[i]&=static_cast<uint16_t>(~(1u<<index));
    for(size_t n=0;n<incoming.count;++n){
        const auto &f=incoming.flights[n];uint16_t mask=matching_zones(config,f);if(!mask)continue;
        size_t slot=0;while(slot<batch.count&&std::strcmp(batch.flights[slot].aircraft_id,f.aircraft_id))++slot;
        if(slot<batch.count){
            Flight next=f;const auto &old=batch.flights[slot];
            if(!std::strcmp(old.callsign,next.callsign)&&old.route_source==RouteSource::CallsignDatabase){next.origin=old.origin;next.destination=old.destination;next.airline=old.airline;next.route_source=old.route_source;}
            else lookup[slot]=0;
            batch.flights[slot]=next;received[slot]=now;zones[slot]=mask;continue;
        }
        if(batch.count==MaxFlights){
            overflow_zones|=static_cast<uint16_t>(1u<<index);
            // Bound memory; choose the best normalized distance across all zones.
            double worst=-1;slot=0;
            for(size_t i=0;i<batch.count;++i){double rank=1e100;if(zones[i])for(size_t z=0;z<config.count;++z){double r=zone_rank(config.zones[z],batch.flights[i]);if(r<rank)rank=r;}if(rank>worst){worst=rank;slot=i;}}
            double rank=1e100;for(size_t z=0;z<config.count;++z){double r=zone_rank(config.zones[z],f);if(r<rank)rank=r;}if(rank>=worst)continue;
        }else ++batch.count;
        batch.flights[slot]=f;received[slot]=now;zones[slot]=mask;lookup[slot]=0;
    }
    expire(now);
}
bool CommunityCache::route_candidate(uint32_t now,Flight &out){
    for(size_t i=0;i<batch.count;++i){const auto &f=batch.flights[i];
        if(f.route_source==RouteSource::Unknown&&f.callsign[0]&&(!lookup[i]||static_cast<uint32_t>(now-lookup[i])>=60000)){
            lookup[i]=now?now:1;out=f;return true;
        }
    }return false;
}
void CommunityCache::route_result(const Flight &f){
    for(size_t i=0;i<batch.count;++i)if(!std::strcmp(batch.flights[i].aircraft_id,f.aircraft_id)&&!std::strcmp(batch.flights[i].callsign,f.callsign)){
        batch.flights[i].origin=f.origin;batch.flights[i].destination=f.destination;batch.flights[i].airline=f.airline;batch.flights[i].route_source=f.route_source;return;
    }
}
void CommunityCache::display(const CommunityConfig &config,const LogoCatalog &logos,uint32_t now,Batch &out){
    expire(now);reset(out,Provider::AdsbFi);out.overflow=batch.overflow;
    for(size_t i=0;i<batch.count;++i)if(community_displayable(config,batch.flights[i],logos))out.flights[out.count++]=batch.flights[i];
    for(size_t i=1;i<out.count;++i){
        Flight value=out.flights[i];double rank=1e100;
        for(size_t z=0;z<config.count;++z){double r=zone_rank(config.zones[z],value);if(r<rank)rank=r;}
        size_t at=i;
        while(at){double previous=1e100;for(size_t z=0;z<config.count;++z){double r=zone_rank(config.zones[z],out.flights[at-1]);if(r<previous)previous=r;}if(previous<=rank)break;out.flights[at]=out.flights[at-1];--at;}
        out.flights[at]=value;
    }
}
}
