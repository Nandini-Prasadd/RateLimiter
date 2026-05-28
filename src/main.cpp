#include "storage/InMemoryStore.hpp"
#include <iostream>

int main() {
    InMemoryStore store;
    std::string user = "test";

    for (int i = 0; i < 20; i++) {
        TokenBucket& bucket = store.getBucket(user);
        bool allowed = bucket.allow();

        std::cout << "Request " << i << " "
                  << (allowed ? "allowed" : "blocked") << "\n";
    }

    return 0;
}
