#include <iostream>
#include <vector>
using namespace std;

int main() {
    vector<vector<int>> matrix = {
        {1, 2, 3},
        {4, 5, 6},
        {7, 8, 9}
    };

    for (const auto &row : matrix) {
        for (const auto &val : row) {
            cout << val << " ";
        }
        cout << endl;
    }
    return 0;
}