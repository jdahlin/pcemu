#include "state_archive.hpp"
#include <iostream>
using namespace pc;
int main() {
    try {
        std::map<uint32_t,std::deque<std::vector<uint8_t>>> queues{{7,{{1,2,3},{4,5}}}};
        std::optional<std::u16string> text=u"checkpoint";
        std::vector<bool> flags{true,false,true};
        StateArchive output; output(queues,text,flags); auto bytes=output.take();
        auto expected=queues;
        queues.clear(); text.reset(); flags.clear();
        StateArchive input(bytes); input(queues,text,flags); input.finish();
        if (queues!=expected || text!=u"checkpoint" || flags!=std::vector<bool>{true,false,true}) return 1;
        for (auto corrupt : {std::vector<uint8_t>{},std::vector<uint8_t>(8,0xff)}) {
            bool rejected=false;
            try { StateArchive bad(corrupt); bad(queues); } catch (...) { rejected=true; }
            if (!rejected) return 1;
        }
        bool flag=false, rejected=false;
        try { const std::array<uint8_t,1> invalid{2}; StateArchive bad(invalid); bad(flag); } catch (...) { rejected=true; }
        if (!rejected) return 1;
        std::cout<<"Snapshot collections, bounds, truncation and boolean validation passed\n"; return 0;
    } catch (...) { return 1; }
}
