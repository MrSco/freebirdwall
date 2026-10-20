#include "../esp32/http_body.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
#include <string>
#include <iostream>
using namespace flightwall;
struct Source {std::string bytes;size_t at=0,fragment=1;static int read(void *p,uint8_t *out,size_t cap){auto &s=*static_cast<Source *>(p);size_t n=s.bytes.size()-s.at;if(n>cap)n=cap;if(n>s.fragment)n=s.fragment;std::memcpy(out,s.bytes.data()+s.at,n);s.at+=n;return static_cast<int>(n);}};
int main(){
    const std::string fixed="HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: 9\r\n\r\n{\"ac\":[]}";
    const std::string chunked="HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4;foo=bar\r\n{\"ac\r\n5\r\n\":[]}\r\n0\r\nX-Trailer: yes\r\n\r\n";
    for(size_t fragment=1;fragment<=fixed.size();++fragment)for(const auto &response:{fixed,chunked}){
        Source s{response,0,fragment};HttpBody body({&s,Source::read});assert(body.headers()&&body.status()==200);
        std::string result;int c;while((c=body.read())>=0)result+=static_cast<char>(c);
        assert(c==-1&&result=="{\"ac\":[]}"&&body.bytes()==9&&body.read()==-1);
    }
    for(size_t n=0;n<fixed.size();++n){Source s{fixed.substr(0,n)};HttpBody body({&s,Source::read});if(body.headers()){int c;while((c=body.read())>=0){}assert(c==-2);}}
    for(const auto bad:{"Content-Length: 1\r\nContent-Length: 1", "Content-Length: 1\r\nTransfer-Encoding: chunked", "Content-Encoding: gzip", "Content-Length: 131073", "Transfer-Encoding: gzip, chunked", "Content-Type: text/html"}){
        Source s{std::string("HTTP/1.1 200 OK\r\n")+bad+"\r\n\r\nx"};HttpBody body({&s,Source::read});assert(!body.headers());
    }
    for(const auto bad:{"1\r\nxX\r\n0\r\n\r\n", "0\r\n", "Z\r\nx\r\n0\r\n\r\n", "20001\r\n"}){
        Source s{std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n")+bad};HttpBody body({&s,Source::read});assert(body.headers());int c;while((c=body.read())>=0){}assert(c==-2);
    }
    Source retry{"HTTP/1.1 429 Too Many Requests\r\nRetry-After: 120\r\nContent-Length: 0\r\n\r\n"};HttpBody body({&retry,Source::read});assert(body.headers()&&body.status()==429&&body.retry_after()==120&&body.read()==-1);
    Source closed{"HTTP/1.0 200 OK\r\n\r\n{}"};HttpBody b({&closed,Source::read});assert(b.headers()&&b.read()=='{'&&b.read()=='}'&&b.read()==-1);
    std::cout<<"HTTP body: all fragment sizes, fixed/chunked/close framing, truncation, compression/conflict/size rejection and Retry-After passed\n";
}
