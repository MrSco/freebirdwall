#include "../src/provider_policy.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
using namespace flightwall;
int main(){
    Request request{};Zone zone{40.641300,-73.778100,18.52};   /* JFK */
    assert(make_request(Provider::AdsbFi,zone,request));
    assert(request.tls&&!request.needs_device_key&&!std::strcmp(request.host,"opendata.adsb.fi")&&std::strstr(request.path,"/dist/10.000000"));
    assert(!std::strcmp(request.method,"GET"));
    zone.latitude=91;assert(!make_request(Provider::AdsbFi,zone,request));
    zone.latitude=std::numeric_limits<double>::quiet_NaN();assert(!make_request(Provider::AdsbFi,zone,request));
    zone={0,0,464};assert(!make_request(Provider::AdsbFi,zone,request));
    zone.radius_km=0;assert(!make_request(Provider::AdsbFi,zone,request));
    assert(make_request(Provider::Vendor,zone,request)&&request.needs_device_key&&request.tls&&!std::strcmp(request.path,"/v2/flights"));
    StaticJsonDocument<4096> local;StaticJsonDocument<4096> body;
    assert(!deserializeJson(local,R"({"radius_requests":[{"latitude":26,"longitude":-80,"radius_km":5}],"data_filters":{"show_heli_ac":false,"airline_icao_filter":["DAL"]},"display_config":{"brightness_percent":100},"owner_aliases":{"revision":9},"token":"secret","api_key":"secret","userId":"secret"})"));
    assert(vendor_request_body(local.as<JsonVariantConst>(),body));
    assert(body["radius_requests"].is<JsonArray>()&&body["show_heli_ac"].is<bool>()&&!body["show_heli_ac"].as<bool>());
    assert(!std::strcmp(body["airline_icao_filter"][0],"DAL"));
    assert(!body.containsKey("token")&&!body.containsKey("api_key")&&!body.containsKey("display_config")&&!body.containsKey("owner_aliases")&&!body.containsKey("userId"));
    StaticJsonDocument<1> tiny;assert(!vendor_request_body(local.as<JsonVariantConst>(),tiny)&&tiny.isNull());
    local["data_filters"]=false;assert(!vendor_request_body(local.as<JsonVariantConst>(),body));
    StaticJsonDocument<4096> nested;
    assert(!deserializeJson(nested,R"({"radius_requests":[{"latitude":26,"longitude":-80,"radius_km":5,"token":"nested-secret"}],"tracked_flights":[{"flight_number":"DAL42","auto_remove":false,"token":"nested-secret"}]})"));
    assert(vendor_request_body(nested.as<JsonVariantConst>(),body));
    assert(!body["radius_requests"][0].containsKey("token")&&!body["tracked_flights"][0].containsKey("token"));
    nested["geo_request"].to<JsonObject>()["coordinates"]="unverified";
    assert(!vendor_request_body(nested.as<JsonVariantConst>(),body)&&body.isNull());
    nested.remove("geo_request");nested["radius_requests"][0]["latitude"]=91;
    assert(!vendor_request_body(nested.as<JsonVariantConst>(),body)&&body.isNull());
    PollBudget budget;assert(budget.ready(0)&&budget.reserve(0));
    assert(!budget.reserve(999)&&!budget.reserve(4999)&&budget.reserve(5000));
    budget.complete(5000,429,120);assert(!budget.ready(124999)&&budget.ready(125000));
    budget.complete(125000,200);assert(budget.next_ms()==130000);
    budget.complete(130000,0);assert(budget.next_ms()==135000);
    budget.complete(135000,500);assert(budget.next_ms()==145000);
    for(unsigned i=0;i<10;++i){budget.complete(200000,500);}
    assert(budget.next_ms()==500000);
    // Signed deadline comparisons stay valid across a uint32 millisecond wrap.
    PollBudget wrap;assert(wrap.reserve(0xfffffff0u));assert(!wrap.ready(0xfffffff1u));assert(!wrap.ready(1000));assert(wrap.ready(5000));
    wrap.complete(5000,503,0xffffffff);assert(wrap.next_ms()==86405000);
    FrameState state;Batch active{},staging{};state.select(Provider::AdsbFi,active);
    assert(state.stale(0)&&active.count==0);
    staging.provider=Provider::AdsbFi;staging.count=1;std::strcpy(staging.flights[0].callsign,"DAL42");
    assert(state.publish(Result::Ok,staging,10,active)&&!state.stale(60010)&&state.stale(60011));
    assert(!state.publish(Result::Invalid,staging,70000,active)&&active.count==1&&state.updated_ms==10);
    state.select(Provider::Vendor,active);assert(active.count==0&&state.stale(0));
    assert(!state.publish(Result::Ok,staging,70000,active));
    staging.provider=Provider::Vendor;staging.count=MaxFlights+1;assert(!state.publish(Result::Ok,staging,70000,active));
    std::cout<<"Provider policy: fixed endpoints, credential separation, filters, rate/backoff, rollover, last-good and explicit source switching passed\n";
}
