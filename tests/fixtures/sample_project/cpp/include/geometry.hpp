#pragma once
#include <vector>

namespace geo {

struct Point {
    double x = 0, y = 0;
};

class Polygon {
public:
    explicit Polygon(std::vector<Point> pts);
    double area() const;
    double perimeter() const;
    std::size_t size() const { return points_.size(); }

private:
    std::vector<Point> points_;
};

double distance(const Point& a, const Point& b);

}  // namespace geo
