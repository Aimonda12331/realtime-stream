//g++ -std=c++11 range_based_for_loop.cpp -o range_based_for_loop
#include <vector>
#include <iostream>

int main() {
    std::vector<int> buffers = {1, 2, 3};

    // Dấu ':' nghĩa là lấy từng phần từ trong buffer
    for (const auto &buf : buffers) {
        std::cout << buf << " " << std::endl;
    }
    return 0;
}