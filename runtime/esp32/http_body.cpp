#include "http_body.h"
#include "../src/flight_data.h"
#include <cstring>
namespace flightwall {
namespace {
bool equal(const char *a,const char *b){for(;*a&&*b;++a,++b){char c=*a,d=*b;if(c>='A'&&c<='Z')c+=32;if(d>='A'&&d<='Z')d+=32;if(c!=d)return false;}return !*a&&!*b;}
bool decimal(const char *p,size_t &n){n=0;if(!*p)return false;for(;*p;++p){if(*p<'0'||*p>'9'||n>(MaxResponseBytes-static_cast<unsigned>(*p-'0'))/10)return false;n=n*10+*p-'0';}return true;}
}
int HttpBody::raw(){
    if(at_==end_){int n=source_.read?source_.read(source_.user,buffer_,sizeof buffer_):-1;
        if(n<0||n>static_cast<int>(sizeof buffer_)){failed_=true;return -2;}
        if(!n)return -1;
        at_=0;end_=static_cast<size_t>(n);
    }return buffer_[at_++];
}
bool HttpBody::line(char *out,size_t cap){
    size_t n=0;
    for(;;){int c=raw();if(c<0||++headers_>8192)return false;
        if(c=='\r'){if(raw()!='\n'||++headers_>8192)return false;out[n]=0;return true;}
        if(c=='\n'||c<32||c>126||n+1>=cap)return false;
        out[n++]=static_cast<char>(c);
    }
}
bool HttpBody::headers(){
    char text[1024];if(ready_||!line(text,sizeof text))return false;
    if(std::strncmp(text,"HTTP/1.1 ",9)&&std::strncmp(text,"HTTP/1.0 ",9))return false;
    if(std::strlen(text)<12||text[9]<'1'||text[9]>'5'||text[10]<'0'||text[10]>'9'||text[11]<'0'||text[11]>'9'||(text[12]&&text[12]!=' '))return false;
    status_=static_cast<unsigned>((text[9]-'0')*100+(text[10]-'0')*10+text[11]-'0');
    bool encoding=false,transfer=false,type=false;
    while(line(text,sizeof text)){
        if(!text[0]){if(chunked_&&length_)return false;ready_=true;return true;}
        char *colon=std::strchr(text,':');if(!colon||colon==text)return false;
        for(char *p=text;p<colon;++p)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='-'))return false;
        *colon=0;char *value=colon+1;while(*value==' ')++value;
        size_t n=std::strlen(value);while(n&&value[n-1]==' ')value[--n]=0;
        if(equal(text,"content-length")){if(length_||!decimal(value,remaining_))return false;length_=true;}
        else if(equal(text,"transfer-encoding")){if(transfer||!equal(value,"chunked"))return false;transfer=chunked_=true;}
        else if(equal(text,"content-encoding")){if(encoding||!equal(value,"identity"))return false;encoding=true;}
        else if(equal(text,"content-type")){
            if(type)return false;
            type=true;
            char *semi=std::strchr(value,';');if(semi)*semi=0;
            if(status_==200&&!equal(value,"application/json"))return false;
        }else if(equal(text,"retry-after")){size_t seconds=0;if(decimal(value,seconds))retry_after_=static_cast<uint32_t>(seconds);}
    }return false;
}
bool HttpBody::next_chunk(){
    char text[1024];if(chunk_crlf_&&(raw()!='\r'||raw()!='\n'))return false;
    if(!line(text,sizeof text))return false;
    size_t size=0;unsigned digits=0;char *p=text;
    for(;*p&&*p!=';';++p){unsigned d=*p>='0'&&*p<='9'?*p-'0':*p>='a'&&*p<='f'?*p-'a'+10:*p>='A'&&*p<='F'?*p-'A'+10:16;
        if(d==16||++digits>8||size>(MaxResponseBytes-d)/16)return false;
        size=size*16+d;
    }
    if(!digits||size>MaxResponseBytes-total_)return false;
    if(!size){while(line(text,sizeof text)){if(!text[0]){done_=true;return true;}if(!std::strchr(text,':'))return false;}return false;}
    chunk_=size;chunk_crlf_=true;return true;
}
int HttpBody::read(){
    if(!ready_||failed_)return -2;
    if(done_)return -1;
    if(chunked_&&!chunk_){if(!next_chunk()){failed_=true;return -2;}if(done_)return -1;}
    if(length_&&!remaining_){done_=true;return -1;}
    int c=raw();
    if(c<0){if(c==-1&&!chunked_&&!length_){done_=true;return -1;}failed_=true;return -2;}
    if(++total_>MaxResponseBytes){failed_=true;return -2;}
    if(chunked_)--chunk_;
    if(length_)--remaining_;
    return c;
}
}
