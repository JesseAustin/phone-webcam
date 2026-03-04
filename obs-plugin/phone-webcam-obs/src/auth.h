#pragma once
// auth.h — Shared-secret HMAC-SHA256 authentication
//
// Zero external dependencies: SHA-256 is implemented inline below.
// Both OBS (C++) and Android (Kotlin) use the same algorithm:
//
//   HMAC-SHA256( UTF-8(password), challenge_bytes ) -> 32-byte MAC
//
// If both sides know the same password, the MACs match.
// An empty password means "no auth" — all connections are accepted.

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <array>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#endif

namespace Auth {

    namespace detail {

    static constexpr uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
        0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
        0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
        0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
        0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
        0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
        0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
        0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
        0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };

    inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    inline uint32_t ch (uint32_t x,uint32_t y,uint32_t z) { return (x&y)^(~x&z); }
    inline uint32_t maj(uint32_t x,uint32_t y,uint32_t z) { return (x&y)^(x&z)^(y&z); }
    inline uint32_t sig0(uint32_t x) { return rotr(x,2)^rotr(x,13)^rotr(x,22); }
    inline uint32_t sig1(uint32_t x) { return rotr(x,6)^rotr(x,11)^rotr(x,25); }
    inline uint32_t gam0(uint32_t x) { return rotr(x,7)^rotr(x,18)^(x>>3); }
    inline uint32_t gam1(uint32_t x) { return rotr(x,17)^rotr(x,19)^(x>>10); }

    struct Sha256Ctx {
        uint32_t state[8];
        uint64_t count;
        uint8_t  buf[64];
    };

    inline void sha256_init(Sha256Ctx& c) {
        c.state[0]=0x6a09e667; c.state[1]=0xbb67ae85;
        c.state[2]=0x3c6ef372; c.state[3]=0xa54ff53a;
        c.state[4]=0x510e527f; c.state[5]=0x9b05688c;
        c.state[6]=0x1f83d9ab; c.state[7]=0x5be0cd19;
        c.count=0;
        memset(c.buf,0,sizeof(c.buf));
    }

    inline void sha256_transform(Sha256Ctx& c, const uint8_t* block) {
        uint32_t w[64],a,b,cc,d,e,f,g,h,t1,t2;
        for(int i=0;i<16;i++)
            w[i]=(uint32_t)block[i*4]<<24|(uint32_t)block[i*4+1]<<16
                |(uint32_t)block[i*4+2]<<8|(uint32_t)block[i*4+3];
        for(int i=16;i<64;i++)
            w[i]=gam1(w[i-2])+w[i-7]+gam0(w[i-15])+w[i-16];
        a=c.state[0];b=c.state[1];cc=c.state[2];d=c.state[3];
        e=c.state[4];f=c.state[5];g=c.state[6];h=c.state[7];
        for(int i=0;i<64;i++){
            t1=h+sig1(e)+ch(e,f,g)+K[i]+w[i];
            t2=sig0(a)+maj(a,b,cc);
            h=g;g=f;f=e;e=d+t1;d=cc;cc=b;b=a;a=t1+t2;
        }
        c.state[0]+=a;c.state[1]+=b;c.state[2]+=cc;c.state[3]+=d;
        c.state[4]+=e;c.state[5]+=f;c.state[6]+=g;c.state[7]+=h;
    }

    inline void sha256_update(Sha256Ctx& c, const uint8_t* data, size_t len) {
        size_t used = c.count & 63;
        c.count += len;
        if(used){
            size_t free = 64-used;
            if(len<free){ memcpy(c.buf+used,data,len); return; }
            memcpy(c.buf+used,data,free);
            sha256_transform(c,c.buf);
            data+=free; len-=free;
        }
        while(len>=64){ sha256_transform(c,data); data+=64; len-=64; }
        memcpy(c.buf,data,len);
    }

    inline void sha256_final(Sha256Ctx& c, uint8_t out[32]) {
        size_t used=c.count&63;
        c.buf[used++]=0x80;
        if(used>56){ memset(c.buf+used,0,64-used); sha256_transform(c,c.buf); used=0; }
        memset(c.buf+used,0,56-used);
        uint64_t bc=c.count*8;
        c.buf[56]=(bc>>56)&0xFF; c.buf[57]=(bc>>48)&0xFF;
        c.buf[58]=(bc>>40)&0xFF; c.buf[59]=(bc>>32)&0xFF;
        c.buf[60]=(bc>>24)&0xFF; c.buf[61]=(bc>>16)&0xFF;
        c.buf[62]=(bc>>8)&0xFF;  c.buf[63]= bc&0xFF;
        sha256_transform(c,c.buf);
        for(int i=0;i<8;i++){
            out[i*4]  =(c.state[i]>>24)&0xFF; out[i*4+1]=(c.state[i]>>16)&0xFF;
            out[i*4+2]=(c.state[i]>>8)&0xFF;  out[i*4+3]= c.state[i]&0xFF;
        }
    }

    } // namespace detail

    // ---------------------------------------------------------------------------
    // Public API
    // ---------------------------------------------------------------------------

    inline std::array<uint8_t,32> hmac_sha256(
        const std::string& key,
        const std::vector<uint8_t>& message)
    {
        uint8_t kpad[64] = {};
        if(key.size() > 64){
            detail::Sha256Ctx kc; detail::sha256_init(kc);
            detail::sha256_update(kc,(const uint8_t*)key.data(),key.size());
            detail::sha256_final(kc,kpad);
        } else {
            memcpy(kpad,key.data(),key.size());
        }
        uint8_t opad[64], ipad[64];
        for(int i=0;i<64;i++){ opad[i]=kpad[i]^0x5c; ipad[i]=kpad[i]^0x36; }

        uint8_t inner[32];
        detail::Sha256Ctx ic; detail::sha256_init(ic);
        detail::sha256_update(ic,ipad,64);
        detail::sha256_update(ic,message.data(),message.size());
        detail::sha256_final(ic,inner);

        std::array<uint8_t,32> out;
        detail::Sha256Ctx oc; detail::sha256_init(oc);
        detail::sha256_update(oc,opad,64);
        detail::sha256_update(oc,inner,32);
        detail::sha256_final(oc,out.data());
        return out;
    }

    inline std::string toHex(const std::array<uint8_t,32>& b) {
        static const char* h="0123456789abcdef";
        std::string s; s.reserve(64);
        for(auto x:b){ s+=h[x>>4]; s+=h[x&0xF]; }
        return s;
    }

    inline std::vector<uint8_t> fromHex(const std::string& s) {
        if(s.size()%2!=0) return {};
        std::vector<uint8_t> out; out.reserve(s.size()/2);
        for(size_t i=0;i<s.size();i+=2){
            auto c=[](char x)->int{
                if(x>='0'&&x<='9') return x-'0';
                if(x>='a'&&x<='f') return x-'a'+10;
                if(x>='A'&&x<='F') return x-'A'+10;
                return -1;
            };
            int hi=c(s[i]),lo=c(s[i+1]);
            if(hi<0||lo<0) return {};
            out.push_back((uint8_t)((hi<<4)|lo));
        }
        return out;
    }

    inline bool constTimeEqual(const std::string& a, const std::string& b) {
        if(a.size()!=b.size()) return false;
        uint8_t diff=0;
        for(size_t i=0;i<a.size();i++) diff|=(uint8_t)a[i]^(uint8_t)b[i];
        return diff==0;
    }

    // ---------------------------------------------------------------------------
    // Random challenge generator
    // ---------------------------------------------------------------------------
    inline std::vector<uint8_t> generateChallenge() {
        std::vector<uint8_t> ch(32);
    #ifdef _WIN32
        HCRYPTPROV hProv = 0;
        if(CryptAcquireContext(&hProv,nullptr,nullptr,PROV_RSA_FULL,CRYPT_VERIFYCONTEXT)){
            CryptGenRandom(hProv,32,ch.data());
            CryptReleaseContext(hProv,0);
        } else {
            for(auto& b:ch) b=(uint8_t)(rand()&0xFF);
        }
    #else
        FILE* f=fopen("/dev/urandom","rb");
        if(f){ fread(ch.data(),1,32,f); fclose(f); }
        else  { for(auto& b:ch) b=(uint8_t)(rand()&0xFF); }
    #endif
        return ch;
    }

} // namespace Auth