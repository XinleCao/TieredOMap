// ORAM Storage Server — holds encrypted binary tree storage for remote clients.
// Usage: ./oram_server [--port=12345]

#include "tiered_omap/network/storage_server.h"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    int port = 12345;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto eq = a.find('=');
        if (eq != std::string::npos) {
            auto k = a.substr(0, eq), v = a.substr(eq + 1);
            if (k == "--port") port = std::stoi(v);
        }
    }

    std::cerr << "ORAM Storage Server starting on port " << port << "\n";
    tiered_omap::StorageServer server(port);
    server.run();
    return 0;
}
