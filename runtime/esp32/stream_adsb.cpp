#include "stream_adsb.h"
#include <cstring>
#include <cmath>

namespace flightwall {
namespace {
bool space(int c){return c==' '||c=='\t'||c=='\r'||c=='\n';}
bool digit(int c){return c>='0'&&c<='9';}
bool hex(int c){return digit(c)||(c>='a'&&c<='f')||(c>='A'&&c<='F');}
struct Scan {
    BodyReader source;size_t total=0;int cached=-3;
    Result error=Result::Ok;char *capture=nullptr;size_t size=0,capacity=0;
    int peek(){
        if(cached==-3){
            cached=source.read(source.user);
            if(cached>=0&&++total>MaxResponseBytes)error=Result::TooLarge;
            if(cached < -1||cached>255)error=Result::Invalid;
        }
        return error==Result::Ok?cached:-2;
    }
    int get(){
        int c=peek();if(c>=0){cached=-3;if(capture){
            if(size==capacity){error=Result::TooLarge;return -2;}
            capture[size++]=static_cast<char>(c);
        }}return c;
    }
    void ws(){while(space(peek()))get();}
    bool take(int c){if(peek()!=c)return false;return get()==c;}
    bool string(){
        if(!take('"'))return false;
        for(;;){int c=get();if(c<32)return false;if(c=='"')return true;
            if(c=='\\'){
                c=get();if(c=='u'){for(unsigned i=0;i<4;++i)if(!hex(get()))return false;}
                else if(c!='"'&&c!='\\'&&c!='/'&&c!='b'&&c!='f'&&c!='n'&&c!='r'&&c!='t')return false;
            }
        }
    }
    bool literal(const char *p){while(*p)if(get()!=*p++)return false;return true;}
    bool number(){
        take('-');
        if(!take('0')){if(peek()<'1'||peek()>'9')return false;while(digit(peek()))get();}
        if(take('.')){if(!digit(peek()))return false;while(digit(peek()))get();}
        if(peek()=='e'||peek()=='E'){get();if(peek()=='+'||peek()=='-')get();if(!digit(peek()))return false;while(digit(peek()))get();}
        return error==Result::Ok;
    }
    bool value(unsigned depth){
        ws();if(depth>12)return false;
        const int c=peek();
        if(c=='"')return string();
        if(c=='t')return literal("true");
        if(c=='f')return literal("false");
        if(c=='n')return literal("null");
        if(c!='{'&&c!='[')return number();
        get();ws();const int close=c=='{'?'}':']';
        if(take(close))return true;
        for(;;){
            if(c=='{'){if(!string())return false;ws();if(!take(':'))return false;}
            if(!value(depth+1))return false;
            ws();
            if(take(close))return true;
            if(!take(','))return false;
            ws();
        }
    }
};
Result parse(Scan &s,JsonDocument &doc,char *row,size_t cap,Batch &out,const ParseOptions &options){
    s.ws();if(!s.take('{'))return Result::Invalid;
    bool found=false;s.ws();if(s.take('}'))return Result::Invalid;
    for(;;){
        // Decode escaped top-level keys without retaining their enclosing object.
        char key[256];s.capture=key;s.capacity=sizeof key;s.size=0;
        if(!s.string())return s.error==Result::Ok?Result::Invalid:s.error;
        s.capture=nullptr;
        auto err=deserializeJson(doc,static_cast<const char *>(key),s.size);
        if(err)return err==DeserializationError::NoMemory?Result::NoMemory:Result::Invalid;
        bool selected=doc.is<const char*>()&&doc.as<JsonString>().size()==std::strlen(doc.as<const char*>())&&
            (!std::strcmp(doc.as<const char*>(),"ac")||!std::strcmp(doc.as<const char*>(),"aircraft"));
        s.ws();if(!s.take(':'))return Result::Invalid;s.ws();
        if(selected){
            if(found||!s.take('['))return Result::Invalid;
            found=true;s.ws();
            if(!s.take(']'))for(;;){
                s.capture=row;s.capacity=cap;s.size=0;
                if(!s.value(2))return s.error==Result::Ok?Result::Invalid:s.error;
                s.capture=nullptr;
                // The row is scratch owned by this call. Zero-copy string
                // parsing avoids retaining a second copy during TLS reads.
                err=deserializeJson(doc,row,s.size,DeserializationOption::NestingLimit(10));
                if(err)return err==DeserializationError::NoMemory?Result::NoMemory:Result::Invalid;
                if(append_adsb_aircraft(doc.as<JsonVariantConst>(),out,options)!=Result::Ok)return Result::Invalid;
                s.ws();if(s.take(']'))break;if(!s.take(','))return Result::Invalid;s.ws();
            }
        }else if(!s.value(1))return s.error==Result::Ok?Result::Invalid:s.error;
        s.ws();if(s.take('}'))break;if(!s.take(','))return Result::Invalid;s.ws();
    }
    s.ws();if(s.error!=Result::Ok)return s.error;
    return found&&s.peek()==-1?Result::Ok:Result::Invalid;
}
}
Result decode_adsb_stream(BodyReader source,JsonDocument &doc,char *row,size_t cap,
                          Batch &out,const ParseOptions &options){
    reset(out,Provider::AdsbFi);doc.clear();
    if(!source.read||!row||!cap||!std::isfinite(options.max_position_age_seconds)||options.max_position_age_seconds<0)return Result::Invalid;
    Scan scan{source};const Result result=parse(scan,doc,row,cap,out,options);doc.clear();
    if(result!=Result::Ok)reset(out,Provider::AdsbFi);
    return result;
}
}
