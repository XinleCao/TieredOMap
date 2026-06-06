// ORAM Storage Server — holds encrypted binary tree storage for remote clients.
// Usage: ./oram_server [--port=12345] [--setup_cache_dir=path] [--disk_data_dir=path]

#include "tiered_omap/network/storage_server.h"
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    int port = 12345;
    std::string setup_cache_dir;
    std::string disk_data_dir;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto eq = a.find('=');
        if (eq != std::string::npos) {
            auto k = a.substr(0, eq), v = a.substr(eq + 1);
            if (k == "--port") port = std::stoi(v);
            else if (k == "--setup_cache_dir") setup_cache_dir = v;
            else if (k == "--disk_data_dir") disk_data_dir = v;
        }
    }

    std::cerr << "ORAM Storage Server starting on port " << port << "\n";
    tiered_omap::StorageServer server(port, setup_cache_dir, disk_data_dir);
    server.run();
    return 0;
}
