#include "geometry.hpp"

#include <iostream>

static void report(const geo::Polygon& p) {
    std::cout << "area=" << p.area() << " perimeter=" << p.perimeter() << "\n";
}

int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

int main() {
    geo::Polygon square({{0, 0}, {1, 0}, {1, 1}, {0, 1}});
    report(square);
    std::cout << fib(10) << "\n";
    return 0;
}
