// Developer-only offline probe. This process is not part of normal board use.
#include "../src/flight_data.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <set>
using namespace flightwall;
bool exists(void *ctx,const char *path){return static_cast<const std::set<std::string>*>(ctx)->count(path)!=0;}
int main(int argc,char **argv){
    if(argc!=3&&argc!=4){std::cerr<<"Usage: feed_inspect vendor-msgpack|vendor-json|adsb-fi FILE [PRIVATE_LOGO_INVENTORY_JSON]\n";return 2;}
    const std::string mode=argv[1];if(mode!="vendor-msgpack"&&mode!="vendor-json"&&mode!="adsb-fi")return 2;
    std::ifstream file(argv[2],std::ios::binary|std::ios::ate);if(!file)return 2;
    if(file.tellg()<0||file.tellg()>static_cast<std::streamoff>(MaxResponseBytes))return 2;
    file.seekg(0);
    std::string bytes((std::istreambuf_iterator<char>(file)),{});
    DynamicJsonDocument doc(96*1024);Batch out{};
    auto result=decode(mode=="adsb-fi"?Provider::AdsbFi:Provider::Vendor,mode=="vendor-msgpack"?Encoding::MessagePack:Encoding::Json,
        reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size(),doc,out);
    size_t airline_logos=0,category_logos=0,text_only=0;
    if(argc==4){
        std::ifstream inventory(argv[3],std::ios::binary|std::ios::ate);if(!inventory)return 2;
        if(inventory.tellg()<0||inventory.tellg()>1024*1024)return 2;
        inventory.seekg(0);
        DynamicJsonDocument index(512*1024);if(deserializeJson(index,inventory)||!index["files"].is<JsonArray>())return 2;
        std::set<std::string> paths;
        for(JsonVariantConst row:index["files"].as<JsonArrayConst>())if(row["path"].is<const char*>())paths.emplace(row["path"].as<const char*>());
        LogoCatalog catalog{&paths,exists,nullptr,0};
        for(size_t i=0;i<out.count;++i){auto logo=resolve_logo(out.flights[i],catalog);
            if(logo.kind==LogoKind::Owner||logo.kind==LogoKind::Stock)++airline_logos;
            else if(logo.kind==LogoKind::Category)++category_logos;else ++text_only;
        }
    }
    std::cout<<"{\"ok\":"<<(result==Result::Ok?"true":"false")<<",\"flights\":"<<out.count<<",\"rejected\":"<<out.rejected
        <<",\"duplicates\":"<<out.duplicates<<",\"overflow\":"<<(out.overflow?"true":"false")<<",\"document_bytes\":"<<doc.memoryUsage()
        <<",\"airline_logos\":"<<airline_logos<<",\"category_logos\":"<<category_logos<<",\"text_only\":"<<text_only<<"}\n";
    return result==Result::Ok?0:1;
}
