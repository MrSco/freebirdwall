#pragma once
#include "provider_policy.h"
namespace flightwall {
struct ZoneRule {Zone zone;double min_altitude=0,max_altitude=0;bool has_min=false,has_max=false;};
struct CommunityConfig {
    ZoneRule zones[16];size_t count=0;
    bool commercial=true,heli=false,light=false,smalljet=false,other=false,unknown_routes=false;
    double min_speed=0,max_speed=0;bool has_min_speed=false,has_max_speed=false;
    char airlines[128][4];size_t airline_count=0;
    // Positions only: tracked flights and geo areas need the vendor source and
    // are left out (counted, so the owner can be told).
    size_t skipped_tracked=0,skipped_geo=0;
};
Result community_config(JsonVariantConst request_config,CommunityConfig &);
// Great-circle chord comparison (polynomial sin, no extra target libm). Radius
// and altitude comparisons use km/metres respectively. Handles dateline/poles.
double zone_rank(const ZoneRule &,const Flight &);
uint16_t matching_zones(const CommunityConfig &,const Flight &);
bool community_candidate(const CommunityConfig &,const Flight &,const LogoCatalog &);
bool community_displayable(const CommunityConfig &,const Flight &,const LogoCatalog &);
}
