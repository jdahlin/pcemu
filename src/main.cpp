#include "pc_session.hpp"
int main(int argc, char **argv) {
    return pc::runPcSessionCommandLine(argc, argv, pc::createFrontend);
}
