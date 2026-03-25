#include <iostream>
#include <algorithm>
#include <vector>

    //Lambda đơn giản
auto f = []() {
    std::cout << ("Hello\n");
};

int main() {
    f();
    return 0;
}