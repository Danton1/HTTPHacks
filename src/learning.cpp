/*
    This file is used for learning C++ and testing knowledge. A simple sandbox to help assist development
*/

#include <iostream>
#include <string>
// Important library, already exists, std library, reduces written code, imports every function call 
using namespace std; 
// << similar to a print function

int main() {
    string input;
    cout << "Hello World!" << endl;
    
    cout << "Type Input: ";
    cin >> input;
    cout << input;

    return 0;
}