#include "provider_wire.h"
#include <cmath>
#include <cstring>

namespace flightwall {
namespace {
// Bounded owned records can also arrive from non-JSON callers; validate their
// termination before passing strings to ArduinoJson or the logo resolver.
template<size_t N> bool terminated(const char (&s)[N]) { return std::memchr(s,0,N)!=nullptr; }
bool valid(const Airport &a) { return terminated(a.iata)&&terminated(a.icao)&&terminated(a.name); }
bool valid(const Flight &f) {
    const auto &a=f.airline;
    return terminated(f.aircraft_id)&&terminated(f.callsign)&&terminated(f.registration)&&
        terminated(f.aircraft_type)&&terminated(f.aircraft_name)&&terminated(a.icao)&&terminated(a.iata)&&
        terminated(a.name)&&terminated(a.vendor_code)&&terminated(a.vendor_paint)&&terminated(a.vendor_operator)&&
        valid(f.origin)&&valid(f.destination)&&std::strlen(f.aircraft_id)>=6;
}
void metric(JsonObject row,const char *key,const Metric &m,Unit expected) {
    if(m.valid&&m.unit==expected&&std::isfinite(m.value)) row[key]=m.value;
}
void airport(JsonObject row,const Airport &a,const char *name,const char *iata,const char *icao) {
    if(a.name[0])row[name]=a.name;
    if(a.iata[0])row[iata]=a.iata;
    if(a.icao[0])row[icao]=a.icao;
}
}
Result encode_stock_frame(const Batch &batch,const LogoCatalog &catalog,JsonDocument &doc,
                          uint8_t *out,size_t cap,size_t &length) {
    length=0;doc.clear();
    if(!out||!cap||batch.count>MaxFlights||
       (batch.provider!=Provider::Vendor&&batch.provider!=Provider::AdsbFi))return Result::Invalid;
    const size_t count=batch.count<StockMaxFlights?batch.count:StockMaxFlights;
    for(size_t i=0;i<batch.count;++i)
        if(batch.flights[i].provider!=batch.provider||!valid(batch.flights[i]))return Result::Invalid;
    // Exact stock reader uses its integer decoder for ov; MessagePack Boolean
    // tags are rejected even though a generic JSON decoder accepts them.
    doc["v"]=1;doc["ov"]=static_cast<unsigned>(batch.overflow||batch.count>StockMaxFlights);
    auto rows=doc.createNestedArray("f");
    for(size_t i=0;i<count;++i) {
        const auto &f=batch.flights[i];auto row=rows.createNestedObject();
        row["icao"]=f.aircraft_id;row["cs"]=f.callsign;row["reg"]=f.registration;
        row["ac_type"]=f.aircraft_type;row["ac_name"]=f.aircraft_name;
        airport(row,f.origin,"orig","orig_iata","orig_icao");
        airport(row,f.destination,"dest","dest_iata","dest_icao");
        auto logo=resolve_logo(f,catalog);
        // The preserved stock renderer resolves al_code from its filesystem.
        // Owner-path support needs its separate renderer binding before upload UI.
        // Copy the stack-local logo code; the document must own it after this row.
        if(logo.airline_code[0])row["al_code"].set(ArduinoJson::JsonString(logo.airline_code,ArduinoJson::JsonString::Copied));
        if(f.airline.name[0])row["al_name"]=f.airline.name;
        const char *category=f.category==Category::Helicopter?"Heli":f.category==Category::Light?"Light":
            f.category==Category::SmallJet?"Smalljet":"";
        if(*category)row["ac_class"]=category;
        metric(row,"alt_b",f.altitude,Unit::Metres);metric(row,"vel",f.speed,Unit::MetresPerSecond);
        metric(row,"vr",f.vertical_rate,Unit::MetresPerSecond);metric(row,"trk",f.heading,Unit::Degrees);
        if(f.has_ground_state)row["gnd"]=f.on_ground;
    }
    if(doc.overflowed()){doc.clear();return Result::NoMemory;}
    const size_t need=measureMsgPack(doc);
    if(need>cap){doc.clear();return Result::TooLarge;}
    const size_t written=serializeMsgPack(doc,out,cap);doc.clear();
    if(written!=need)return Result::Invalid;
    length=written;return Result::Ok;
}
bool deliver_stock_frame(const Batch &batch,const LogoCatalog &catalog,JsonDocument &doc,
                         uint8_t *wire,size_t cap,void *output,StockDecoder decoder) {
    size_t length=0;
    if(!decoder||!output)return false;
    if(encode_stock_frame(batch,catalog,doc,wire,cap,length)!=Result::Ok)return false;
    return decoder(wire,length,output);
}
} // namespace flightwall
