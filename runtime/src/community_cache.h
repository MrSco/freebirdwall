#pragma once
#include "community_config.h"
namespace flightwall {
struct CommunityCache {
    Batch batch{};uint32_t received[MaxFlights]{},lookup[MaxFlights]{};uint16_t zones[MaxFlights]{},overflow_zones=0;
    void clear();
    void expire(uint32_t now);
    void replace_zone(size_t zone,const Batch &,const CommunityConfig &,uint32_t now);
    bool route_candidate(uint32_t now,Flight &out);
    void route_result(const Flight &);
    void display(const CommunityConfig &,const LogoCatalog &,uint32_t now,Batch &out);
private:void erase(size_t);
};
}
