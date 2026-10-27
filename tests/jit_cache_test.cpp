#include "jit.hpp"
#include <algorithm>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace pc;
static unsigned checks;
static void check(bool v,const char*m){++checks;if(!v)throw std::runtime_error(m);}
static void equal(const CPU&a,const CPU&b){
    check(a.ip==b.ip && a.instructions==b.instructions && a.flags==b.flags,"cache prefix control");
    check(std::equal(a.r,a.r+8,b.r),"cache GPR state");
    check(std::equal(a.fp,a.fp+8,b.fp) && a.fpTag==b.fpTag && a.fpStatus==b.fpStatus,"relocated FP state");
    check(a.mem==b.mem,"relocated memory effects");
}
int main(int argc,char **argv){try{
    CPU probe(65536);Jit available(probe);if(!available.enabled()){std::cout<<"Skipped: native JIT unavailable\n";return 0;}
    for(bool links:{false,true})for(bool mapped:{false,true})for(bool paging:{false,true})for(auto mode:{Jit::BlockMode::Basic,Jit::BlockMode::FollowJumps,Jit::BlockMode::Trace}) {
        CPU c(1024*1024);c.cr[0]=0x11;c.s[CS].big=true;c.s[DS].limit=0xffffffff;
        c.write(0x80000,0x3fc00000,4);
        // Many unique bodies fill a 64 KiB arena. The hot entry mixes native
        // FP and guarded memory; a second hot entry owns a deferred descriptor.
        for(unsigned n=0;n<200;++n){
            const uint8_t code[]={0xb8,uint8_t(n),0,0,0,0xd9,5,0,0,8,0,
                0xd8,0x0d,0,0,8,0,0xd9,0x1d,4,0,8,0,0x40,0xeb,4};
            const auto address=0x1000+n*256;
            std::copy(std::begin(code),std::end(code),c.mem.begin()+address);
            c.mem[address+sizeof(code)+4]=0x42;
            c.mem[address+sizeof(code)+5]=0xeb;c.mem[address+sizeof(code)+6]=0;
            c.mem[address+sizeof(code)+7]=0xf4;
        }
        c.mem[0x100]=0xd9;c.mem[0x101]=0xfe; // FSIN decoded boundary helper.
        if(paging){
            c.write(0x90000,0x91007,4);
            for(unsigned i=0;i<256;++i)c.write(0x91000+i*4,(i<<12)|7,4);
            c.cr[0]|=0x80000000;c.cr[3]=0x90000;c.flushTLB();
        }
        auto ref=c;Jit jit(c,true,0,65536,mode);jit.setDirectLinks(links,0);
        const auto map=(argc>1?std::filesystem::path(argv[1]):std::filesystem::temp_directory_path())/ ("pcemu-cache-"+std::to_string(getpid())+"-"+std::to_string(links)+"-"+std::to_string(mapped)+"-"+std::to_string(paging)+"-"+std::to_string(unsigned(mode))+".jsonl");
        if(mapped)jit.setDebugMap(map.string());
        auto run=[&](unsigned ip){c.ip=ref.ip=ip;auto n=jit.run();for(unsigned j=0;j<n;++j)ref.step();equal(c,ref);};
        run(0x1000);run(0x100);
        for(unsigned n=1;n<200;++n){
            for(unsigned k=0;k<12;++k)run(0x1000);
            for(unsigned k=0;k<4;++k)run(0x100);
            run(0x1000+n*256);
        }
        check(jit.cacheRecycles>1,"test must recycle native addresses");
        if(!links){
            check(jit.cachedCodeBytes()<=2*65536,"image cache is bounded");
            check(jit.cacheRestoredBlocks>0 && jit.cacheRestoredBytes>0,"unlinked bodies restore after collection");
            auto compiled=jit.compiledBlocks;run(0x1000);run(0x100);
            check(jit.compiledBlocks==compiled,"hot native and deferred bodies were unnecessarily recompiled");
        }else check(jit.cacheRestoredBlocks==0,"linked bodies use full reset");
        if(!links){
            // A genuine write to ANOTHER translated body on the same page must
            // not force this unchanged body's recompilation. Same-value writes
            // also change the page generation without changing machine code.
            run(0x1100);auto compiled=jit.compiledBlocks;
            c.write(0x1101,97,1);ref.write(0x1101,97,1);run(0x1000);
            check(jit.compiledBlocks==compiled && jit.cacheValidatedBlocks>0,"unchanged page neighbor was recompiled");
            c.write(0x1001,c.mem[0x1001],1);ref.write(0x1001,ref.mem[0x1001],1);run(0x1000);
            check(jit.compiledBlocks==compiled,"same-value code store was recompiled");
            check(jit.cacheImageEvictions>0,"finite image cache must exercise eviction");
        }
        if(!mapped && !links)jit.setDebugMap(map.string()); // Late enable: cached images have no region table.
        // Mutate a retained body's guest page, then its constant operand. Code
        // generation guards must reject stale moved instructions and metadata.
        c.write(0x1001,123,1);ref.write(0x1001,123,1);run(0x1000);
        check(c.r[AX]==124,"retained code generation invalidated");
        c.watchWrite=[](uint32_t,uint32_t,int){};ref.watchWrite=c.watchWrite;run(0x1000);
        if(mapped || !links){
            jit.flushDebugMap(); // Further recycling must also republish maps after an explicit flush.
            for(unsigned n=1;n<100;++n)run(0x1000+n*256);
            jit.flushDebugMap();std::ifstream stream(map);std::string line;unsigned resets=0,loads=0;
            while(std::getline(stream,line)){resets+=line.find("\"event\":\"reset\"")!=std::string::npos;loads+=line.find("\"event\":\"load\"")!=std::string::npos;}
            check(resets>1 && loads>100,"relocation maps retain address lifetimes");
            if(argc==1)std::filesystem::remove(map);
        }
        jit.clear();check(jit.residentCodeBytes()==0 && jit.cacheRestoredBlocks==0 && jit.cachedCodeBytes()==0,"clear releases cached code state");
    }
    std::cout<<"Passed "<<checks<<" code cache checks\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
