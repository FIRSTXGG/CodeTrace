#include "geometry.hpp"

#include <cmath>

namespace geo {

Polygon::Polygon(std::vector<Point> pts) : points_(std::move(pts)) {}

double Polygon::area() const {
    double sum = 0;
    for (std::size_t i = 0; i < size(); ++i) {
        const Point& a = points_[i];
        const Point& b = points_[(i + 1) % size()];
        sum += a.x * b.y - b.x * a.y;
    }
    return std::abs(sum) / 2.0;
}

double Polygon::perimeter() const {
    double total = 0;
    for (std::size_t i = 0; i < size(); ++i) total += distance(points_[i], points_[(i + 1) % size()]);
    return total;
}

double distance(const Point& a, const Point& b) { return std::hypot(a.x - b.x, a.y - b.y); }

}  // namespace geo
