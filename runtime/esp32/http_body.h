#pragma once
#include <stddef.h>
#include <stdint.h>
namespace flightwall {
// Transport returns bytes, zero for orderly EOF, negative for IO/timeout. It
// owns TLS/deadline checks; this parser owns HTTP framing and byte limits.
struct HttpTransport { void *user;int (*read)(void *,uint8_t *,size_t); };
class HttpBody {
public:
    explicit HttpBody(HttpTransport source):source_(source){}
    bool headers();
    int read(); // byte, -1 complete body, -2 malformed/truncated/oversized/IO
    unsigned status() const {return status_;}
    uint32_t retry_after() const {return retry_after_;}
    size_t bytes() const {return total_;}
private:
    HttpTransport source_;uint8_t buffer_[512];size_t at_=0,end_=0,total_=0,headers_=0;
    size_t remaining_=0,chunk_=0;unsigned status_=0;uint32_t retry_after_=0;
    bool length_=false,chunked_=false,done_=false,failed_=false,chunk_crlf_=false,ready_=false;
    int raw();bool line(char *,size_t);bool next_chunk();
};
}
