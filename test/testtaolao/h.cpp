#include <iostream>

const char* a = "Nguyen Van A";

void hehe(&a) {
    const char* b = "Nguyen Van B";
    a = b;
    std::cout << a << std::endl;  
}

int main() {
    hehe(&a);
    std::cout << a << std::endl; // in lại để thấy thay đổi
    return 0;
}